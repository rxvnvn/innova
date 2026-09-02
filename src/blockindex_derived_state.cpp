// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "blockindex_derived_state.h"

#include "util.h"

#include <openssl/sha.h>
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

static void WriteU64LE(std::vector<unsigned char>& out, uint64_t value)
{
    for (int i = 0; i < 8; ++i)
        out.push_back((unsigned char)((value >> (8 * i)) & 0xff));
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

// TRUE little-endian uint256: raw 32 bytes in memory order.
// uint256 stores its data as uint32_t pn[8] in host byte order (which is LE
// on all supported platforms). The raw byte representation IS true LE.
static void WriteHashBytesLE(std::vector<unsigned char>& out, const uint256& hash)
{
    // uint256::begin() returns pointer to the raw 32-byte representation
    const unsigned char* raw = hash.begin();
    out.insert(out.end(), raw, raw + 32);
}

static bool ReadHashBytesLE(const unsigned char* data, size_t size, size_t* offset, uint256* out, std::string* error)
{
    if (*offset > size || size - *offset < 32)
        return SetError(error, "short read for hash");
    // uint256 can be constructed from raw bytes via begin() mutation
    memcpy(out->begin(), data + *offset, 32);
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

static bool EncodeDerivedHeaderV2(const BlockIndexDerivedHeader& header, std::vector<unsigned char>* out)
{
    out->clear();
    out->insert(out->end(), BLOCK_INDEX_DERIVED_MAGIC, BLOCK_INDEX_DERIVED_MAGIC + 8);
    WriteU32LE(*out, header.formatVersion);
    WriteU32LE(*out, header.schemaVersion);
    WriteU32LE(*out, header.headerSize);
    WriteU32LE(*out, header.entrySize);
    WriteU64LE(*out, header.generation);
    WriteU64LE(*out, header.entryCount);
    // content binding: 32 raw bytes
    out->insert(out->end(), header.contentBinding, header.contentBinding + 32);
    WriteU64LE(*out, 0); // reserved
    return out->size() == BLOCK_INDEX_DERIVED_HEADER_SIZE_V2;
}

static bool DecodeDerivedHeader(const unsigned char* data, size_t size, BlockIndexDerivedHeader* header, std::string* error)
{
    if (size < 8)
        return SetError(error, "derived header too short for magic");
    if (memcmp(data, BLOCK_INDEX_DERIVED_MAGIC, 8) != 0)
        return SetError(error, "invalid derived magic");

    // Read format/schema versions first to determine header layout
    if (size < 24)
        return SetError(error, "derived header too short for version");
    size_t offset = 8;
    if (!ReadU32LE(data, size, &offset, &header->formatVersion, error) ||
        !ReadU32LE(data, size, &offset, &header->schemaVersion, error) ||
        !ReadU32LE(data, size, &offset, &header->headerSize, error) ||
        !ReadU32LE(data, size, &offset, &header->entrySize, error))
        return false;

    // V2 format only
    if (header->formatVersion != BLOCK_INDEX_DERIVED_FORMAT_VERSION)
        return SetError(error, "unsupported derived format version (expected V2)");
    if (header->schemaVersion != BLOCK_INDEX_DERIVED_SCHEMA_VERSION)
        return SetError(error, "unsupported derived schema version (expected V2)");
    if (header->headerSize != BLOCK_INDEX_DERIVED_HEADER_SIZE_V2)
        return SetError(error, "invalid derived header size (expected 80)");
    if (header->entrySize != BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2)
        return SetError(error, "invalid derived entry size (expected 56)");
    if (size != BLOCK_INDEX_DERIVED_HEADER_SIZE_V2)
        return SetError(error, "derived header exact size mismatch");

    if (!ReadU64LE(data, size, &offset, &header->generation, error) ||
        !ReadU64LE(data, size, &offset, &header->entryCount, error))
        return false;

    // content binding: 32 raw bytes
    if (size - offset < 32)
        return SetError(error, "derived header too short for content binding");
    memcpy(header->contentBinding, data + offset, 32);
    offset += 32;

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

bool ComputeDerivedContentBinding(const uint256& tipHash,
                                  uint64_t recordCount,
                                  uint64_t generation,
                                  unsigned char binding[32])
{
    // SHA256(tipHash || recordCount || generation) in true LE byte order
    SHA256_CTX ctx;
    SHA256_Init(&ctx);

    // tipHash: raw 32 bytes (true LE)
    SHA256_Update(&ctx, tipHash.begin(), 32);

    // recordCount: LE 8 bytes
    unsigned char rcBuf[8];
    for (int i = 0; i < 8; ++i)
        rcBuf[i] = (unsigned char)((recordCount >> (8 * i)) & 0xff);
    SHA256_Update(&ctx, rcBuf, 8);

    // generation: LE 8 bytes
    unsigned char genBuf[8];
    for (int i = 0; i < 8; ++i)
        genBuf[i] = (unsigned char)((generation >> (8 * i)) & 0xff);
    SHA256_Update(&ctx, genBuf, 8);

    SHA256_Final(binding, &ctx);
    return true;
}

bool ComputeGenerationRoot(uint64_t generation,
                           const uint256& tipHash,
                           uint64_t recordCount,
                           const unsigned char recordsDigest[32],
                           const unsigned char activeDigest[32],
                           const unsigned char hashIndexDigest[32],
                           const unsigned char derivedEntriesDigest[32],
                           const unsigned char dagInputDigest[32],
                           unsigned char root[32])
{
    // SHA256(domain || version || generation || tip || count || component digests)
    SHA256_CTX ctx;
    SHA256_Init(&ctx);

    // Domain separator: "GENROOT" + null byte
    static const unsigned char domain[8] = {'G','E','N','R','O','O','T','\0'};
    SHA256_Update(&ctx, domain, 8);

    // Version: uint32 LE = 1
    unsigned char verBuf[4];
    verBuf[0] = 1; verBuf[1] = 0; verBuf[2] = 0; verBuf[3] = 0;
    SHA256_Update(&ctx, verBuf, 4);

    // Generation: uint64 LE
    unsigned char genBuf[8];
    for (int i = 0; i < 8; ++i)
        genBuf[i] = (unsigned char)((generation >> (8 * i)) & 0xff);
    SHA256_Update(&ctx, genBuf, 8);

    // Tip hash: raw 32 bytes
    SHA256_Update(&ctx, tipHash.begin(), 32);

    // Record count: uint64 LE
    unsigned char rcBuf[8];
    for (int i = 0; i < 8; ++i)
        rcBuf[i] = (unsigned char)((recordCount >> (8 * i)) & 0xff);
    SHA256_Update(&ctx, rcBuf, 8);

    // Component digests (each 32 bytes)
    SHA256_Update(&ctx, recordsDigest, 32);
    SHA256_Update(&ctx, activeDigest, 32);
    SHA256_Update(&ctx, hashIndexDigest, 32);
    SHA256_Update(&ctx, derivedEntriesDigest, 32);
    SHA256_Update(&ctx, dagInputDigest, 32);

    SHA256_Final(root, &ctx);
    return true;
}

bool EncodeBlockIndexDerivedEntry(const BlockIndexDerivedEntry& entry,
                                  std::vector<unsigned char>* out,
                                  std::string* error)
{
    if (!out)
        return SetError(error, "null encode output");

    out->clear();
    out->reserve(BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2);
    WriteHashBytesLE(*out, entry.chainTrust);           // [0..32)
    WriteU32LE(*out, entry.stakeModifierChecksum);       // [32..36)
    {
        // int64 -> uint64 cast for LE encoding
        std::vector<unsigned char> tmp;
        WriteU64LE(tmp, (uint64_t)entry.stakeModifierTime);
        out->insert(out->end(), tmp.begin(), tmp.end()); // [36..44)
    }
    WriteU32LE(*out, entry.nSize);                       // [44..48)
    WriteU32LE(*out, entry.flags);                       // [48..52)
    if (out->size() != BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2 - 4)
        return SetError(error, "entry payload size mismatch during encode");
    WriteU32LE(*out, EntryChecksum(&(*out)[0], out->size())); // [52..56)
    if (out->size() != BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2)
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
    if (size != BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2)
        return SetError(error, "entry size mismatch");

    const uint32_t checksum = EntryChecksum(data, BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2 - 4);
    size_t checksumOffset = BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2 - 4;
    uint32_t storedChecksum = 0;
    if (!ReadU32LE(data, size, &checksumOffset, &storedChecksum, error))
        return false;
    if (checksum != storedChecksum)
        return SetError(error, "derived entry checksum mismatch");

    size_t offset = 0;
    if (!ReadHashBytesLE(data, size, &offset, &out->chainTrust, error) ||
        !ReadU32LE(data, size, &offset, &out->stakeModifierChecksum, error) ||
        !ReadI64LE(data, size, &offset, &out->stakeModifierTime, error) ||
        !ReadU32LE(data, size, &offset, &out->nSize, error) ||
        !ReadU32LE(data, size, &offset, &out->flags, error))
        return false;
    if (offset != BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2 - 4)
        return SetError(error, "entry payload trailing bytes");

    // Validate unknown flags: reject if any unknown flag bits are set
    if (out->flags & ~BLOCK_INDEX_DERIVED_FLAG_KNOWN_MASK)
        return SetError(error, "derived entry has unknown flag bits");

    // Canonicalize unavailable values: if flag not set, value must be 0
    if (!out->HasStakeModifierTime() && out->stakeModifierTime != 0)
        return SetError(error, "derived entry: modifier time not available but value nonzero");
    if (!out->HasBlockSize() && out->nSize != 0)
        return SetError(error, "derived entry: block size not available but value nonzero");

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
    : generation(0), entryCount(0), formatVersion(0), schemaVersion(0),
      writable(false), open(false)
{
    memset(contentBinding, 0, 32);
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
    memcpy(header.contentBinding, contentBinding, 32);
    std::vector<unsigned char> bytes;
    EncodeDerivedHeaderV2(header, &bytes);
    return WriteFileAtomically(derivedPath, bytes, error);
}

bool BlockIndexDerivedStateStore::LoadHeader(uint64_t* fileSize, std::string* error)
{
    if (!boost::filesystem::exists(derivedPath))
        return SetError(error, "missing derived.dat");
    const uint64_t size = boost::filesystem::file_size(derivedPath);
    if (size < BLOCK_INDEX_DERIVED_HEADER_SIZE_V2)
        return SetError(error, "truncated derived.dat header");
    std::vector<unsigned char> bytes;
    if (!ReadExactFile(derivedPath, &bytes, BLOCK_INDEX_DERIVED_HEADER_SIZE_V2, error))
        return false;
    BlockIndexDerivedHeader header;
    if (!DecodeDerivedHeader(&bytes[0], bytes.size(), &header, error))
        return false;
    if (!ValidateHeader(header, error))
        return false;
    generation = header.generation;
    entryCount = header.entryCount;
    memcpy(contentBinding, header.contentBinding, 32);
    formatVersion = header.formatVersion;
    schemaVersion = header.schemaVersion;
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
    if (header.headerSize != BLOCK_INDEX_DERIVED_HEADER_SIZE_V2)
        return SetError(error, "invalid derived header size");
    if (header.entrySize != BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2)
        return SetError(error, "invalid derived entry size");
    if (header.generation == 0)
        return SetError(error, "derived generation 0 is invalid");
    if (header.generation != generation)
        return SetError(error, "derived generation mismatch");
    ClearError(error);
    return true;
}

bool BlockIndexDerivedStateStore::Create(const std::string& dir, uint64_t gen,
                                          const unsigned char binding[32],
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
    if (binding)
        memcpy(store.contentBinding, binding, 32);
    else
        memset(store.contentBinding, 0, 32);
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
    if (!CheckedAddMul(BLOCK_INDEX_DERIVED_HEADER_SIZE_V2, store.entryCount,
                       BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2, &entryEnd))
        return SetError(error, "derived entry region overflow");
    if (fileSize < entryEnd)
        return SetError(error, "derived.dat truncated within entry region");
    // Exact size check: reject trailing data
    if (fileSize > entryEnd)
        return SetError(error, "derived.dat has trailing data beyond entry region");

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
    bytes.reserve(entries.size() * BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2);
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
    if (!CheckedAddMul(BLOCK_INDEX_DERIVED_HEADER_SIZE_V2, id - 1,
                       BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2, &offset))
    {
        SetError(error, "entry offset overflow");
        return BLOCK_INDEX_DERIVED_LOOKUP_IO_ERROR;
    }
    uint64_t entryEnd = 0;
    if (!CheckedAddMul(offset, 1, BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2, &entryEnd))
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
    std::vector<unsigned char> bytes(BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2, 0);
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
    BlockIndexDerivedHeader header;
    header.generation = generation;
    header.entryCount = entryCount;
    memcpy(header.contentBinding, contentBinding, 32);
    std::vector<unsigned char> bytes;
    EncodeDerivedHeaderV2(header, &bytes);

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

uint32_t BlockIndexDerivedStateStore::FormatVersion() const
{
    return formatVersion;
}

uint32_t BlockIndexDerivedStateStore::SchemaVersion() const
{
    return schemaVersion;
}

bool BlockIndexDerivedStateStore::ValidateContentBinding(const uint256& expectedTipHash,
                                                          uint64_t expectedRecordCount,
                                                          std::string* error) const
{
    unsigned char expected[32];
    ComputeDerivedContentBinding(expectedTipHash, expectedRecordCount, generation, expected);
    if (memcmp(contentBinding, expected, 32) != 0)
        return SetError(error, "derived content binding mismatch");
    ClearError(error);
    return true;
}

void BlockIndexDerivedStateStore::SetContentBinding(const unsigned char binding[32])
{
    if (binding)
        memcpy(contentBinding, binding, 32);
    else
        memset(contentBinding, 0, 32);
}

void BlockIndexDerivedStateStore::GetContentBinding(unsigned char out[32]) const
{
    if (out)
        memcpy(out, contentBinding, 32);
}
