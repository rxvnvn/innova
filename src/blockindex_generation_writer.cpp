// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "blockindex_generation_writer.h"
#include "candidate_frontier_metadata.h"

#include "util.h"

#include <boost/filesystem.hpp>

#include <cstring>

namespace fs = boost::filesystem;

namespace {
static bool SetError(std::string* error, const std::string& message)
{
    if (error)
        *error = message;
    return false;
}
static void ClearError(std::string* error)
{
    if (error)
        error->clear();
}
} // namespace

BlockIndexGenerationWriter::BlockIndexGenerationWriter()
    : generation_(0), open_(false)
{
}

BlockIndexGenerationWriter::~BlockIndexGenerationWriter()
{
    Close();
}

bool BlockIndexGenerationWriter::OpenTarget(const std::string& stagingDir,
                                            uint64_t generation,
                                            std::string* error)
{
    Close();
    stagingDir_ = stagingDir;
    generation_ = generation;
    fs::path sdir(stagingDir_);
    boost::system::error_code ec;
    if (fs::exists(sdir))
        fs::remove_all(sdir, ec);
    if (!fs::create_directories(sdir, ec) && ec)
        return SetError(error, "writer: create staging dir failed");

    if (!FixedBlockIndexStore::Create(stagingDir_, generation, &store_, error))
        return false;
    if (!BlockIndexHashIndex::Create(stagingDir_, generation, &hashIndex_, error))
        return false;
    if (!BlockIndexActiveIndex::Create(stagingDir_, generation, &active_, error))
        return false;
    unsigned char binding[32];
    memset(binding, 0, 32);
    if (!BlockIndexDerivedStateStore::Create(stagingDir_, generation, binding,
                                             &derived_, error))
        return false;

    open_ = true;
    ClearError(error);
    return true;
}

bool BlockIndexGenerationWriter::AppendRecord(const BlockIndexRecord& rec,
                                              BlockIndexId* outId,
                                              std::string* error)
{
    if (!open_)
        return SetError(error, "writer not open");
    recordBatch_.push_back(rec);
    if (outId)
        // RecordId = physical count + 1 (sequential assignment matches store)
        *outId = store_.PhysicalRecordCount() + recordBatch_.size();
    if (recordBatch_.size() >= kBatchEntries)
    {
        std::vector<BlockIndexId> ids;
        if (!store_.AppendBatch(recordBatch_, &ids, error))
            return false;
        recordBatch_.clear();
    }
    return true;
}

bool BlockIndexGenerationWriter::AppendDerived(const BlockIndexDerivedEntry& entry,
                                               std::string* error)
{
    if (!open_)
        return SetError(error, "writer not open");
    derivedBatch_.push_back(entry);
    if (derivedBatch_.size() >= kBatchEntries)
    {
        if (!derived_.AppendBatch(derivedBatch_, error))
            return false;
        derivedBatch_.clear();
    }
    return true;
}

bool BlockIndexGenerationWriter::AppendActive(BlockIndexId id, int32_t height,
                                              std::string* error)
{
    if (!open_)
        return SetError(error, "writer not open");
    activeBatch_.push_back(id);
    // active.dat uses dense height via AppendBatch; heights are implicit by
    // batch order (must be appended ascending 0..tip).
    if (activeBatch_.size() >= kBatchEntries)
    {
        if (!active_.AppendBatch(activeBatch_, error))
            return false;
        activeBatch_.clear();
    }
    return true;
}

bool BlockIndexGenerationWriter::Flush(std::string* error)
{
    if (!open_)
        return SetError(error, "writer not open");
    if (!recordBatch_.empty())
    {
        std::vector<BlockIndexId> ids;
        if (!store_.AppendBatch(recordBatch_, &ids, error))
            return false;
        recordBatch_.clear();
    }
    if (!derivedBatch_.empty())
    {
        if (!derived_.AppendBatch(derivedBatch_, error))
            return false;
        derivedBatch_.clear();
    }
    if (!activeBatch_.empty())
    {
        if (!active_.AppendBatch(activeBatch_, error))
            return false;
        activeBatch_.clear();
    }
    return true;
}

bool BlockIndexGenerationWriter::PutHashIndex(const uint256& hash, BlockIndexId id,
                                              std::string* error)
{
    if (!open_)
        return SetError(error, "writer not open");
    return hashIndex_.Put(hash, id, error);
}

bool BlockIndexGenerationWriter::Finalize(uint64_t generation,
                                          const uint256& committedTipHash,
                                          BlockIndexId committedTipId,
                                          int32_t committedTipHeight,
                                          uint64_t recordCount,
                                          uint32_t generationCapability,
                                          const unsigned char* dagInputDigest,
                                          std::string* error)
{
    if (!Flush(error))
        return false;

    // content binding depends on capability:
    // - AUTHORITATIVE: full generation root recomputed from on-disk files
    //   (commits to records+active+hashindex+derived+DAG inputs), with the
    //   DAG input digest persisted in the MANIFEST for validation.
    // - AUTHORITATIVE_FRONTIER (R2c.1c): same authoritative full root AND the
    //   mandatory bound dag-tip-frontier.dat folded by RecomputeGenerationRootFromFiles
    //   (bindFrontier=true). A caller that declares this capability MUST have
    //   materialized the artifact in the staging dir first; otherwise the
    //   recompute fails closed and the writer never finalizes a frontier-capable
    //   generation without its artifact.
    // - OLD_SHADOW: metadata-only binding (tipHash || recordCount || gen).
    unsigned char binding[32];
    if (generationCapability == BLOCK_INDEX_GENERATION_CAPABILITY_AUTHORITATIVE ||
        generationCapability == BLOCK_INDEX_GENERATION_CAPABILITY_AUTHORITATIVE_FRONTIER)
    {
        unsigned char dagDigest[32];
        memset(dagDigest, 0, 32);
        const unsigned char* usedDag = dagInputDigest ? dagInputDigest : dagDigest;
        // Materialize candidate leaves before root computation so the writer and
        // validator bind the same immutable sidecar.
        FixedBlockIndexManifest preManifest = store_.GetManifest();
        preManifest.recordCount = recordCount;
        preManifest.committedTipId = committedTipId;
        preManifest.committedTipHeight = committedTipHeight;
        preManifest.committedTipHash = committedTipHash;
        preManifest.state = BLOCK_INDEX_MANIFEST_BUILDING;
        if (!store_.WriteManifest(preManifest, error))
            return SetError(error, "writer: pre-root MANIFEST write failed: " + (error ? *error : ""));
        if (!EnsureCandidateLeafMetadata(stagingDir_, generation, error))
            return SetError(error, "writer: candidate leaves materialization failed: " + (error ? *error : ""));
        unsigned char candidateBinding[32];
        if (!ComputeCandidateLeavesBinding(stagingDir_, generation, candidateBinding, error))
            return SetError(error, "writer: candidate leaves binding failed: " + (error ? *error : ""));
        // RecomputeGenerationRootFromFiles applies the canonical mix itself.
        boost::filesystem::path sdir(stagingDir_);
        if (!RecomputeGenerationRootFromFiles(sdir, generation, committedTipHash,
                                              recordCount, usedDag,
                                              generationCapability ==
                                                  BLOCK_INDEX_GENERATION_CAPABILITY_AUTHORITATIVE_FRONTIER,
                                              binding, error))
            return SetError(error, std::string("writer: recompute generation root failed: ") +
                                   (error ? *error : "unknown"));
    }
    else
    {
        if (!ComputeDerivedContentBinding(committedTipHash, recordCount, generation,
                                          binding))
            return SetError(error, "writer: compute derived content binding failed");
    }

    // The derived header contentBinding holds the generation root (AUTHORITATIVE)
    // or the shadow metadata binding.
    derived_.SetContentBinding(binding);
    if (!derived_.Finalize(error))
        return false;

    FixedBlockIndexManifest man = store_.GetManifest();
    man.state = BLOCK_INDEX_MANIFEST_COMPLETE;
    man.recordCount = recordCount;
    man.committedTipId = committedTipId;
    man.committedTipHeight = committedTipHeight;
    man.committedTipHash = committedTipHash;
    man.capability = generationCapability;
    if (generationCapability == BLOCK_INDEX_GENERATION_CAPABILITY_AUTHORITATIVE ||
        generationCapability == BLOCK_INDEX_GENERATION_CAPABILITY_AUTHORITATIVE_FRONTIER)
    {
        unsigned char dagDigest[32];
        memset(dagDigest, 0, 32);
        memcpy(man.dagInputDigest, dagInputDigest ? dagInputDigest : dagDigest, 32);
    }
    if (!store_.WriteManifest(man, error))
        return false;

    // Release LevelDB LOCK + FILE* so a downstream publish can re-open.
    hashIndex_.Close();
    open_ = false;
    ClearError(error);
    return true;
}

bool BlockIndexGenerationWriter::ComputeGenerationRootFromFiles(
    const std::string& stagingDir, uint64_t generation, unsigned char root[32],
    std::string* error)
{
    // A full recompute needs the persisted dagInputDigest. We require the caller
    // to have set it; simplest is to recompute from the current manifest's
    // dagInputDigest. For migration/migration-builder use the derived binding
    // path (Shadow) unless a true AUTHORITATIVE root is required (M6).
    (void)generation;
    (void)root;
    SetError(error, "writer: generation-root-from-files requires M6 (digest) wiring");
    return false;
}

bool BlockIndexGenerationWriter::ValidatePublishSelect(const std::string& root,
                                                       uint64_t generation,
                                                       std::string* error)
{
    BlockIndexLifecycleStatus p = BlockIndexGenerationManager::PublishGeneration(root, generation, error);
    if (p != BLOCK_INDEX_LIFECYCLE_OK)
        return SetError(error, "writer: publish failed (" + std::to_string(int(p)) + "): " + (error ? *error : ""));
    BlockIndexLifecycleStatus v = BlockIndexGenerationManager::ValidateGeneration(root, generation, error);
    if (v != BLOCK_INDEX_LIFECYCLE_OK)
        return SetError(error, "writer: validate failed (" + std::to_string(int(v)) + "): " + (error ? *error : ""));
    BlockIndexLifecycleStatus s = BlockIndexGenerationManager::SelectGeneration(root, generation, error);
    if (s != BLOCK_INDEX_LIFECYCLE_OK)
        return SetError(error, "writer: select failed (" + std::to_string(int(s)) + "): " + (error ? *error : ""));
    ClearError(error);
    return true;
}

void BlockIndexGenerationWriter::Close()
{
    hashIndex_.Close();
    open_ = false;
}

uint64_t BlockIndexGenerationWriter::RecordCount() const
{
    return open_ ? store_.PhysicalRecordCount() : 0;
}

bool BlockIndexGenerationWriter::IsOpen() const
{
    return open_;
}