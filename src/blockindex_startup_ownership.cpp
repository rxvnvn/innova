#include "blockindex_startup_ownership.h"

#include <boost/filesystem.hpp>

namespace fs = boost::filesystem;

const char* BlockIndexStartupOwnershipStateName(BlockIndexStartupOwnershipState state)
{
    switch (state)
    {
    case BLOCK_INDEX_STARTUP_OWNERSHIP_EMPTY_NEW: return "EMPTY_NEW";
    case BLOCK_INDEX_STARTUP_OWNERSHIP_LEGACY_MIGRATION_REQUIRED: return "LEGACY_MIGRATION_REQUIRED";
    case BLOCK_INDEX_STARTUP_OWNERSHIP_V2_AUTHORITATIVE: return "V2_AUTHORITATIVE";
    case BLOCK_INDEX_STARTUP_OWNERSHIP_V2_RECOVERY_REQUIRED: return "V2_RECOVERY_REQUIRED";
    case BLOCK_INDEX_STARTUP_OWNERSHIP_FATAL_CORRUPTION: return "FATAL_CORRUPTION";
    }
    return "UNKNOWN";
}

// Non-mutating probe: is there any V2 generation artifact (gen-N or build-N.tmp)
// under v2Root even though CURRENT is absent? Used only to distinguish an
// incomplete migration from a genuinely empty root. Never creates anything.
static bool HasUnpublishedGenerationArtifacts(const std::string& v2Root)
{
    if (v2Root.empty())
        return false;
    fs::path root(v2Root);
    boost::system::error_code ec;
    if (!fs::is_directory(root, ec) || ec)
        return false;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
    {
        std::string name = it->path().filename().string();
        if (name.compare(0, 15, "blockindex-gen-") == 0 || name.compare(0, 17, "blockindex-build-") == 0)
            return true;
    }
    return false;
}

// Non-mutating legacy presence probe. We deliberately only stat the directory
// and never open the LevelDB (opening can create storage). The legacy block
// index lives in <datadir>/txleveldb.
static bool LegacyBlockIndexPresent(const std::string& dataDir)
{
    if (dataDir.empty())
        return false;
    boost::system::error_code ec;
    fs::path legacy = fs::path(dataDir) / "txleveldb";
    return fs::is_directory(legacy, ec) && !ec;
}

bool ClassifyBlockIndexStartupOwnership(const std::string& v2Root,
                                        const std::string& dataDir,
                                        BlockIndexStartupOwnershipDecision* out,
                                        std::string* error)
{
    if (!out)
    {
        if (error) *error = "null decision output";
        return false;
    }
    if (error) error->clear();

    BlockIndexStartupOwnershipDecision d;
    d.legacyPresent = LegacyBlockIndexPresent(dataDir);
    d.legacyReadable = d.legacyPresent;
    d.unpublishedArtifacts = HasUnpublishedGenerationArtifacts(v2Root);

    // --- V2 side: the durable ownership marker is CURRENT. ---
    BlockIndexCurrentRecord cur;
    std::string curErr;
    BlockIndexLifecycleStatus st = BlockIndexGenerationManager::ReadCurrent(v2Root, &cur, &curErr);
    d.v2CurrentStatus = st;

    switch (st)
    {
    case BLOCK_INDEX_LIFECYCLE_OK:
        // CURRENT present + parseable: validate the selected generation
        // structurally (read-only) before declaring V2 authority.
        {
            uint64_t gen = 0;
            std::string openErr;
            BlockIndexLifecycleStatus os = BlockIndexGenerationManager::OpenCurrent(v2Root, &gen, &openErr);
            if (os == BLOCK_INDEX_LIFECYCLE_OK)
            {
                d.state = BLOCK_INDEX_STARTUP_OWNERSHIP_V2_AUTHORITATIVE;
                d.v2Valid = true;
                d.v2Generation = gen;
                d.detail = "CURRENT valid; selected generation validated";
            }
            else if (os == BLOCK_INDEX_LIFECYCLE_MISSING_GENERATION)
            {
                // Ownership was published (CURRENT exists) but the referenced
                // generation is gone. NO legacy fallback/downgrade.
                d.state = BLOCK_INDEX_STARTUP_OWNERSHIP_FATAL_CORRUPTION;
                d.v2Generation = cur.generation;
                d.detail = "CURRENT references a missing generation: " + openErr;
            }
            else
            {
                // Present-but-corrupt/incompatible selected generation.
                // Frozen protocol has no general deterministic recovery here.
                d.state = BLOCK_INDEX_STARTUP_OWNERSHIP_FATAL_CORRUPTION;
                d.v2Generation = cur.generation;
                d.detail = "CURRENT selected generation invalid: " + openErr;
            }
        }
        break;

    case BLOCK_INDEX_LIFECYCLE_NOT_PUBLISHED:
        // Ownership has NOT crossed the durable commit point.
        if (d.legacyPresent)
        {
            d.state = BLOCK_INDEX_STARTUP_OWNERSHIP_LEGACY_MIGRATION_REQUIRED;
            d.detail = d.unpublishedArtifacts
                           ? "CURRENT absent; unpublished generation artifacts present; legacy source -> retry migration"
                           : "CURRENT absent; legacy source present -> migration required";
        }
        else
        {
            d.state = BLOCK_INDEX_STARTUP_OWNERSHIP_EMPTY_NEW;
            d.detail = d.unpublishedArtifacts
                           ? "CURRENT absent; unpublished generation artifacts present; no legacy -> fresh bootstrap"
                           : "CURRENT absent; no legacy -> fresh datadir";
        }
        break;

    case BLOCK_INDEX_LIFECYCLE_CORRUPT:
        // CURRENT is the ownership marker: a malformed CURRENT must NOT be
        // treated as V2 absence. Fail closed (no legacy fallback).
        d.state = BLOCK_INDEX_STARTUP_OWNERSHIP_FATAL_CORRUPTION;
        d.detail = "CURRENT present but corrupt: " + curErr;
        break;

    case BLOCK_INDEX_LIFECYCLE_MISSING_GENERATION:
        d.state = BLOCK_INDEX_STARTUP_OWNERSHIP_FATAL_CORRUPTION;
        d.detail = "CURRENT references missing generation: " + curErr;
        break;

    case BLOCK_INDEX_LIFECYCLE_ERROR:
    default:
        d.state = BLOCK_INDEX_STARTUP_OWNERSHIP_FATAL_CORRUPTION;
        d.detail = "CURRENT probe error: " + curErr;
        break;
    }

    *out = d;
    if (error) *error = d.detail;
    return true;
}

std::string GetDefaultBlockIndexV2Root(const std::string& dataDir)
{
    // PM1-P0-07b (C1 filesystem contract): the authoritative block-index state
    // lives DIRECTLY under the effective Innova network datadir. The C1 lifecycle
    // files are already namespace-safe (blockindex-current, blockindex-gen-%06llu,
    // blockindex-build-%06llu.tmp), so the default root IS the effective network
    // datadir. It MUST NOT append a blockindex-v2/ (or blockindex/) container.
    if (dataDir.empty())
        return std::string();
    return dataDir;
}

std::string ResolveBlockIndexV2Root(const std::string& overrideRoot,
                                    const std::string& dataDir)
{
    if (!overrideRoot.empty())
        return overrideRoot;
    return GetDefaultBlockIndexV2Root(dataDir);
}
