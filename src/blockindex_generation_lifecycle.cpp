#include "blockindex_generation_lifecycle.h"

#include "blockindex_hashindex.h"
#include "blockindex_derived_state.h"
#include "util.h"

#include <boost/filesystem.hpp>

#include <zlib.h>

#include <fcntl.h>
#include <limits>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>

namespace fs = boost::filesystem;

namespace {

static const unsigned char BLOCK_INDEX_CURRENT_MAGIC[8] = {'I','N','N','B','C','U','R','1'};

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

static void WriteU32LE(std::string* out, uint32_t value)
{
    out->push_back((char)(value & 0xff));
    out->push_back((char)((value >> 8) & 0xff));
    out->push_back((char)((value >> 16) & 0xff));
    out->push_back((char)((value >> 24) & 0xff));
}

static void WriteU64LE(std::string* out, uint64_t value)
{
    for (int i = 0; i < 8; ++i)
        out->push_back((char)((value >> (8 * i)) & 0xff));
}

static bool ReadU32LE(const unsigned char* p, uint32_t* out)
{
    *out = ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return true;
}

static bool ReadU64LE(const unsigned char* p, uint64_t* out)
{
    *out = ((uint64_t)p[0]) |
           ((uint64_t)p[1] << 8) |
           ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) |
           ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) |
           ((uint64_t)p[7] << 56);
    return true;
}

static uint32_t CurrentChecksum(const unsigned char* data, size_t size)
{
    return (uint32_t)crc32(0L, data, (uInt)size);
}

// Directory fsync: durably persist a rename/creation into `dir`. Returns true on
// success or when the platform does not support it (best-effort). Honest
// wording: this helps durability under normal local filesystems but is not a
// universal hardware/FS atomicity guarantee.
static bool SyncDirectory(const fs::path& dir, std::string* error)
{
#ifndef WIN32
    int fd = open(dir.string().c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0)
    {
        // Best-effort: some environments disallow O_DIRECTORY on a dir fd.
        SetError(error, "directory fsync open failed: " + dir.string());
        return false;
    }
    int rc = fsync(fd);
    close(fd);
    if (rc != 0)
    {
        SetError(error, "directory fsync failed: " + dir.string());
        return false;
    }
    ClearError(error);
    return true;
#else
    (void)dir;
    ClearError(error);
    return true; // Windows lacks a direct directory-fsync; MoveFileEx visibility is relied on
#endif
}

static bool WriteFileDurable(const fs::path& path, const std::string& data, std::string* error)
{
    const fs::path tmp = path.string() + ".tmp";
    FILE* file = fopen(tmp.string().c_str(), "wb");
    if (!file)
        return SetError(error, "open failed: " + tmp.string());
    if (!data.empty() && fwrite(data.data(), 1, data.size(), file) != data.size())
    {
        fclose(file);
        return SetError(error, "write failed: " + tmp.string());
    }
    FileCommit(file);
    fclose(file);
    if (!RenameOver(tmp, path))
        return SetError(error, "rename failed: " + path.string());
    ClearError(error);
    return true;
}

static bool ReadWholeFile(const fs::path& path, std::string* data, std::string* error)
{
    FILE* file = fopen(path.string().c_str(), "rb");
    if (!file)
        return SetError(error, "open failed: " + path.string());
    fseek(file, 0, SEEK_END);
    long sz = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (sz < 0)
    {
        fclose(file);
        return SetError(error, "tell failed: " + path.string());
    }
    data->resize((size_t)sz);
    if (sz)
    {
        size_t n = fread(&(*data)[0], 1, (size_t)sz, file);
        if (n != (size_t)sz)
        {
            fclose(file);
            return SetError(error, "short read: " + path.string());
        }
    }
    fclose(file);
    ClearError(error);
    return true;
}

} // namespace

bool EncodeBlockIndexCurrentRecord(const BlockIndexCurrentRecord& record, std::string* out, std::string* error)
{
    if (!out)
        return SetError(error, "null CURRENT encode output");
    if (record.generation == 0)
        return SetError(error, "CURRENT generation 0 is invalid");
    out->clear();
    out->reserve(BLOCK_INDEX_CURRENT_SIZE_V1);
    out->insert(out->end(), (const char*)BLOCK_INDEX_CURRENT_MAGIC, (const char*)BLOCK_INDEX_CURRENT_MAGIC + 8);
    WriteU32LE(out, record.formatVersion);
    WriteU32LE(out, record.schemaVersion);
    WriteU64LE(out, record.generation);
    const uint32_t csum = CurrentChecksum((const unsigned char*)out->data(), out->size());
    WriteU32LE(out, csum);
    if (out->size() != BLOCK_INDEX_CURRENT_SIZE_V1)
        return SetError(error, "CURRENT encode size mismatch");
    ClearError(error);
    return true;
}

bool DecodeBlockIndexCurrentRecord(const char* data, size_t size, BlockIndexCurrentRecord* out, std::string* error)
{
    if (!data || !out)
        return SetError(error, "null CURRENT decode input/output");
    if (size != BLOCK_INDEX_CURRENT_SIZE_V1)
        return SetError(error, "CURRENT size mismatch");
    const unsigned char* p = (const unsigned char*)data;
    if (memcmp(p, BLOCK_INDEX_CURRENT_MAGIC, 8) != 0)
        return SetError(error, "invalid CURRENT magic");

    BlockIndexCurrentRecord rec;
    size_t off = 8;
    ReadU32LE(p + off, &rec.formatVersion); off += 4;
    ReadU32LE(p + off, &rec.schemaVersion); off += 4;
    ReadU64LE(p + off, &rec.generation); off += 8;
    if (off != 24)
        return SetError(error, "CURRENT decode offset mismatch");

    uint32_t stored = 0;
    ReadU32LE(p + off, &stored);
    const uint32_t expected = CurrentChecksum(p, 24);
    if (stored != expected)
        return SetError(error, "CURRENT checksum mismatch");

    if (rec.formatVersion != BLOCK_INDEX_CURRENT_FORMAT_VERSION)
        return SetError(error, "unsupported CURRENT format version");
    if (rec.schemaVersion != BLOCK_INDEX_CURRENT_SCHEMA_VERSION)
        return SetError(error, "unsupported CURRENT schema version");
    if (rec.generation == 0)
        return SetError(error, "CURRENT generation 0 is invalid");

    *out = rec;
    ClearError(error);
    return true;
}

std::string BlockIndexGenerationManager::GenerationName(uint64_t generation)
{
    return strprintf("gen-%06llu", (unsigned long long)generation);
}

std::string BlockIndexGenerationManager::GenerationPath(const std::string& root, uint64_t generation)
{
    return (fs::path(root) / GenerationName(generation)).string();
}

std::string BlockIndexGenerationManager::StagingName(uint64_t generation)
{
    return strprintf("build-%06llu.tmp", (unsigned long long)generation);
}

std::string BlockIndexGenerationManager::StagingPath(const std::string& root, uint64_t generation)
{
    return (fs::path(root) / StagingName(generation)).string();
}

BlockIndexLifecycleStatus BlockIndexGenerationManager::ValidateGenerationDir(
    const std::string& generationDir, uint64_t expectedGeneration,
    bool requireStableName, uint64_t generation, std::string* error)
{
    if (generation == 0)
    {
        SetError(error, "generation 0 is invalid");
        return BLOCK_INDEX_LIFECYCLE_ERROR;
    }
    const fs::path dir(generationDir);
    if (!fs::exists(dir) || !fs::is_directory(dir))
    {
        SetError(error, "generation directory missing: " + generationDir);
        return BLOCK_INDEX_LIFECYCLE_MISSING_GENERATION;
    }
    if (requireStableName)
    {
        if (dir.filename().string() != GenerationName(generation))
        {
            SetError(error, "generation directory name does not match gen-%06llu: " + dir.string());
            return BLOCK_INDEX_LIFECYCLE_ERROR;
        }
        // A build-*.tmp is never a selectable stable generation.
        if (dir.filename().string() == StagingName(generation))
        {
            SetError(error, "staging build-N.tmp is not a stable generation: " + dir.string());
            return BLOCK_INDEX_LIFECYCLE_ERROR;
        }
    }

    // Required components.
    const fs::path recordsPath = dir / BLOCK_INDEX_RECORDS_FILE_NAME;
    const fs::path manifestPath = dir / BLOCK_INDEX_MANIFEST_FILE_NAME;
    const fs::path activePath = dir / BLOCK_INDEX_ACTIVE_FILE_NAME;
    const fs::path hashDir = dir / BLOCK_INDEX_HASHINDEX_DIR_NAME;
    if (!fs::exists(recordsPath) || !fs::exists(manifestPath) ||
        !fs::exists(activePath) || !fs::exists(hashDir))
    {
        SetError(error, "generation missing required component: " + dir.string());
        return BLOCK_INDEX_LIFECYCLE_ERROR;
    }

    // MANIFEST COMPLETE + generation binding + committed region via reader.
    FixedBlockIndexOpenOptions opts;
    opts.requireCompleteManifest = true;
    FixedBlockIndexStore store;
    if (!FixedBlockIndexStore::OpenReadOnly(dir.string(), opts, &store, error))
        return BLOCK_INDEX_LIFECYCLE_ERROR;
    const FixedBlockIndexManifest& m = store.GetManifest();
    if (m.state != BLOCK_INDEX_MANIFEST_COMPLETE)
    {
        SetError(error, "generation MANIFEST not COMPLETE");
        return BLOCK_INDEX_LIFECYCLE_ERROR;
    }
    if (m.generation != expectedGeneration)
    {
        SetError(error, "generation id bound mismatch (MANIFEST)");
        return BLOCK_INDEX_LIFECYCLE_ERROR;
    }

    // active.dat generation binding + dense layout via reader (O(1) open).
    BlockIndexActiveIndex active;
    if (!BlockIndexActiveIndex::Open(dir.string(), expectedGeneration, &active, error))
        return BLOCK_INDEX_LIFECYCLE_ERROR;
    if (active.Generation() != expectedGeneration)
    {
        SetError(error, "generation id bound mismatch (active.dat)");
        return BLOCK_INDEX_LIFECYCLE_ERROR;
    }

    // hashindex generation binding (note: bundled LevelDB open is logical
    // read-only; may create/refresh LOG/LOCK metadata inside the generation dir).
    BlockIndexHashIndex hashIdx;
    if (!BlockIndexHashIndex::Open(dir.string(), expectedGeneration, &hashIdx, error))
        return BLOCK_INDEX_LIFECYCLE_ERROR;
    if (hashIdx.Generation() != expectedGeneration)
    {
        SetError(error, "generation id bound mismatch (hashindex)");
        return BLOCK_INDEX_LIFECYCLE_ERROR;
    }
    hashIdx.Close();

    // Committed tip coherence (O(1): only the tip entry + tip record, not the
    // full O(n) active scan).
    if (m.committedTipHeight >= 0)
    {
        if ((uint64_t)m.committedTipHeight > (uint64_t)active.PhysicalHeight())
        {
            SetError(error, "committed tip height exceeds active.dat physical height");
            return BLOCK_INDEX_LIFECYCLE_ERROR;
        }
        if (m.committedTipId == BLOCK_INDEX_ID_INVALID || m.committedTipId > m.recordCount)
        {
            SetError(error, "committed tip RecordId invalid/beyond count");
            return BLOCK_INDEX_LIFECYCLE_ERROR;
        }
        BlockIndexId activeTipId = BLOCK_INDEX_ID_INVALID;
        if (!active.ReadEntry(m.committedTipHeight, &activeTipId, error))
            return BLOCK_INDEX_LIFECYCLE_ERROR;
        if (activeTipId != m.committedTipId)
        {
            SetError(error, "active.dat tip RecordId not coherent with MANIFEST committedTipId");
            return BLOCK_INDEX_LIFECYCLE_ERROR;
        }
        BlockIndexRecord tipRec;
        if (!store.Read(m.committedTipId, &tipRec, error))
            return BLOCK_INDEX_LIFECYCLE_ERROR;
        if (tipRec.hash != m.committedTipHash || tipRec.height != m.committedTipHeight)
        {
            SetError(error, "committed tip record/hash/height incoherent with MANIFEST");
            return BLOCK_INDEX_LIFECYCLE_ERROR;
        }
    }

    // derived.dat validation (optional component for authoritative capability).
    // If present, validate format, generation binding, entry count, and content
    // binding. If absent, the generation is OLD_SHADOW_VALID (not authoritative-
    // capable but still valid for shadow/legacy use).
    const fs::path derivedPath = dir / BLOCK_INDEX_DERIVED_FILE_NAME;
    if (fs::exists(derivedPath))
    {
        BlockIndexDerivedStateStore derivedStore;
        if (!BlockIndexDerivedStateStore::OpenReadOnly(dir.string(), expectedGeneration, &derivedStore, error))
        {
            // derived.dat present but corrupt/mismatched: fail validation
            return BLOCK_INDEX_LIFECYCLE_ERROR;
        }
        // Entry count must match MANIFEST record count
        if (derivedStore.EntryCount() != m.recordCount)
        {
            SetError(error, "derived.dat entry count does not match MANIFEST record count");
            return BLOCK_INDEX_LIFECYCLE_ERROR;
        }
        // Content binding must match MANIFEST tip/count/generation
        if (!derivedStore.ValidateContentBinding(m.committedTipHash, m.recordCount, error))
        {
            return BLOCK_INDEX_LIFECYCLE_ERROR;
        }
    }

    ClearError(error);
    return BLOCK_INDEX_LIFECYCLE_OK;
}

BlockIndexLifecycleStatus BlockIndexGenerationManager::ValidateGeneration(const std::string& root,
                                                                          uint64_t generation,
                                                                          std::string* error)
{
    return ValidateGenerationDir(GenerationPath(root, generation), generation,
                                 /*requireStableName=*/true, generation, error);
}

BlockIndexLifecycleStatus BlockIndexGenerationManager::PublishGeneration(const std::string& root,
                                                                         uint64_t generation,
                                                                         std::string* error)
{
    if (generation == 0)
    {
        SetError(error, "generation 0 is invalid");
        return BLOCK_INDEX_LIFECYCLE_ERROR;
    }
    const fs::path staging(StagingPath(root, generation));
    const fs::path stable(GenerationPath(root, generation));
    if (!fs::exists(staging))
    {
        SetError(error, "staging generation missing: " + staging.string());
        return BLOCK_INDEX_LIFECYCLE_ERROR;
    }
    if (fs::exists(stable))
    {
        SetError(error, "stable generation already exists (refusing to overwrite rollback): " + stable.string());
        return BLOCK_INDEX_LIFECYCLE_ERROR;
    }
    // Validate the staging generation structurally (build-N.tmp, not stable name).
    const BlockIndexLifecycleStatus v = ValidateGenerationDir(staging.string(), generation,
                                                             /*requireStableName=*/false, generation, error);
    if (v != BLOCK_INDEX_LIFECYCLE_OK)
        return v;

    // Ensure no write handle is open on the generation before rename. (Caller is
    // responsible for having closed the builder/writer. Nothing is held here.)

    // Durable rename build-N.tmp -> gen-N.
    if (!RenameOver(staging, stable))
    {
        SetError(error, "publication rename failed: " + staging.string() + " -> " + stable.string());
        return BLOCK_INDEX_LIFECYCLE_ERROR;
    }
    std::string dirErr;
    SyncDirectory(fs::path(root), &dirErr); // best-effort durability of the rename

    // Reopen gen-N and validate again after rename.
    const BlockIndexLifecycleStatus v2 = ValidateGenerationDir(stable.string(), generation,
                                                              /*requireStableName=*/true, generation, error);
    if (v2 != BLOCK_INDEX_LIFECYCLE_OK)
        return v2;

    ClearError(error);
    return BLOCK_INDEX_LIFECYCLE_OK;
}

BlockIndexLifecycleStatus BlockIndexGenerationManager::SelectGeneration(const std::string& root,
                                                                        uint64_t generation,
                                                                        std::string* error)
{
    // Validate the selected generation first.
    const BlockIndexLifecycleStatus v = ValidateGeneration(root, generation, error);
    if (v != BLOCK_INDEX_LIFECYCLE_OK)
        return v;

    // Encode CURRENT atomically (tmp write + file fsync + atomic rename + dir fsync).
    BlockIndexCurrentRecord rec;
    rec.generation = generation;
    std::string encoded;
    if (!EncodeBlockIndexCurrentRecord(rec, &encoded, error))
        return BLOCK_INDEX_LIFECYCLE_ERROR;
    const fs::path currentPath = fs::path(root) / BLOCK_INDEX_CURRENT_FILE_NAME;
    if (!WriteFileDurable(currentPath, encoded, error))
        return BLOCK_INDEX_LIFECYCLE_ERROR;
    std::string dirErr;
    SyncDirectory(fs::path(root), &dirErr); // best-effort durability of CURRENT create/rename

    ClearError(error);
    return BLOCK_INDEX_LIFECYCLE_OK;
}

BlockIndexLifecycleStatus BlockIndexGenerationManager::ReadCurrent(const std::string& root,
                                                                   BlockIndexCurrentRecord* out,
                                                                   std::string* error)
{
    const fs::path currentPath = fs::path(root) / BLOCK_INDEX_CURRENT_FILE_NAME;
    if (!fs::exists(currentPath))
    {
        ClearError(error);
        return BLOCK_INDEX_LIFECYCLE_NOT_PUBLISHED; // absent == NOT_PUBLISHED
    }
    std::string data;
    if (!ReadWholeFile(currentPath, &data, error))
        return BLOCK_INDEX_LIFECYCLE_ERROR;
    if (!out)
    {
        SetError(error, "null CURRENT output");
        return BLOCK_INDEX_LIFECYCLE_ERROR;
    }
    if (!DecodeBlockIndexCurrentRecord(data.data(), data.size(), out, error))
        return BLOCK_INDEX_LIFECYCLE_CORRUPT; // present but malformed
    ClearError(error);
    return BLOCK_INDEX_LIFECYCLE_OK;
}

BlockIndexLifecycleStatus BlockIndexGenerationManager::OpenCurrent(const std::string& root,
                                                                   uint64_t* selectedGeneration,
                                                                   std::string* error)
{
    BlockIndexCurrentRecord rec;
    const BlockIndexLifecycleStatus st = ReadCurrent(root, &rec, error);
    if (st != BLOCK_INDEX_LIFECYCLE_OK)
    {
        if (st == BLOCK_INDEX_LIFECYCLE_NOT_PUBLISHED)
            ClearError(error);
        return st;
    }
    if (selectedGeneration)
        *selectedGeneration = rec.generation;

    // Validate the selected stable generation.
    const BlockIndexLifecycleStatus v = ValidateGeneration(root, rec.generation, error);
    if (v != BLOCK_INDEX_LIFECYCLE_OK)
    {
        if (v == BLOCK_INDEX_LIFECYCLE_MISSING_GENERATION)
            return BLOCK_INDEX_LIFECYCLE_MISSING_GENERATION;
        return v;
    }
    ClearError(error);
    return BLOCK_INDEX_LIFECYCLE_OK;
}