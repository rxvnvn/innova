#include "blockindex_activeindex.h"

#include "util.h"

#include <boost/filesystem.hpp>

#include <limits>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace {

static const unsigned char BLOCK_INDEX_ACTIVE_MAGIC[8] = {'I','N','N','B','A','C','T','1'};

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

static void WriteU32LE(std::vector<unsigned char>& out, uint32_t value)
{
    out.push_back((unsigned char)(value & 0xff));
    out.push_back((unsigned char)((value >> 8) & 0xff));
    out.push_back((unsigned char)((value >> 16) & 0xff));
    out.push_back((unsigned char)((value >> 24) & 0xff));
}

static void WriteU64LE(std::vector<unsigned char>& out, uint64_t value)
{
    for (int i = 0; i < 8; ++i)
        out.push_back((unsigned char)((value >> (8 * i)) & 0xff));
}

static bool ReadU32LEFrom(const unsigned char* data, size_t size, size_t* offset, uint32_t* out, std::string* error)
{
    if (*offset > size || size - *offset < 4)
        return SetError(error, "short read for uint32");
    const unsigned char* p = data + *offset;
    *out = ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    *offset += 4;
    return true;
}

static bool ReadU64LEFrom(const unsigned char* data, size_t size, size_t* offset, uint64_t* out, std::string* error)
{
    if (*offset > size || size - *offset < 8)
        return SetError(error, "short read for uint64");
    const unsigned char* p = data + *offset;
    *out = ((uint64_t)p[0]) |
           ((uint64_t)p[1] << 8) |
           ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) |
           ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) |
           ((uint64_t)p[7] << 56);
    *offset += 8;
    return true;
}

static bool DecodeU64LEBytes(const char* data, size_t size, uint64_t* out, std::string* error)
{
    if (!data || !out)
        return SetError(error, "null decode input/output");
    if (size != 8)
        return SetError(error, "invalid entry byte length");
    const unsigned char* p = (const unsigned char*)data;
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

static bool ReadExactFile(const boost::filesystem::path& path, std::vector<unsigned char>* out, size_t expectedSize, std::string* error)
{
    FILE* file = fopen(path.string().c_str(), "rb");
    if (!file)
        return SetError(error, "open failed: " + path.string());
    out->assign(expectedSize, 0);
    const size_t n = expectedSize ? fread(&(*out)[0], 1, expectedSize, file) : 0;
    fclose(file);
    if (n != expectedSize)
        return SetError(error, "short read: " + path.string());
    return true;
}

static bool WriteFileAtomically(const boost::filesystem::path& dest, const std::vector<unsigned char>& bytes, std::string* error)
{
    const boost::filesystem::path tmp = dest.string() + ".tmp";
    FILE* file = fopen(tmp.string().c_str(), "wb");
    if (!file)
        return SetError(error, "open failed: " + tmp.string());
    if (!bytes.empty() && fwrite(&bytes[0], 1, bytes.size(), file) != bytes.size())
    {
        fclose(file);
        return SetError(error, "write failed: " + tmp.string());
    }
    FileCommit(file);
    fclose(file);
    if (!RenameOver(tmp, dest))
        return SetError(error, "rename failed: " + dest.string());
    ClearError(error);
    return true;
}

static bool EncodeHeader(uint64_t generation, std::vector<unsigned char>* out)
{
    out->clear();
    out->insert(out->end(), BLOCK_INDEX_ACTIVE_MAGIC, BLOCK_INDEX_ACTIVE_MAGIC + 8);
    WriteU32LE(*out, BLOCK_INDEX_ACTIVE_SCHEMA_VERSION);          // format_version
    WriteU32LE(*out, BLOCK_INDEX_ACTIVE_SCHEMA_VERSION);          // schema_version
    WriteU32LE(*out, BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1);          // header_size
    WriteU32LE(*out, BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1);           // entry_size
    WriteU64LE(*out, generation);                                  // generation
    WriteU64LE(*out, 0);                                           // reserved
    return out->size() == BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1;
}

static bool DecodeHeader(const unsigned char* data, size_t size, uint64_t* generation, std::string* error)
{
    if (size != BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1)
        return SetError(error, "active.dat header size mismatch");
    if (memcmp(data, BLOCK_INDEX_ACTIVE_MAGIC, 8) != 0)
        return SetError(error, "invalid active.dat magic");
    size_t offset = 8;
    uint32_t formatVersion = 0;
    uint32_t schemaVersion = 0;
    uint32_t headerSize = 0;
    uint32_t entrySize = 0;
    uint64_t reserved = 0;
    if (!ReadU32LEFrom(data, size, &offset, &formatVersion, error) ||
        !ReadU32LEFrom(data, size, &offset, &schemaVersion, error) ||
        !ReadU32LEFrom(data, size, &offset, &headerSize, error) ||
        !ReadU32LEFrom(data, size, &offset, &entrySize, error) ||
        !ReadU64LEFrom(data, size, &offset, generation, error) ||
        !ReadU64LEFrom(data, size, &offset, &reserved, error))
        return false;
    if (formatVersion != BLOCK_INDEX_ACTIVE_SCHEMA_VERSION)
        return SetError(error, "unsupported active.dat format version");
    if (schemaVersion != BLOCK_INDEX_ACTIVE_SCHEMA_VERSION)
        return SetError(error, "unsupported active.dat schema version");
    if (headerSize != BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1)
        return SetError(error, "invalid active.dat header size");
    if (entrySize != BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1)
        return SetError(error, "invalid active.dat entry size");
    if (reserved != 0)
        return SetError(error, "non-zero reserved active.dat header bytes");
    if (offset != size)
        return SetError(error, "active.dat header trailing bytes");
    ClearError(error);
    return true;
}

static bool CheckedHeightToOffset(int64_t height, uint64_t* out)
{
    if (height < 0)
        return false;
    uint64_t base = BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1;
    if ((uint64_t)height > (std::numeric_limits<uint64_t>::max() - base) / BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1)
        return false;
    *out = base + (uint64_t)height * BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1;
    return true;
}

} // namespace

bool EncodeBlockIndexActiveEntry(BlockIndexId id, std::string* out, std::string* error)
{
    if (!out)
        return SetError(error, "null active entry output");
    if (id == BLOCK_INDEX_ID_INVALID)
        return SetError(error, "active entry RecordId 0 is invalid");
    out->clear();
    out->reserve(BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1);
    for (int i = 0; i < 8; ++i)
        out->push_back((char)((id >> (8 * i)) & 0xff));
    if (out->size() != BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1)
        return SetError(error, "invalid encoded active entry size");
    ClearError(error);
    return true;
}

bool DecodeBlockIndexActiveEntry(const char* data, size_t size, BlockIndexId* out, std::string* error)
{
    uint64_t id = 0;
    if (!DecodeU64LEBytes(data, size, &id, error))
        return false;
    if (id == BLOCK_INDEX_ID_INVALID)
        return SetError(error, "decoded active entry RecordId 0 is invalid");
    *out = id;
    ClearError(error);
    return true;
}

struct BlockIndexActiveIndex::ReadHandle
{
    FILE* file;
    CCriticalSection cs;
    explicit ReadHandle(FILE* f) : file(f) {}
    ~ReadHandle() { if (file) fclose(file); }
};

BlockIndexActiveIndex::BlockIndexActiveIndex()
    : generation(0),
      physicalHeight(-1),
      writable(false),
      open(false)
{
}

bool BlockIndexActiveIndex::InitializePaths(const std::string& dir, std::string* error)
{
    dirPath = boost::filesystem::path(dir);
    if (dirPath.empty())
        return SetError(error, "empty active-index generation directory path");
    activePath = dirPath / BLOCK_INDEX_ACTIVE_FILE_NAME;
    ClearError(error);
    return true;
}

bool BlockIndexActiveIndex::WriteHeader(std::string* error)
{
    std::vector<unsigned char> bytes;
    EncodeHeader(generation, &bytes);
    return WriteFileAtomically(activePath, bytes, error);
}

bool BlockIndexActiveIndex::LoadHeader(uint64_t* fileSize, std::string* error)
{
    if (!boost::filesystem::exists(activePath))
        return SetError(error, "missing active.dat");
    const uint64_t size = boost::filesystem::file_size(activePath);
    if (size < BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1)
        return SetError(error, "truncated active.dat header");
    std::vector<unsigned char> bytes;
    if (!ReadExactFile(activePath, &bytes, BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1, error))
        return false;
    uint64_t fileGeneration = 0;
    if (!DecodeHeader(&bytes[0], bytes.size(), &fileGeneration, error))
        return false;
    if (fileGeneration != generation)
        return SetError(error, "generation mismatch between store and active.dat");
    const uint64_t bodyBytes = size - BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1;
    if (bodyBytes % BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1 != 0)
        return SetError(error, "active.dat body is not a dense entry multiple");
    physicalHeight = (int64_t)(bodyBytes / BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1) - 1;
    if (fileSize)
        *fileSize = size;
    ClearError(error);
    return true;
}

bool BlockIndexActiveIndex::Create(const std::string& dir, uint64_t gen, BlockIndexActiveIndex* out, std::string* error)
{
    if (!out)
        return SetError(error, "null output active index");
    if (gen == 0)
        return SetError(error, "generation 0 is invalid");
    BlockIndexActiveIndex index;
    if (!index.InitializePaths(dir, error))
        return false;
    if (boost::filesystem::exists(index.activePath))
        return SetError(error, "active.dat already exists");
    boost::filesystem::create_directories(index.dirPath);
    index.generation = gen;
    index.physicalHeight = -1;
    index.writable = true;
    index.open = true;
    if (!index.WriteHeader(error))
        return false;
    *out = index;
    ClearError(error);
    return true;
}

bool BlockIndexActiveIndex::OpenExisting(uint64_t expectedGeneration, bool makeWritable, std::string* error)
{
    open = true;
    writable = makeWritable;
    uint64_t headerGeneration = 0;
    if (!boost::filesystem::exists(activePath))
        return SetError(error, "missing active.dat");
    const uint64_t activeFileSize = boost::filesystem::file_size(activePath);
    if (activeFileSize < BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1)
        return SetError(error, "truncated active.dat header");
    if ((activeFileSize - BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1) % BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1 != 0)
        return SetError(error, "invalid active.dat file size (not a dense multiple)");
    std::vector<unsigned char> bytes;
    if (!ReadExactFile(activePath, &bytes, BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1, error))
        return false;
    if (!DecodeHeader(&bytes[0], bytes.size(), &headerGeneration, error))
        return false;
    if (expectedGeneration != 0 && headerGeneration != expectedGeneration)
        return SetError(error, "active.dat generation mismatch");
    generation = headerGeneration;
    uint64_t fileSize = 0;
    if (!LoadHeader(&fileSize, error))
        return false;
    if (!makeWritable)
    {
        FILE* readFile = fopen(activePath.string().c_str(), "rb");
        if (!readFile)
            return SetError(error, "persistent active read open failed: " + activePath.string());
        readHandle.reset(new ReadHandle(readFile));
    }
    ClearError(error);
    return true;
}

bool BlockIndexActiveIndex::Open(const std::string& dir, uint64_t expectedGeneration, BlockIndexActiveIndex* out, std::string* error)
{
    if (!out)
        return SetError(error, "null output active index");
    BlockIndexActiveIndex index;
    if (!index.InitializePaths(dir, error))
        return false;
    if (!index.OpenExisting(expectedGeneration, false, error))
        return false;
    *out = index;
    ClearError(error);
    return true;
}

bool BlockIndexActiveIndex::OpenWritable(const std::string& dir, uint64_t expectedGeneration, BlockIndexActiveIndex* out, std::string* error)
{
    if (!out)
        return SetError(error, "null output active index");
    BlockIndexActiveIndex index;
    if (!index.InitializePaths(dir, error))
        return false;
    if (!index.OpenExisting(expectedGeneration, true, error))
        return false;
    *out = index;
    ClearError(error);
    return true;
}

bool BlockIndexActiveIndex::Append(BlockIndexId id, int32_t height, std::string* error)
{
    if (!open || !writable)
        return SetError(error, "active index is not writable");
    if (id == BLOCK_INDEX_ID_INVALID)
        return SetError(error, "active entry RecordId 0 is invalid");
    if (physicalHeight == std::numeric_limits<int32_t>::max() - 1)
        return SetError(error, "active height space exhausted");
    if ((int64_t)height != physicalHeight + 1)
        return SetError(error, "non-dense active append (height must be physicalHeight + 1)");
    uint64_t offset = 0;
    if (!CheckedHeightToOffset(height, &offset))
        return SetError(error, "active append height overflow");
    std::vector<unsigned char> entry;
    entry.push_back((unsigned char)(id & 0xff));
    entry.push_back((unsigned char)((id >> 8) & 0xff));
    entry.push_back((unsigned char)((id >> 16) & 0xff));
    entry.push_back((unsigned char)((id >> 24) & 0xff));
    entry.push_back((unsigned char)((id >> 32) & 0xff));
    entry.push_back((unsigned char)((id >> 40) & 0xff));
    entry.push_back((unsigned char)((id >> 48) & 0xff));
    entry.push_back((unsigned char)((id >> 56) & 0xff));

    FILE* file = fopen(activePath.string().c_str(), "rb+");
    if (!file)
        return SetError(error, "append open failed: " + activePath.string());
    if (fseeko(file, (off_t)offset, SEEK_SET) != 0)
    {
        fclose(file);
        return SetError(error, "seek failed for active append");
    }
    if (fwrite(&entry[0], 1, entry.size(), file) != entry.size())
    {
        fclose(file);
        return SetError(error, "append write failed: " + activePath.string());
    }
    FileCommit(file);
    fclose(file);
    physicalHeight = height;
    ClearError(error);
    return true;
}

bool BlockIndexActiveIndex::AppendBatch(const std::vector<BlockIndexId>& ids, std::string* error)
{
    if (!open || !writable)
        return SetError(error, "active index is not writable");
    if (ids.empty())
        return SetError(error, "empty batch active append");
    for (size_t i = 0; i < ids.size(); ++i)
    {
        if (ids[i] == BLOCK_INDEX_ID_INVALID)
            return SetError(error, "active entry RecordId 0 is invalid");
    }
    if (physicalHeight == std::numeric_limits<int32_t>::max() - 1)
        return SetError(error, "active height space exhausted");
    if (ids.size() > (size_t)(std::numeric_limits<int32_t>::max() - physicalHeight))
        return SetError(error, "active height range overflow");

    const int32_t startHeight = (int32_t)(physicalHeight + 1);
    uint64_t startOffset = 0;
    if (!CheckedHeightToOffset(startHeight, &startOffset))
        return SetError(error, "active append height overflow");

    std::vector<unsigned char> bytes;
    bytes.reserve(ids.size() * BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1);
    for (size_t i = 0; i < ids.size(); ++i)
    {
        const BlockIndexId id = ids[i];
        for (int b = 0; b < 8; ++b)
            bytes.push_back((char)((id >> (8 * b)) & 0xff));
    }

    FILE* file = fopen(activePath.string().c_str(), "rb+");
    if (!file)
        return SetError(error, "append open failed: " + activePath.string());
    if (fseeko(file, (off_t)startOffset, SEEK_SET) != 0)
    {
        fclose(file);
        return SetError(error, "seek failed for active batch append");
    }
    if (fwrite(&bytes[0], 1, bytes.size(), file) != bytes.size())
    {
        fclose(file);
        return SetError(error, "batch append write failed: " + activePath.string());
    }
    FileCommit(file); // single fsync
    fclose(file);
    physicalHeight = startHeight + (int32_t)ids.size() - 1;
    ClearError(error);
    return true;
}

bool BlockIndexActiveIndex::TruncateTo(int32_t height, std::string* error)
{
    if (!open || !writable)
        return SetError(error, "active index is not writable");
    if ((int64_t)height > physicalHeight)
        return SetError(error, "truncate height above current physical height");
    uint64_t size = BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1;
    if (height < -1)
    {
        return SetError(error, "truncate height below empty index");
    }
    else if (height >= 0)
    {
        const uint64_t count = (uint64_t)(height + 1);
        const uint64_t bytes = count * BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1;
        if (bytes > std::numeric_limits<uint64_t>::max() - BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1)
            return SetError(error, "truncate size overflow");
        size = BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1 + bytes;
    }
    FILE* file = fopen(activePath.string().c_str(), "rb+");
    if (!file)
        return SetError(error, "truncate open failed: " + activePath.string());
    if (ftruncate(fileno(file), (off_t)size) != 0)
    {
        fclose(file);
        return SetError(error, "truncate failed: " + activePath.string());
    }
    FileCommit(file);
    fclose(file);
    physicalHeight = height;
    ClearError(error);
    return true;
}

bool BlockIndexActiveIndex::ReadEntry(int32_t height, BlockIndexId* outId, std::string* error) const
{
    if (!open)
        return SetError(error, "active index is not open");
    if (!outId)
        return SetError(error, "null active entry output id");
    if (height < 0 || (int64_t)height > physicalHeight)
        return SetError(error, "active entry height outside physical range");
    uint64_t offset = 0;
    if (!CheckedHeightToOffset(height, &offset))
        return SetError(error, "active entry offset overflow");
    if (!readHandle)
    {
        // Writable builders/reorg editors deliberately do not retain a read fd;
        // preserve their legacy per-call read behavior.
        FILE* file = fopen(activePath.string().c_str(), "rb");
        if (!file) return SetError(error, "read open failed: " + activePath.string());
        if (fseeko(file, (off_t)offset, SEEK_SET) != 0) { fclose(file); return SetError(error, "seek failed for active entry read"); }
        std::string bytes(BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1, '\0');
        const size_t n = fread(&bytes[0], 1, bytes.size(), file);
        fclose(file);
        if (n != bytes.size()) return SetError(error, "short active entry read");
        return DecodeBlockIndexActiveEntry(bytes.data(), bytes.size(), outId, error);
    }
    LOCK(readHandle->cs);
    if (fseeko(readHandle->file, (off_t)offset, SEEK_SET) != 0)
        return SetError(error, "seek failed for active entry read");
    std::string bytes(BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1, '\0');
    const size_t n = fread(&bytes[0], 1, bytes.size(), readHandle->file);
    if (n != bytes.size())
        return SetError(error, "short active entry read");
    return DecodeBlockIndexActiveEntry(bytes.data(), bytes.size(), outId, error);
}

int64_t BlockIndexActiveIndex::PhysicalHeight() const
{
    return open ? physicalHeight : -1;
}

uint64_t BlockIndexActiveIndex::Generation() const
{
    return generation;
}

bool BlockIndexActiveIndex::IsOpen() const
{
    return open;
}

bool BlockIndexActiveIndex::IsLogicalReadOnly() const
{
    return open && !writable;
}

FixedBlockIndexShadowActiveLookup::FixedBlockIndexShadowActiveLookup()
    : open(false)
{
}

bool FixedBlockIndexShadowActiveLookup::Open(const std::string& generationDir, FixedBlockIndexShadowActiveLookup* out, std::string* error)
{
    if (!out)
        return SetError(error, "null shadow active lookup output");
    FixedBlockIndexShadowActiveLookup tmp;
    FixedBlockIndexOpenOptions storeOptions;
    storeOptions.requireCompleteManifest = true;
    if (!FixedBlockIndexStore::OpenReadOnly(generationDir, storeOptions, &tmp.store, error))
        return false;
    const uint64_t generation = tmp.store.GetManifest().generation;
    if (!BlockIndexActiveIndex::Open(generationDir, generation, &tmp.active, error))
        return false;
    if (tmp.active.Generation() != generation)
        return SetError(error, "shadow active lookup generation mismatch");

    const FixedBlockIndexManifest& manifest = tmp.store.GetManifest();
    const int32_t tipHeight = manifest.committedTipHeight;
    if (tipHeight >= 0)
    {
        if (tmp.active.PhysicalHeight() < tipHeight)
            return SetError(error, "active.dat truncated within committed region");
        for (int32_t h = 0; h <= tipHeight; ++h)
        {
            BlockIndexId id = BLOCK_INDEX_ID_INVALID;
            if (!tmp.active.ReadEntry(h, &id, error))
                return false;
            if (id == BLOCK_INDEX_ID_INVALID)
                return SetError(error, "sparse zero active entry within committed region");
            if (id > manifest.recordCount)
                return SetError(error, "active entry exceeds committed record count");
        }
        if (manifest.committedTipId != BLOCK_INDEX_ID_INVALID)
        {
            BlockIndexId tipEntry = BLOCK_INDEX_ID_INVALID;
            if (!tmp.active.ReadEntry(tipHeight, &tipEntry, error))
                return false;
            if (tipEntry != manifest.committedTipId)
                return SetError(error, "active.dat committed-tip not coherent with MANIFEST");
        }
    }
    tmp.open = true;
    *out = std::move(tmp);
    ClearError(error);
    return true;
}

BlockIndexActiveLookupStatus FixedBlockIndexShadowActiveLookup::LookupByHeight(int32_t height, BlockIndexRecord* outRecord, BlockIndexId* outId, std::string* error) const
{
    if (!open)
    {
        SetError(error, "shadow active lookup is not open");
        return BLOCK_INDEX_ACTIVE_LOOKUP_ERROR;
    }
    if (!outRecord || !outId)
    {
        SetError(error, "null shadow active lookup outputs");
        return BLOCK_INDEX_ACTIVE_LOOKUP_ERROR;
    }
    const FixedBlockIndexManifest& manifest = store.GetManifest();
    if (height < 0)
    {
        SetError(error, "negative active height");
        return BLOCK_INDEX_ACTIVE_LOOKUP_ERROR;
    }
    if (height > manifest.committedTipHeight)
    {
        *outId = BLOCK_INDEX_ID_INVALID;
        ClearError(error);
        return BLOCK_INDEX_ACTIVE_LOOKUP_NOT_FOUND;
    }
    BlockIndexId id = BLOCK_INDEX_ID_INVALID;
    if (!active.ReadEntry(height, &id, error))
        return BLOCK_INDEX_ACTIVE_LOOKUP_ERROR;
    if (id == BLOCK_INDEX_ID_INVALID)
    {
        SetError(error, "zero active RecordId");
        return BLOCK_INDEX_ACTIVE_LOOKUP_ERROR;
    }
    if (id > manifest.recordCount)
    {
        SetError(error, "active RecordId exceeds committed record count");
        return BLOCK_INDEX_ACTIVE_LOOKUP_ERROR;
    }
    if (!store.Read(id, outRecord, error))
        return BLOCK_INDEX_ACTIVE_LOOKUP_ERROR;
    if (outRecord->height != height)
    {
        SetError(error, "active height/record height mismatch");
        return BLOCK_INDEX_ACTIVE_LOOKUP_ERROR;
    }
    *outId = id;
    ClearError(error);
    return BLOCK_INDEX_ACTIVE_LOOKUP_FOUND;
}