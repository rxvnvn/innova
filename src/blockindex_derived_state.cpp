// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "blockindex_derived_state.h"

#include "util.h"

#include <zlib.h>

#include <algorithm>
#include <limits>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace {

static const unsigned char BLOCK_INDEX_DERIVED_MAGIC[8] = {'I','N','N','B','D','R','V','1'};

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

static void WriteI32LE(std::vector<unsigned char>& out, int32_t value)
{
    WriteU32LE(out, (uint32_t)value);
}

static void WriteU64LE(std::vector<unsigned char>& out, uint64_t value)
{
    for (int i = 0; i < 8; ++i)
        out.push_back((unsigned char)((value >> (8 * i)) & 0xff));
}

static void WriteI64LE(std::vector<unsigned char>& out, int64_t value)
{
    WriteU64LE(out, (uint64_t)value);
}

static bool ReadU32LE(const unsigned char* data, size_t size, size_t* offset, uint32_t* out, std::string* error)
{
    if (*offset > size || size - *offset < 4)
        return SetError(error, "short read for uint32");
    const unsigned char* p = data + *offset;
    *out = ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    *offset += 4;
    return true;
}

static bool ReadI32LE(const unsigned char* data, size_t size, size_t* offset, int32_t* out, std::string* error)
{
    uint32_t tmp = 0;
    if (!ReadU32LE(data, size, offset, &tmp, error))
        return false;
    *out = (int32_t)tmp;
    return true;
}

static bool ReadU64LE(const unsigned char* data, size_t size, size_t* offset, uint64_t* out, std::string* error)
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

static bool ReadI64LE(const unsigned char* data, size_t size, size_t* offset, int64_t* out, std::string* error)
{
    uint64_t tmp = 0;
    if (!ReadU64LE(data, size, offset, &tmp, error))
        return false;
    *out = (int64_t)tmp;
    return true;
}

static unsigned char HexDigit(unsigned int value)
{
    return (unsigned char)(value < 10 ? ('0' + value) : ('a' + value - 10));
}

static bool ParseHexNibble(char c, unsigned char* out)
{
    if (c >= '0' && c <= '9')
    {
        *out = (unsigned char)(c - '0');
        return true;
    }
    if (c >= 'a' && c <= 'f')
    {
        *out = (unsigned char)(10 + (c - 'a'));
        return true;
    }
    if (c >= 'A' && c <= 'F')
    {
        *out = (unsigned char)(10 + (c - 'A'));
        return true;
    }
    return false;
}

static void WriteHashBytes(std::vector<unsigned char>& out, const uint256& hash)
{
    const std::string hex = hash.GetHex();
    for (size_t i = 0; i < hex.size(); i += 2)
    {
        unsigned char hi = 0;
        unsigned char lo = 0;
        ParseHexNibble(hex[i], &hi);
        ParseHexNibble(hex[i + 1], &lo);
        out.push_back((unsigned char)((hi << 4) | lo));
    }
}

static bool ReadHashBytes(const unsigned char* data, size_t size, size_t* offset, uint256* out, std::string* error)
{
    if (*offset > size || size - *offset < 32)
        return SetError(error, "short read for hash");
    std::string hex;
    hex.resize(64);
    for (size_t i = 0; i < 32; ++i)
    {
        const unsigned char value = data[*offset + i];
        hex[2 * i] = (char)HexDigit((value >> 4) & 0x0f);
        hex[2 * i + 1] = (char)HexDigit(value & 0x0f);
    }
    *out = uint256(hex);
    *offset += 32;
    return true;
}

static uint32_t EntryChecksum(const unsigned char* data, size_t size)
{
    return (uint32_t)crc32(0L, data, (uInt)size);
}

static bool CheckedAddMul(uint64_t base, uint64_t count, uint64_t unit, uint64_t* out)
{
    if (count != 0 && unit > std::numeric_limits<uint64_t>::max() / count)
        return false;
    const uint64_t scaled = count * unit;
    if (base > std::numeric_limits<uint64_t>::max() - scaled)
        return false;
    *out = base + scaled;
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

static bool EncodeDerivedHeader(const BlockIndexDerivedHeader& header, std::vector<unsigned char>* out)
{
    out->clear();
    out->insert(out->end(), BLOCK_INDEX_DERIVED_MAGIC, BLOCK_INDEX_DERIVED_MAGIC + 8);
    WriteU32LE(*out, header.formatVersion);
    WriteU32LE(*out, header.schemaVersion);
    WriteU32LE(*out, header.headerSize);
    WriteU32LE(*out, header.entrySize);
    WriteU64LE(*out, header.generation);
    WriteU64LE(*out, header.entryCount);
    WriteU64LE(*out, 0); // reserved
    return out->size() == BLOCK_INDEX_DERIVED_HEADER_SIZE_V1;
}

static bool DecodeDerivedHeader(const unsigned char* data, size_t size, BlockIndexDerivedHeader* header, std::string* error)
{
    if (size != BLOCK_INDEX_DERIVED_HEADER_SIZE_V1)
        return SetError(error, "derived header size mismatch");
    if (memcmp(data, BLOCK_INDEX_DERIVED_MAGIC, 8) != 0)
        return SetError(error, "invalid derived magic");
    size_t offset = 8;
    if (!ReadU32LE(data, size, &offset, &header->formatVersion, error) ||
        !ReadU32LE(data, size, &offset, &header->schemaVersion, error) ||
        !ReadU32LE(data, size, &offset, &header->headerSize, error) ||
        !ReadU32LE(data, size, &offset, &header->entrySize, error) ||
        !ReadU64LE(data, size, &offset, &header->generation, error) ||
        !ReadU64LE(data, size, &offset, &header->entryCount, error))
        return false;
    uint64_t reserved = 0;
    if (!ReadU64LE(data, size, &offset, &reserved, error))
        return false;
    if (reserved != 0)
        return SetError(error, "non-zero reserved derived header bytes");
    if (offset != size)
        return SetError(error, "derived header trailing bytes");
    ClearError(error);
    return true;
}

} // namespace

bool EncodeBlockIndexDerivedEntry(const BlockIndexDerivedEntry& entry,
                                  std::vector<unsigned char>* out,
                                  std::string* error)
{
    if (!out)
        return SetError(error, "null encode output");

    out->clear();
    out->reserve(BLOCK_INDEX_DERIVED_ENTRY_SIZE_V1);
    WriteHashBytes(*out, entry.chainTrust);
    WriteU32LE(*out, entry.stakeModifierChecksum);
    WriteI64LE(*out, entry.stakeModifierTime);
    WriteU32LE(*out, entry.flags);
    if (out->size() != BLOCK_INDEX_DERIVED_ENTRY_SIZE_V1 - 4)
        return SetError(error, "entry payload size mismatch during encode");
    WriteU32LE(*out, EntryChecksum(&(*out)[0], out->size()));
    if (out->size() != BLOCK_INDEX_DERIVED_ENTRY_SIZE_V1)
        return SetError(error, "entry size mismatch during encode");
    ClearError(error);
    return true;
}

bool DecodeBlockIndexDerivedEntry(const unsigned char* data, size_t size,
                                  BlockIndexDerivedEntry* out,
                                  std::string* error)
{
    if (!data || !out)
        return SetError(error, "null decode input/output");
    if (size != BLOCK_INDEX_DERIVED_ENTRY_SIZE_V1)
        return SetError(error, "entry size mismatch");

    const uint32_t checksum = EntryChecksum(data, BLOCK_INDEX_DERIVED_ENTRY_SIZE_V1 - 4);
    size_t checksumOffset = BLOCK_INDEX_DERIVED_ENTRY_SIZE_V1 - 4;
    uint32_t storedChecksum = 0;
    if (!ReadU32LE(data, size, &checksumOffset, &storedChecksum, error))
        return false;
    if (checksum != storedChecksum)
        return SetError(error, "derived entry checksum mismatch");

    size_t offset = 0;
    if (!ReadHashBytes(data, size, &offset, &out->chainTrust, error) ||
        !ReadU32LE(data, size, &offset, &out->stakeModifierChecksum, error) ||
        !ReadI64LE(data, size, &offset, &out->stakeModifierTime, error) ||
        !ReadU32LE(data, size, &offset, &out->flags, error))
        return false;
    if (offset != BLOCK_INDEX_DERIVED_ENTRY_SIZE_V1 - 4)
        return SetError(error, "entry payload trailing bytes");
    ClearError(error);
    return true;
}

struct BlockIndexDerivedStateStore::ReadHandle
{
    FILE* file;
    uint64_t fileSize;
    CCriticalSection cs;
    ReadHandle(FILE* f, uint64_t size) : file(f), fileSize(size) {}
    ~ReadHandle() { if (file) fclose(file); }
};

BlockIndexDerivedStateStore::BlockIndexDerivedStateStore()
    : generation(0), entryCount(0), writable(false), open(false)
{
}

bool BlockIndexDerivedStateStore::InitializePaths(const std::string& dir, std::string* error)
{
    dirPath = boost::filesystem::path(dir);
    if (dirPath.empty())
        return SetError(error, "empty derived state directory path");
    derivedPath = dirPath / BLOCK_INDEX_DERIVED_FILE_NAME;
    ClearError(error);
    return true;
}

bool BlockIndexDerivedStateStore::WriteHeader(std::string* error)
{
    BlockIndexDerivedHeader header;
    header.generation = generation;
    header.entryCount = entryCount;
    std::vector<unsigned char> bytes;
    EncodeDerivedHeader(header, &bytes);
    return WriteFileAtomically(derivedPath, bytes, error);
}

bool BlockIndexDerivedStateStore::LoadHeader(uint64_t* fileSize, std::string* error)
{
    if (!boost::filesystem::exists(derivedPath))
        return SetError(error, "missing derived.dat");
    const uint64_t size = boost::filesystem::file_size(derivedPath);
    if (size < BLOCK_INDEX_DERIVED_HEADER_SIZE_V1)
        return SetError(error, "truncated derived.dat header");
    std::vector<unsigned char> bytes;
    if (!ReadExactFile(derivedPath, &bytes, BLOCK_INDEX_DERIVED_HEADER_SIZE_V1, error))
        return false;
    BlockIndexDerivedHeader header;
    if (!DecodeDerivedHeader(&bytes[0], bytes.size(), &header, error))
        return false;
    if (!ValidateHeader(header, error))
        return false;
    generation = header.generation;
    entryCount = header.entryCount;
    if (fileSize)
        *fileSize = size;
    ClearError(error);
    return true;
}

bool BlockIndexDerivedStateStore::ValidateHeader(const BlockIndexDerivedHeader& header,
                                                  std::string* error) const
{
    if (header.formatVersion != BLOCK_INDEX_DERIVED_FORMAT_VERSION)
        return SetError(error, "unsupported derived format version");
    if (header.schemaVersion != BLOCK_INDEX_DERIVED_SCHEMA_VERSION)
        return SetError(error, "unsupported derived schema version");
    if (header.headerSize != BLOCK_INDEX_DERIVED_HEADER_SIZE_V1)
        return SetError(error, "invalid derived header size");
    if (header.entrySize != BLOCK_INDEX_DERIVED_ENTRY_SIZE_V1)
        return SetError(error, "invalid derived entry size");
    if (header.generation == 0)
        return SetError(error, "derived generation 0 is invalid");
    if (header.generation != generation)
        return SetError(error, "derived generation mismatch");
    ClearError(error);
    return true;
}

bool BlockIndexDerivedStateStore::Create(const std::string& dir, uint64_t gen,
                                          BlockIndexDerivedStateStore* out,
                                          std::string* error)
{
    if (!out)
        return SetError(error, "null output store");
    if (gen == 0)
        return SetError(error, "generation 0 is invalid");
    BlockIndexDerivedStateStore store;
    if (!store.InitializePaths(dir, error))
        return false;
    boost::filesystem::create_directories(store.dirPath);
    if (boost::filesystem::exists(store.derivedPath))
        return SetError(error, "derived.dat already exists");
    store.generation = gen;
    store.entryCount = 0;
    store.writable = true;
    store.open = true;
    if (!store.WriteHeader(error))
        return false;
    *out = store;
    ClearError(error);
    return true;
}

bool BlockIndexDerivedStateStore::OpenReadOnly(const std::string& dir, uint64_t expectedGeneration,
                                                BlockIndexDerivedStateStore* out,
                                                std::string* error)
{
    if (!out)
        return SetError(error, "null output store");
    BlockIndexDerivedStateStore store;
    if (!store.InitializePaths(dir, error))
        return false;
    store.generation = expectedGeneration;
    store.open = true;
    store.writable = false;

    uint64_t fileSize = 0;
    if (!store.LoadHeader(&fileSize, error))
        return false;

    // Validate entry region
    uint64_t entryEnd = 0;
    if (!CheckedAddMul(BLOCK_INDEX_DERIVED_HEADER_SIZE_V1, store.entryCount,
                       BLOCK_INDEX_DERIVED_ENTRY_SIZE_V1, &entryEnd))
        return SetError(error, "derived entry region overflow");
    if (fileSize < entryEnd)
        return SetError(error, "derived.dat truncated within entry region");

    FILE* readFile = fopen(store.derivedPath.string().c_str(), "rb");
    if (!readFile)
        return SetError(error, "persistent read open failed: " + store.derivedPath.string());
    store.readHandle.reset(new ReadHandle(readFile, fileSize));
    *out = store;
    ClearError(error);
    return true;
}

bool BlockIndexDerivedStateStore::Append(const BlockIndexDerivedEntry& entry,
                                          std::string* error)
{
    if (!open || !writable)
        return SetError(error, "store is not writable");
    if (entryCount == std::numeric_limits<uint64_t>::max())
        return SetError(error, "entry count exhausted");

    std::vector<unsigned char> encoded;
    if (!EncodeBlockIndexDerivedEntry(entry, &encoded, error))
        return false;

    FILE* file = fopen(derivedPath.string().c_str(), "ab");
    if (!file)
        return SetError(error, "append open failed: " + derivedPath.string());
    if (!encoded.empty() && fwrite(&encoded[0], 1, encoded.size(), file) != encoded.size())
    {
        fclose(file);
        return SetError(error, "append write failed: " + derivedPath.string());
    }
    FileCommit(file);
    fclose(file);

    ++entryCount;
    ClearError(error);
    return true;
}

bool BlockIndexDerivedStateStore::AppendBatch(const std::vector<BlockIndexDerivedEntry>& entries,
                                               std::string* error)
{
    if (!open || !writable)
        return SetError(error, "store is not writable");
    if (entries.empty())
        return SetError(error, "empty batch append");
    if (entryCount == std::numeric_limits<uint64_t>::max())
        return SetError(error, "entry count exhausted");
    if (entries.size() > std::numeric_limits<uint64_t>::max() - entryCount)
        return SetError(error, "entry count exhausted by batch");

    std::vector<unsigned char> bytes;
    bytes.reserve(entries.size() * BLOCK_INDEX_DERIVED_ENTRY_SIZE_V1);
    for (size_t i = 0; i < entries.size(); ++i)
    {
        std::vector<unsigned char> encoded;
        if (!EncodeBlockIndexDerivedEntry(entries[i], &encoded, error))
            return false;
        bytes.insert(bytes.end(), encoded.begin(), encoded.end());
    }

    FILE* file = fopen(derivedPath.string().c_str(), "ab");
    if (!file)
        return SetError(error, "append open failed: " + derivedPath.string());
    if (!bytes.empty() && fwrite(&bytes[0], 1, bytes.size(), file) != bytes.size())
    {
        fclose(file);
        return SetError(error, "append write failed: " + derivedPath.string());
    }
    FileCommit(file);
    fclose(file);

    entryCount += entries.size();
    ClearError(error);
    return true;
}

BlockIndexDerivedLookupStatus BlockIndexDerivedStateStore::Read(BlockIndexId id,
                                                                 BlockIndexDerivedEntry* out,
                                                                 std::string* error) const
{
    if (!open)
    {
        SetError(error, "store is not open");
        return BLOCK_INDEX_DERIVED_LOOKUP_NOT_OPEN;
    }
    if (!out)
    {
        SetError(error, "null entry output");
        return BLOCK_INDEX_DERIVED_LOOKUP_IO_ERROR;
    }
    if (id == BLOCK_INDEX_ID_INVALID || id > entryCount)
    {
        *out = BlockIndexDerivedEntry();
        ClearError(error);
        return BLOCK_INDEX_DERIVED_LOOKUP_NOT_FOUND;
    }

    uint64_t offset = 0;
    if (!CheckedAddMul(BLOCK_INDEX_DERIVED_HEADER_SIZE_V1, id - 1,
                       BLOCK_INDEX_DERIVED_ENTRY_SIZE_V1, &offset))
    {
        SetError(error, "entry offset overflow");
        return BLOCK_INDEX_DERIVED_LOOKUP_IO_ERROR;
    }
    uint64_t entryEnd = 0;
    if (!CheckedAddMul(offset, 1, BLOCK_INDEX_DERIVED_ENTRY_SIZE_V1, &entryEnd))
    {
        SetError(error, "entry end overflow");
        return BLOCK_INDEX_DERIVED_LOOKUP_IO_ERROR;
    }
    if (!readHandle)
    {
        SetError(error, "persistent read handle missing");
        return BLOCK_INDEX_DERIVED_LOOKUP_IO_ERROR;
    }
    if (readHandle->fileSize < entryEnd)
    {
        SetError(error, "truncated entry bytes");
        return BLOCK_INDEX_DERIVED_LOOKUP_CORRUPT;
    }

    LOCK(readHandle->cs);
    if (fseeko(readHandle->file, (off_t)offset, SEEK_SET) != 0)
    {
        SetError(error, "seek failed for entry read");
        return BLOCK_INDEX_DERIVED_LOOKUP_IO_ERROR;
    }
    std::vector<unsigned char> bytes(BLOCK_INDEX_DERIVED_ENTRY_SIZE_V1, 0);
    const size_t n = fread(&bytes[0], 1, bytes.size(), readHandle->file);
    if (n != bytes.size())
    {
        SetError(error, "short entry read");
        return BLOCK_INDEX_DERIVED_LOOKUP_IO_ERROR;
    }
    if (!DecodeBlockIndexDerivedEntry(&bytes[0], bytes.size(), out, error))
        return BLOCK_INDEX_DERIVED_LOOKUP_CORRUPT;
    ClearError(error);
    return BLOCK_INDEX_DERIVED_LOOKUP_FOUND;
}

bool BlockIndexDerivedStateStore::Finalize(std::string* error)
{
    if (!open || !writable)
        return SetError(error, "store is not writable");
    // Rewrite header in-place with final entry count (the header is at offset 0
    // and is exactly BLOCK_INDEX_DERIVED_HEADER_SIZE_V1 bytes; the entry data
    // follows immediately and must not be disturbed).
    BlockIndexDerivedHeader header;
    header.generation = generation;
    header.entryCount = entryCount;
    std::vector<unsigned char> bytes;
    EncodeDerivedHeader(header, &bytes);

    FILE* file = fopen(derivedPath.string().c_str(), "r+b");
    if (!file)
        return SetError(error, "finalize open failed: " + derivedPath.string());
    if (fwrite(&bytes[0], 1, bytes.size(), file) != bytes.size())
    {
        fclose(file);
        return SetError(error, "finalize write failed: " + derivedPath.string());
    }
    FileCommit(file);
    fclose(file);
    ClearError(error);
    return true;
}

uint64_t BlockIndexDerivedStateStore::EntryCount() const
{
    return entryCount;
}

uint64_t BlockIndexDerivedStateStore::Generation() const
{
    return generation;
}

bool BlockIndexDerivedStateStore::IsOpen() const
{
    return open;
}
