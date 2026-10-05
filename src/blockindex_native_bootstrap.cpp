// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "blockindex_native_bootstrap.h"

#include "blockindex_generation_builder.h"
#include "blockindex_generation_lifecycle.h"
#include "fixed_blockindex_store.h"

#include "main.h"
#include "kernel.h"
// GetGlobalTxdbPtrForTest is a test/ops accessor defined in txdb-leveldb.cpp
// (declared extern at its other call sites, not in a public header).
extern void* GetGlobalTxdbPtrForTest();

#include <boost/filesystem.hpp>

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
        if (sscanf(name.c_str(), "blockindex-gen-%llu", (unsigned long long*)&id) == 1 ||
            sscanf(name.c_str(), "blockindex-build-%llu", (unsigned long long*)&id) == 1)
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
                                std::string* error)
{
    if (dataDir.empty() || snapshotDir.empty())
        return SetError(error, "snapshot: empty dataDir/snapshotDir");

    // Quiescence guard: a copy taken while the process-global legacy LevelDB is
    // open is not guaranteed to be a consistent point-in-time snapshot. Fail
    // closed rather than produce a torn migration source.
    if (GetGlobalTxdbPtrForTest() != NULL)
        return SetError(error, "snapshot: legacy LevelDB is open; refusing non-consistent migration source");

    fs::path src = fs::path(dataDir) / "txleveldb";
    boost::system::error_code ec;
    if (!fs::is_directory(src, ec) || ec)
        return SetError(error, "snapshot: legacy txleveldb directory absent");

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
                fs::copy_file(it->path(), target, fs::copy_option::overwrite_if_exists);
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
    std::string* error)
{
    if (v2Root.empty() || dataDir.empty())
    {
        SetError(error, "native prepare: empty v2Root/dataDir");
        return BLOCK_INDEX_NATIVE_PREPARE_FAILED;
    }

    boost::system::error_code ec;
    fs::create_directories(v2Root, ec);

    const uint64_t gen = NextGenerationId(v2Root);
    const std::string staging = BlockIndexGenerationManager::StagingPath(v2Root, gen);

    BlockIndexGenerationSource source;
    std::string snapDir = (fs::path(v2Root) / "blockindex-migsrc.tmp").string();
    bool haveSnap = false;

    if (state == BLOCK_INDEX_STARTUP_OWNERSHIP_EMPTY_NEW)
    {
        if (!CreateGenesisBlockIndexSource(dataDir, &source, error))
            return BLOCK_INDEX_NATIVE_PREPARE_FAILED;
    }
    else if (state == BLOCK_INDEX_STARTUP_OWNERSHIP_LEGACY_MIGRATION_REQUIRED)
    {
        if (!SnapshotLegacyBlockIndexDb(dataDir, snapDir, error))
            return BLOCK_INDEX_NATIVE_PREPARE_FAILED;
        haveSnap = true;
        if (!ReadLegacyBlockIndexSource(snapDir, &source, error))
        {
            fs::remove_all(snapDir, ec);
            return BLOCK_INDEX_NATIVE_PREPARE_FAILED;
        }
        // Exact nSize from the LIVE datadir blk*.dat files (read-only).
        source.blockDataDir = dataDir;
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
        BlockIndexGenerationBuilder builder;
        BlockIndexGenerationStats stats;
        ok = builder.Build(source, staging, gen, &stats, error);
        builder.Close();

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
