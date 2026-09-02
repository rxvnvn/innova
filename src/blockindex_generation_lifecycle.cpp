#include "blockindex_generation_lifecycle.h"

#include "blockindex_hashindex.h"
#include "blockindex_activeindex.h"
#include "blockindex_derived_state.h"
#include "fixed_blockindex_store.h"
#include "util.h"

#include <openssl/sha.h>

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

// A.10.1b-fix3: Independently recompute generation root from actual component
// files on disk. This verifies content integrity without trusting the builder's
// in-memory state. Returns true on success, false on I/O or format error.
// Exposed in header for testing.
bool RecomputeGenerationRootFromFiles(const fs::path& dir,
                                              uint64_t generation,
                                              const uint256& tipHash,
                                              uint64_t recordCount,
                                              const unsigned char persistedDagInputDigest[32],
                                              unsigned char recomputedRoot[32],
                                              std::string* error)
{
    // 1. Recompute recordsDigest from records.dat
    unsigned char recordsDigest[32];
    {
        const fs::path recordsPath = dir / BLOCK_INDEX_RECORDS_FILE_NAME;
        FILE* rf = fopen(recordsPath.string().c_str(), "rb");
        if (!rf)
            return SetError(error, "cannot open records.dat for root recomputation");
        fseeko(rf, (off_t)BLOCK_INDEX_RECORDS_HEADER_SIZE_V1, SEEK_SET);
        SHA256_CTX ctx;
        SHA256_Init(&ctx);
        std::vector<unsigned char> recBuf(BLOCK_INDEX_RECORD_SIZE_V1);
        for (uint64_t i = 0; i < recordCount; ++i)
        {
            if (fread(&recBuf[0], 1, BLOCK_INDEX_RECORD_SIZE_V1, rf) != BLOCK_INDEX_RECORD_SIZE_V1)
            {
                fclose(rf);
                return SetError(error, "records.dat truncated during root recomputation");
            }
            SHA256_Update(&ctx, &recBuf[0], BLOCK_INDEX_RECORD_SIZE_V1);
        }
        fclose(rf);
        SHA256_Final(recordsDigest, &ctx);
    }

    // 2. Recompute activeDigest from active.dat
    unsigned char activeDigest[32];
    {
        const fs::path activePath = dir / BLOCK_INDEX_ACTIVE_FILE_NAME;
        // Open active index to get physical height (entry count)
        BlockIndexActiveIndex active;
        if (!BlockIndexActiveIndex::Open(dir.string(), generation, &active, error))
            return false;
        int64_t physHeight = active.PhysicalHeight();
        if (physHeight < 0)
            return SetError(error, "active.dat empty during root recomputation");
        FILE* af = fopen(activePath.string().c_str(), "rb");
        if (!af)
            return SetError(error, "cannot open active.dat for root recomputation");
        fseeko(af, (off_t)BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1, SEEK_SET);
        SHA256_CTX ctx;
        SHA256_Init(&ctx);
        std::vector<unsigned char> actBuf(BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1);
        // Entries are at heights 0..physHeight, so physHeight+1 entries total
        for (int64_t h = 0; h <= physHeight; ++h)
        {
            if (fread(&actBuf[0], 1, BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1, af) != BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1)
            {
                fclose(af);
                return SetError(error, "active.dat truncated during root recomputation");
            }
            SHA256_Update(&ctx, &actBuf[0], BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1);
        }
        fclose(af);
        SHA256_Final(activeDigest, &ctx);
    }

    // 3. Recompute hashIndexDigest from records.dat
    // The hashindex maps (hash -> RecordId) for every record. Since records are
    // stored in deterministic hash-sorted order with sequential RecordId, we can
    // recompute from records.dat by reading each record's hash and pairing with
    // its RecordId (1-based index).
    // A.10.1b-fix3: use 8-byte LE RecordId to match active hashindex codec.
    // IMPORTANT: the builder hashes uint256::begin() (internal little-endian
    // storage), NOT the big-endian encoded bytes from WriteHashBytes. We must
    // decode the hash and use begin() to match.
    unsigned char hashIndexDigest[32];
    {
        const fs::path recordsPath = dir / BLOCK_INDEX_RECORDS_FILE_NAME;
        FILE* rf = fopen(recordsPath.string().c_str(), "rb");
        if (!rf)
            return SetError(error, "cannot open records.dat for hashindex recomputation");
        fseeko(rf, (off_t)BLOCK_INDEX_RECORDS_HEADER_SIZE_V1, SEEK_SET);
        SHA256_CTX ctx;
        SHA256_Init(&ctx);
        std::vector<unsigned char> recBuf(BLOCK_INDEX_RECORD_SIZE_V1);
        for (uint64_t i = 0; i < recordCount; ++i)
        {
            if (fread(&recBuf[0], 1, BLOCK_INDEX_RECORD_SIZE_V1, rf) != BLOCK_INDEX_RECORD_SIZE_V1)
            {
                fclose(rf);
                return SetError(error, "records.dat truncated during hashindex recomputation");
            }
            // Decode hash from encoded record (big-endian in file) to match
            // builder's uint256::begin() (little-endian internal storage).
            // WriteHashBytes stores via GetHex roundtrip = big-endian bytes.
            // We reverse to get internal storage layout.
            uint256 recHash;
            {
                // Convert big-endian encoded bytes to hex string, then to uint256
                static const char hexChars[] = "0123456789abcdef";
                std::string hex;
                hex.resize(64);
                for (size_t j = 0; j < 32; ++j)
                {
                    const unsigned char value = recBuf[j];
                    hex[2 * j] = hexChars[(value >> 4) & 0x0f];
                    hex[2 * j + 1] = hexChars[value & 0x0f];
                }
                recHash = uint256(hex);
            }
            // Use internal storage layout (little-endian) to match builder
            SHA256_Update(&ctx, recHash.begin(), 32);
            // RecordId (i+1) as 8-byte LE
            uint64_t recordId = i + 1;
            unsigned char idBuf[8];
            for (int b = 0; b < 8; ++b)
                idBuf[b] = (unsigned char)((recordId >> (8 * b)) & 0xff);
            SHA256_Update(&ctx, idBuf, 8);
        }
        fclose(rf);
        SHA256_Final(hashIndexDigest, &ctx);
    }

    // 4. Recompute derivedEntriesDigest from derived.dat
    unsigned char derivedEntriesDigest[32];
    {
        const fs::path derivedPath = dir / BLOCK_INDEX_DERIVED_FILE_NAME;
        FILE* df = fopen(derivedPath.string().c_str(), "rb");
        if (!df)
            return SetError(error, "cannot open derived.dat for root recomputation");
        fseeko(df, (off_t)BLOCK_INDEX_DERIVED_HEADER_SIZE_V2, SEEK_SET);
        SHA256_CTX ctx;
        SHA256_Init(&ctx);
        std::vector<unsigned char> dervBuf(BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2);
        for (uint64_t i = 0; i < recordCount; ++i)
        {
            if (fread(&dervBuf[0], 1, BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2, df) !=
                BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2)
            {
                fclose(df);
                return SetError(error, "derived.dat truncated during root recomputation");
            }
            SHA256_Update(&ctx, &dervBuf[0], BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2);
        }
        fclose(df);
        SHA256_Final(derivedEntriesDigest, &ctx);
    }

    // 5. dagInputDigest is read from persisted MANIFEST (cannot recompute from
    // generation files alone without external LevelDB DAG snapshot).

    // 6. Compute generation root from recomputed digests
    if (!ComputeGenerationRoot(generation, tipHash, recordCount,
                               recordsDigest, activeDigest, hashIndexDigest,
                               derivedEntriesDigest, persistedDagInputDigest,
                               recomputedRoot))
        return SetError(error, "ComputeGenerationRoot failed during recomputation");

    return true;
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
    // A.10.1b-fix2: Use explicit capability field from MANIFEST.
    // If capability is AUTHORITATIVE, derived.dat MUST be present and valid.
    // If capability is OLD_SHADOW (or absent in V1 manifests), derived.dat is optional.
    const fs::path derivedPath = dir / BLOCK_INDEX_DERIVED_FILE_NAME;
    if (m.capability == BLOCK_INDEX_GENERATION_CAPABILITY_AUTHORITATIVE)
    {
        // Authoritative generation: derived.dat MUST be present and valid
        if (!fs::exists(derivedPath))
        {
            SetError(error, "authoritative generation missing derived.dat");
            return BLOCK_INDEX_LIFECYCLE_ERROR;
        }
        BlockIndexDerivedStateStore derivedStore;
        if (!BlockIndexDerivedStateStore::OpenReadOnly(dir.string(), expectedGeneration, &derivedStore, error))
        {
            return BLOCK_INDEX_LIFECYCLE_ERROR;
        }
        // Entry count must match MANIFEST record count
        if (derivedStore.EntryCount() != m.recordCount)
        {
            SetError(error, "derived.dat entry count does not match MANIFEST record count");
            return BLOCK_INDEX_LIFECYCLE_ERROR;
        }
        // A.10.1b-fix3 C2: Independent generation root recomputation from actual files.
        // Read the expected root from derived header contentBinding.
        unsigned char storedRoot[32];
        derivedStore.GetContentBinding(storedRoot);
        unsigned char zeroBinding[32];
        memset(zeroBinding, 0, 32);
        if (memcmp(storedRoot, zeroBinding, 32) == 0)
        {
            SetError(error, "authoritative generation has zero content binding (incomplete build)");
            return BLOCK_INDEX_LIFECYCLE_ERROR;
        }
        // Independently recompute the generation root from actual component files.
        // dagInputDigest is read from V3 MANIFEST (persisted by builder).
        unsigned char recomputedRoot[32];
        if (!RecomputeGenerationRootFromFiles(dir, expectedGeneration,
                                              m.committedTipHash, m.recordCount,
                                              m.dagInputDigest, recomputedRoot, error))
        {
            return BLOCK_INDEX_LIFECYCLE_ERROR;
        }
        if (memcmp(storedRoot, recomputedRoot, 32) != 0)
        {
            SetError(error, "authoritative generation root mismatch: derived header content binding does not match recomputed root from actual files");
            return BLOCK_INDEX_LIFECYCLE_ERROR;
        }
    }
    else if (fs::exists(derivedPath))
    {
        // Old shadow generation with derived.dat present: validate it
        BlockIndexDerivedStateStore derivedStore;
        if (!BlockIndexDerivedStateStore::OpenReadOnly(dir.string(), expectedGeneration, &derivedStore, error))
        {
            return BLOCK_INDEX_LIFECYCLE_ERROR;
        }
        if (derivedStore.EntryCount() != m.recordCount)
        {
            SetError(error, "derived.dat entry count does not match MANIFEST record count");
            return BLOCK_INDEX_LIFECYCLE_ERROR;
        }
        // For old shadow generations with derived.dat, validate metadata binding
        if (!derivedStore.ValidateContentBinding(m.committedTipHash, m.recordCount, error))
        {
            return BLOCK_INDEX_LIFECYCLE_ERROR;
        }
    }
    // else: old shadow generation without derived.dat -> valid for shadow use

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