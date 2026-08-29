#include "fixed_blockindex_store.h"

#include "util.h"

#include <zlib.h>

#include <algorithm>
#include <limits>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace {

static const unsigned char BLOCK_INDEX_RECORDS_MAGIC[8] = {'I','N','N','B','R','E','C','1'};
static const unsigned char BLOCK_INDEX_MANIFEST_MAGIC[8] = {'I','N','N','B','M','A','N','1'};

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

static uint32_t RecordChecksum(const unsigned char* data, size_t size)
{
    return (uint32_t)crc32(0L, data, (uInt)size);
}

static bool ValidateRecord(const BlockIndexRecord& record, std::string* error)
{
    if (record.hash == 0)
        return SetError(error, "record hash must be non-zero");
    if (record.height < 0)
        return SetError(error, "record height must be non-negative");
    if (record.height == 0 && record.hashPrev != 0)
        return SetError(error, "genesis record must have null hashPrev");
    if (!(record.nFlags & CBlockIndex::BLOCK_PROOF_OF_STAKE))
    {
        if (!record.prevoutStake.IsNull())
            return SetError(error, "proof-of-work record must not carry prevoutStake");
        if (record.nStakeTime != 0)
            return SetError(error, "proof-of-work record must have zero nStakeTime");
    }
    else if (record.nStakeTime == 0)
    {
        return SetError(error, "proof-of-stake record must have non-zero nStakeTime");
    }
    return true;
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

static bool EncodeRecordsHeader(uint64_t generation, std::vector<unsigned char>* out)
{
    out->clear();
    out->insert(out->end(), BLOCK_INDEX_RECORDS_MAGIC, BLOCK_INDEX_RECORDS_MAGIC + 8);
    WriteU32LE(*out, BLOCK_INDEX_FORMAT_VERSION);
    WriteU32LE(*out, BLOCK_INDEX_RECORD_VERSION);
    WriteU32LE(*out, BLOCK_INDEX_RECORDS_HEADER_SIZE_V1);
    WriteU32LE(*out, BLOCK_INDEX_RECORD_SIZE_V1);
    WriteU64LE(*out, generation);
    WriteU64LE(*out, 0);
    return out->size() == BLOCK_INDEX_RECORDS_HEADER_SIZE_V1;
}

static bool DecodeRecordsHeader(const unsigned char* data, size_t size, uint64_t* generation, std::string* error)
{
    if (size != BLOCK_INDEX_RECORDS_HEADER_SIZE_V1)
        return SetError(error, "records header size mismatch");
    if (memcmp(data, BLOCK_INDEX_RECORDS_MAGIC, 8) != 0)
        return SetError(error, "invalid records magic");
    size_t offset = 8;
    uint32_t formatVersion = 0;
    uint32_t recordVersion = 0;
    uint32_t headerSize = 0;
    uint32_t recordSize = 0;
    uint64_t reserved = 0;
    if (!ReadU32LE(data, size, &offset, &formatVersion, error) ||
        !ReadU32LE(data, size, &offset, &recordVersion, error) ||
        !ReadU32LE(data, size, &offset, &headerSize, error) ||
        !ReadU32LE(data, size, &offset, &recordSize, error) ||
        !ReadU64LE(data, size, &offset, generation, error) ||
        !ReadU64LE(data, size, &offset, &reserved, error))
        return false;
    if (formatVersion != BLOCK_INDEX_FORMAT_VERSION)
        return SetError(error, "unsupported records format version");
    if (recordVersion != BLOCK_INDEX_RECORD_VERSION)
        return SetError(error, "unsupported records record version");
    if (headerSize != BLOCK_INDEX_RECORDS_HEADER_SIZE_V1)
        return SetError(error, "invalid records header size");
    if (recordSize != BLOCK_INDEX_RECORD_SIZE_V1)
        return SetError(error, "invalid records record size");
    if (reserved != 0)
        return SetError(error, "non-zero reserved records header bytes");
    if (offset != size)
        return SetError(error, "records header trailing bytes");
    ClearError(error);
    return true;
}

static bool EncodeManifestV1(const FixedBlockIndexManifest& manifest, std::vector<unsigned char>* out, std::string* error)
{
    out->clear();
    out->insert(out->end(), BLOCK_INDEX_MANIFEST_MAGIC, BLOCK_INDEX_MANIFEST_MAGIC + 8);
    WriteU32LE(*out, manifest.formatVersion);
    WriteU32LE(*out, manifest.recordVersion);
    WriteU32LE(*out, manifest.manifestSize);
    WriteU32LE(*out, manifest.recordSize);
    WriteU64LE(*out, manifest.generation);
    WriteU64LE(*out, manifest.recordCount);
    WriteU64LE(*out, manifest.committedTipId);
    WriteI32LE(*out, manifest.committedTipHeight);
    WriteU32LE(*out, manifest.state);
    WriteHashBytes(*out, manifest.committedTipHash);
    if (out->size() != BLOCK_INDEX_MANIFEST_SIZE_V1)
        return SetError(error, "manifest size mismatch during encode");
    ClearError(error);
    return true;
}

static bool DecodeManifestV1(const unsigned char* data, size_t size, FixedBlockIndexManifest* manifest, std::string* error)
{
    if (size != BLOCK_INDEX_MANIFEST_SIZE_V1)
        return SetError(error, "manifest size mismatch");
    if (memcmp(data, BLOCK_INDEX_MANIFEST_MAGIC, 8) != 0)
        return SetError(error, "invalid manifest magic");
    size_t offset = 8;
    if (!ReadU32LE(data, size, &offset, &manifest->formatVersion, error) ||
        !ReadU32LE(data, size, &offset, &manifest->recordVersion, error) ||
        !ReadU32LE(data, size, &offset, &manifest->manifestSize, error) ||
        !ReadU32LE(data, size, &offset, &manifest->recordSize, error) ||
        !ReadU64LE(data, size, &offset, &manifest->generation, error) ||
        !ReadU64LE(data, size, &offset, &manifest->recordCount, error) ||
        !ReadU64LE(data, size, &offset, &manifest->committedTipId, error) ||
        !ReadI32LE(data, size, &offset, &manifest->committedTipHeight, error) ||
        !ReadU32LE(data, size, &offset, &manifest->state, error) ||
        !ReadHashBytes(data, size, &offset, &manifest->committedTipHash, error))
        return false;
    if (offset != size)
        return SetError(error, "manifest trailing bytes");
    ClearError(error);
    return true;
}

} // namespace

bool EncodeBlockIndexRecordV1(const BlockIndexRecord& record, std::vector<unsigned char>* out, std::string* error)
{
    if (!out)
        return SetError(error, "null encode output");
    if (!ValidateRecord(record, error))
        return false;

    out->clear();
    out->reserve(BLOCK_INDEX_RECORD_SIZE_V1);
    WriteHashBytes(*out, record.hash);
    WriteHashBytes(*out, record.hashPrev);
    WriteHashBytes(*out, record.hashMerkleRoot);
    WriteHashBytes(*out, record.hashProof);
    WriteHashBytes(*out, record.prevoutStake.hash);
    WriteI32LE(*out, record.height);
    WriteU32LE(*out, record.nFile);
    WriteU32LE(*out, record.nBlockPos);
    WriteU32LE(*out, record.nFlags);
    WriteI32LE(*out, record.nVersion);
    WriteU32LE(*out, record.nTime);
    WriteU32LE(*out, record.nBits);
    WriteU32LE(*out, record.nNonce);
    WriteI64LE(*out, record.nMint);
    WriteI64LE(*out, record.nMoneySupply);
    WriteU64LE(*out, record.nStakeModifier);
    WriteU32LE(*out, record.prevoutStake.n);
    WriteU32LE(*out, record.nStakeTime);
    if (out->size() != BLOCK_INDEX_RECORD_SIZE_V1 - 4)
        return SetError(error, "record payload size mismatch during encode");
    WriteU32LE(*out, RecordChecksum(&(*out)[0], out->size()));
    if (out->size() != BLOCK_INDEX_RECORD_SIZE_V1)
        return SetError(error, "record size mismatch during encode");
    ClearError(error);
    return true;
}

bool DecodeBlockIndexRecordV1(const unsigned char* data, size_t size, BlockIndexRecord* out, std::string* error)
{
    if (!data || !out)
        return SetError(error, "null decode input/output");
    if (size != BLOCK_INDEX_RECORD_SIZE_V1)
        return SetError(error, "record size mismatch");
    const uint32_t checksum = RecordChecksum(data, BLOCK_INDEX_RECORD_SIZE_V1 - 4);
    size_t checksumOffset = BLOCK_INDEX_RECORD_SIZE_V1 - 4;
    uint32_t storedChecksum = 0;
    if (!ReadU32LE(data, size, &checksumOffset, &storedChecksum, error))
        return false;
    if (checksum != storedChecksum)
        return SetError(error, "record checksum mismatch");

    size_t offset = 0;
    if (!ReadHashBytes(data, size, &offset, &out->hash, error) ||
        !ReadHashBytes(data, size, &offset, &out->hashPrev, error) ||
        !ReadHashBytes(data, size, &offset, &out->hashMerkleRoot, error) ||
        !ReadHashBytes(data, size, &offset, &out->hashProof, error) ||
        !ReadHashBytes(data, size, &offset, &out->prevoutStake.hash, error) ||
        !ReadI32LE(data, size, &offset, &out->height, error) ||
        !ReadU32LE(data, size, &offset, &out->nFile, error) ||
        !ReadU32LE(data, size, &offset, &out->nBlockPos, error) ||
        !ReadU32LE(data, size, &offset, &out->nFlags, error) ||
        !ReadI32LE(data, size, &offset, &out->nVersion, error) ||
        !ReadU32LE(data, size, &offset, &out->nTime, error) ||
        !ReadU32LE(data, size, &offset, &out->nBits, error) ||
        !ReadU32LE(data, size, &offset, &out->nNonce, error) ||
        !ReadI64LE(data, size, &offset, &out->nMint, error) ||
        !ReadI64LE(data, size, &offset, &out->nMoneySupply, error) ||
        !ReadU64LE(data, size, &offset, &out->nStakeModifier, error) ||
        !ReadU32LE(data, size, &offset, &out->prevoutStake.n, error) ||
        !ReadU32LE(data, size, &offset, &out->nStakeTime, error))
        return false;
    if (offset != BLOCK_INDEX_RECORD_SIZE_V1 - 4)
        return SetError(error, "record payload trailing bytes");
    if (!ValidateRecord(*out, error))
        return false;
    ClearError(error);
    return true;
}

FixedBlockIndexStore::FixedBlockIndexStore()
    : generation(0),
      physicalRecordCount(0),
      writable(false),
      open(false)
{
}

bool FixedBlockIndexStore::InitializePaths(const std::string& dir, std::string* error)
{
    dirPath = boost::filesystem::path(dir);
    if (dirPath.empty())
        return SetError(error, "empty blockindex directory path");
    recordsPath = dirPath / BLOCK_INDEX_RECORDS_FILE_NAME;
    manifestPath = dirPath / BLOCK_INDEX_MANIFEST_FILE_NAME;
    ClearError(error);
    return true;
}

bool FixedBlockIndexStore::WriteRecordsHeader(std::string* error)
{
    std::vector<unsigned char> bytes;
    EncodeRecordsHeader(generation, &bytes);
    return WriteFileAtomically(recordsPath, bytes, error);
}

bool FixedBlockIndexStore::LoadRecordsHeader(uint64_t* fileSize, std::string* error)
{
    if (!boost::filesystem::exists(recordsPath))
        return SetError(error, "missing records.dat");
    const uint64_t size = boost::filesystem::file_size(recordsPath);
    if (size < BLOCK_INDEX_RECORDS_HEADER_SIZE_V1)
        return SetError(error, "truncated records.dat header");
    std::vector<unsigned char> bytes;
    if (!ReadExactFile(recordsPath, &bytes, BLOCK_INDEX_RECORDS_HEADER_SIZE_V1, error))
        return false;
    uint64_t fileGeneration = 0;
    if (!DecodeRecordsHeader(&bytes[0], bytes.size(), &fileGeneration, error))
        return false;
    if (fileGeneration != generation)
        return SetError(error, "generation mismatch between store and records.dat");
    const uint64_t bodyBytes = size - BLOCK_INDEX_RECORDS_HEADER_SIZE_V1;
    physicalRecordCount = bodyBytes / BLOCK_INDEX_RECORD_SIZE_V1;
    if (fileSize)
        *fileSize = size;
    ClearError(error);
    return true;
}

bool FixedBlockIndexStore::ValidateManifest(const FixedBlockIndexManifest& candidate, std::string* error) const
{
    if (candidate.formatVersion != BLOCK_INDEX_FORMAT_VERSION)
        return SetError(error, "unsupported manifest format version");
    if (candidate.recordVersion != BLOCK_INDEX_RECORD_VERSION)
        return SetError(error, "unsupported manifest record version");
    if (candidate.manifestSize != BLOCK_INDEX_MANIFEST_SIZE_V1)
        return SetError(error, "invalid manifest size");
    if (candidate.recordSize != BLOCK_INDEX_RECORD_SIZE_V1)
        return SetError(error, "invalid manifest record size");
    if (candidate.generation != generation)
        return SetError(error, "manifest generation mismatch");
    if (candidate.state != BLOCK_INDEX_MANIFEST_BUILDING && candidate.state != BLOCK_INDEX_MANIFEST_COMPLETE)
        return SetError(error, "invalid manifest state");
    if (candidate.recordCount > physicalRecordCount)
        return SetError(error, "manifest recordCount exceeds physical record count");
    if (candidate.recordCount == 0)
    {
        if (candidate.committedTipId != BLOCK_INDEX_ID_INVALID)
            return SetError(error, "empty manifest must have invalid committedTipId");
        if (candidate.committedTipHeight != -1)
            return SetError(error, "empty manifest must have committedTipHeight -1");
        if (candidate.committedTipHash != 0)
            return SetError(error, "empty manifest must have null committedTipHash");
    }
    else
    {
        if (candidate.committedTipId == BLOCK_INDEX_ID_INVALID)
            return SetError(error, "non-empty manifest must have committedTipId");
        if (candidate.committedTipId > candidate.recordCount)
            return SetError(error, "committedTipId exceeds recordCount");
        if (candidate.committedTipHeight < 0)
            return SetError(error, "non-empty manifest must have non-negative committedTipHeight");
        if (candidate.committedTipHash == 0)
            return SetError(error, "non-empty manifest must have non-zero committedTipHash");
    }
    ClearError(error);
    return true;
}

bool FixedBlockIndexStore::ValidateCommittedRegion(uint64_t fileSize, std::string* error) const
{
    uint64_t committedEnd = 0;
    if (!CheckedAddMul(BLOCK_INDEX_RECORDS_HEADER_SIZE_V1, manifest.recordCount, BLOCK_INDEX_RECORD_SIZE_V1, &committedEnd))
        return SetError(error, "committed record range overflow");
    if (fileSize < committedEnd)
        return SetError(error, "records.dat truncated within committed region");
    ClearError(error);
    return true;
}

bool FixedBlockIndexStore::LoadManifest(const FixedBlockIndexOpenOptions& options, std::string* error)
{
    if (!boost::filesystem::exists(manifestPath))
        return SetError(error, "missing MANIFEST");
    std::vector<unsigned char> bytes;
    if (!ReadExactFile(manifestPath, &bytes, BLOCK_INDEX_MANIFEST_SIZE_V1, error))
        return false;
    if (!DecodeManifestV1(&bytes[0], bytes.size(), &manifest, error))
        return false;
    if (!ValidateManifest(manifest, error))
        return false;
    if (options.requireCompleteManifest && manifest.state != BLOCK_INDEX_MANIFEST_COMPLETE)
        return SetError(error, "MANIFEST is not COMPLETE");
    ClearError(error);
    return true;
}

bool FixedBlockIndexStore::Create(const std::string& dir, uint64_t gen, FixedBlockIndexStore* out, std::string* error)
{
    if (!out)
        return SetError(error, "null output store");
    if (gen == 0)
        return SetError(error, "generation 0 is invalid");
    FixedBlockIndexStore store;
    if (!store.InitializePaths(dir, error))
        return false;
    boost::filesystem::create_directories(store.dirPath);
    if (boost::filesystem::exists(store.recordsPath) || boost::filesystem::exists(store.manifestPath))
        return SetError(error, "blockindex shadow store already exists");
    store.generation = gen;
    store.physicalRecordCount = 0;
    store.writable = true;
    store.open = true;
    store.manifest = FixedBlockIndexManifest();
    store.manifest.generation = gen;
    if (!store.WriteRecordsHeader(error))
        return false;
    if (!store.WriteManifest(store.manifest, error))
        return false;
    *out = store;
    ClearError(error);
    return true;
}

bool FixedBlockIndexStore::OpenReadOnly(const std::string& dir, const FixedBlockIndexOpenOptions& options, FixedBlockIndexStore* out, std::string* error)
{
    if (!out)
        return SetError(error, "null output store");
    FixedBlockIndexStore store;
    if (!store.InitializePaths(dir, error))
        return false;
    store.open = true;
    store.writable = false;

    uint64_t manifestGeneration = 0;
    if (!boost::filesystem::exists(store.manifestPath))
        return SetError(error, "missing MANIFEST");
    if (boost::filesystem::file_size(store.manifestPath) != BLOCK_INDEX_MANIFEST_SIZE_V1)
        return SetError(error, "invalid MANIFEST file size");
    std::vector<unsigned char> manifestBytes;
    if (!ReadExactFile(store.manifestPath, &manifestBytes, BLOCK_INDEX_MANIFEST_SIZE_V1, error))
        return false;
    FixedBlockIndexManifest manifest;
    if (!DecodeManifestV1(&manifestBytes[0], manifestBytes.size(), &manifest, error))
        return false;
    manifestGeneration = manifest.generation;
    store.generation = manifestGeneration;

    uint64_t fileSize = 0;
    if (!store.LoadRecordsHeader(&fileSize, error))
        return false;
    store.manifest = manifest;
    if (!store.ValidateManifest(store.manifest, error))
        return false;
    if (options.requireCompleteManifest && store.manifest.state != BLOCK_INDEX_MANIFEST_COMPLETE)
        return SetError(error, "MANIFEST is not COMPLETE");
    if (!store.ValidateCommittedRegion(fileSize, error))
        return false;
    *out = store;
    ClearError(error);
    return true;
}

bool FixedBlockIndexStore::Append(const BlockIndexRecord& record, BlockIndexId* outId, std::string* error)
{
    if (!open || !writable)
        return SetError(error, "store is not writable");
    if (!ValidateRecord(record, error))
        return false;
    if (physicalRecordCount == std::numeric_limits<uint64_t>::max())
        return SetError(error, "record id space exhausted");

    std::vector<unsigned char> encoded;
    if (!EncodeBlockIndexRecordV1(record, &encoded, error))
        return false;

    FILE* file = fopen(recordsPath.string().c_str(), "ab");
    if (!file)
        return SetError(error, "append open failed: " + recordsPath.string());
    if (!encoded.empty() && fwrite(&encoded[0], 1, encoded.size(), file) != encoded.size())
    {
        fclose(file);
        return SetError(error, "append write failed: " + recordsPath.string());
    }
    FileCommit(file);
    fclose(file);

    ++physicalRecordCount;
    if (outId)
        *outId = physicalRecordCount;
    ClearError(error);
    return true;
}

bool FixedBlockIndexStore::AppendBatch(const std::vector<BlockIndexRecord>& records,
                                       std::vector<BlockIndexId>* outIds,
                                       std::string* error)
{
    if (!open || !writable)
        return SetError(error, "store is not writable");
    if (records.empty())
        return SetError(error, "empty batch append");
    if (physicalRecordCount == std::numeric_limits<uint64_t>::max())
        return SetError(error, "record id space exhausted");
    if (records.size() > std::numeric_limits<uint64_t>::max() - physicalRecordCount)
        return SetError(error, "record id space exhausted by batch");

    // Encode all records first (validates each), collecting the exact bytes.
    std::vector<unsigned char> bytes;
    bytes.reserve(records.size() * BLOCK_INDEX_RECORD_SIZE_V1);
    for (size_t i = 0; i < records.size(); ++i)
    {
        std::vector<unsigned char> encoded;
        if (!EncodeBlockIndexRecordV1(records[i], &encoded, error))
            return false;
        bytes.insert(bytes.end(), encoded.begin(), encoded.end());
    }

    FILE* file = fopen(recordsPath.string().c_str(), "ab");
    if (!file)
        return SetError(error, "append open failed: " + recordsPath.string());
    if (!bytes.empty() && fwrite(&bytes[0], 1, bytes.size(), file) != bytes.size())
    {
        fclose(file);
        return SetError(error, "append write failed: " + recordsPath.string());
    }
    FileCommit(file); // single fsync for the whole batch
    fclose(file);

    if (outIds)
        outIds->clear();
    for (size_t i = 0; i < records.size(); ++i)
    {
        ++physicalRecordCount;
        if (outIds)
            outIds->push_back(physicalRecordCount);
    }
    ClearError(error);
    return true;
}

bool FixedBlockIndexStore::Read(BlockIndexId id, BlockIndexRecord* out, std::string* error) const
{
    if (!open)
        return SetError(error, "store is not open");
    if (!out)
        return SetError(error, "null record output");
    if (id == BLOCK_INDEX_ID_INVALID)
        return SetError(error, "RecordId 0 is invalid");
    if (id > manifest.recordCount)
        return SetError(error, "RecordId exceeds committed record count");

    uint64_t offset = 0;
    if (!CheckedAddMul(BLOCK_INDEX_RECORDS_HEADER_SIZE_V1, id - 1, BLOCK_INDEX_RECORD_SIZE_V1, &offset))
        return SetError(error, "record offset overflow");
    const uint64_t fileSize = boost::filesystem::file_size(recordsPath);
    uint64_t recordEnd = 0;
    if (!CheckedAddMul(offset, 1, BLOCK_INDEX_RECORD_SIZE_V1, &recordEnd))
        return SetError(error, "record end overflow");
    if (fileSize < recordEnd)
        return SetError(error, "truncated record bytes");

    FILE* file = fopen(recordsPath.string().c_str(), "rb");
    if (!file)
        return SetError(error, "read open failed: " + recordsPath.string());
    if (fseeko(file, (off_t)offset, SEEK_SET) != 0)
    {
        fclose(file);
        return SetError(error, "seek failed for record read");
    }
    std::vector<unsigned char> bytes(BLOCK_INDEX_RECORD_SIZE_V1, 0);
    const size_t n = fread(&bytes[0], 1, bytes.size(), file);
    fclose(file);
    if (n != bytes.size())
        return SetError(error, "short record read");
    return DecodeBlockIndexRecordV1(&bytes[0], bytes.size(), out, error);
}

bool FixedBlockIndexStore::WriteManifest(const FixedBlockIndexManifest& candidate, std::string* error)
{
    if (!open || !writable)
        return SetError(error, "store is not writable");
    if (!ValidateManifest(candidate, error))
        return false;
    std::vector<unsigned char> bytes;
    if (!EncodeManifestV1(candidate, &bytes, error))
        return false;
    if (!WriteFileAtomically(manifestPath, bytes, error))
        return false;
    manifest = candidate;
    ClearError(error);
    return true;
}

uint64_t FixedBlockIndexStore::CommittedRecordCount() const
{
    return manifest.recordCount;
}

uint64_t FixedBlockIndexStore::PhysicalRecordCount() const
{
    return physicalRecordCount;
}

const FixedBlockIndexManifest& FixedBlockIndexStore::GetManifest() const
{
    return manifest;
}
