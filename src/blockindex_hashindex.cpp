#include "blockindex_hashindex.h"

#include <leveldb/cache.h>
#include <leveldb/db.h>
#include <leveldb/filter_policy.h>

#include <boost/filesystem.hpp>

#include <limits>
#include <string.h>
#include <utility>
#include <vector>

namespace fs = boost::filesystem;

namespace {

static const char* const KEY_META_KIND = "meta:kind";
static const char* const KEY_META_SCHEMA_VERSION = "meta:schema_version";
static const char* const KEY_META_GENERATION = "meta:generation";

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

static void AppendU32LE(std::string* out, uint32_t value)
{
    out->push_back((char)(value & 0xff));
    out->push_back((char)((value >> 8) & 0xff));
    out->push_back((char)((value >> 16) & 0xff));
    out->push_back((char)((value >> 24) & 0xff));
}

static void AppendU64LE(std::string* out, uint64_t value)
{
    for (int i = 0; i < 8; ++i)
        out->push_back((char)((value >> (8 * i)) & 0xff));
}

static bool DecodeU32LE(const char* data, size_t size, uint32_t* out, std::string* error)
{
    if (size != 4)
        return SetError(error, "invalid uint32 value length");
    const unsigned char* p = (const unsigned char*)data;
    *out = ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return true;
}

static bool DecodeU64LE(const char* data, size_t size, uint64_t* out, std::string* error)
{
    if (size != 8)
        return SetError(error, "invalid uint64 value length");
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

static bool ParseHexNibble(char c, unsigned char* out)
{
    if (c >= '0' && c <= '9') {
        *out = (unsigned char)(c - '0');
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        *out = (unsigned char)(10 + (c - 'a'));
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        *out = (unsigned char)(10 + (c - 'A'));
        return true;
    }
    return false;
}

static bool AppendHashBytes(const uint256& hash, std::string* out, std::string* error)
{
    const std::string hex = hash.GetHex();
    if (hex.size() != 64)
        return SetError(error, "unexpected hash hex size");
    for (size_t i = 0; i < hex.size(); i += 2)
    {
        unsigned char hi = 0;
        unsigned char lo = 0;
        if (!ParseHexNibble(hex[i], &hi) || !ParseHexNibble(hex[i + 1], &lo))
            return SetError(error, "invalid hash hex nibble");
        out->push_back((char)((hi << 4) | lo));
    }
    return true;
}

static leveldb::Options MakeHashIndexOptions(bool create)
{
    leveldb::Options options;
    options.create_if_missing = create;
    options.error_if_exists = false;
    options.paranoid_checks = true;
    options.block_cache = leveldb::NewLRUCache(8 * 1048576);
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    options.write_buffer_size = 8 * 1048576;
    options.max_open_files = 256;
    options.compression = leveldb::kSnappyCompression;
    return options;
}

static bool LevelDbGet(leveldb::DB* db, const std::string& key, std::string* value, std::string* error, bool* notFound)
{
    *notFound = false;
    leveldb::ReadOptions options;
    options.verify_checksums = true;
    leveldb::Status status = db->Get(options, key, value);
    if (status.ok())
    {
        ClearError(error);
        return true;
    }
    if (status.IsNotFound())
    {
        *notFound = true;
        ClearError(error);
        return true;
    }
    return SetError(error, std::string("LevelDB get failure: ") + status.ToString());
}

} // namespace

struct BlockIndexHashIndex::SharedState
{
    fs::path generationDir;
    fs::path dbPath;
    leveldb::DB* db;
    leveldb::Options options;
    CCriticalSection cs;
    bool logicalReadOnly;
    bool open;
    uint64_t generation;

    SharedState()
        : db(NULL),
          logicalReadOnly(false),
          open(false),
          generation(0)
    {
    }
};

bool EncodeBlockIndexHashKey(const uint256& hash, std::string* out, std::string* error)
{
    if (!out)
        return SetError(error, "null hash key output");
    out->clear();
    out->reserve(BLOCK_INDEX_HASH_KEY_SIZE);
    out->push_back(BLOCK_INDEX_HASH_KEY_PREFIX);
    if (!AppendHashBytes(hash, out, error))
        return false;
    if (out->size() != BLOCK_INDEX_HASH_KEY_SIZE)
        return SetError(error, "invalid encoded hash key size");
    ClearError(error);
    return true;
}

bool EncodeBlockIndexRecordIdValue(BlockIndexId id, std::string* out, std::string* error)
{
    if (!out)
        return SetError(error, "null RecordId value output");
    if (id == BLOCK_INDEX_ID_INVALID)
        return SetError(error, "RecordId 0 is invalid");
    out->clear();
    out->reserve(BLOCK_INDEX_HASH_VALUE_SIZE);
    AppendU64LE(out, id);
    if (out->size() != BLOCK_INDEX_HASH_VALUE_SIZE)
        return SetError(error, "invalid encoded RecordId value size");
    ClearError(error);
    return true;
}

bool DecodeBlockIndexRecordIdValue(const char* data, size_t size, BlockIndexId* out, std::string* error)
{
    if (!data || !out)
        return SetError(error, "null RecordId decode input/output");
    uint64_t id = 0;
    if (!DecodeU64LE(data, size, &id, error))
        return false;
    if (id == BLOCK_INDEX_ID_INVALID)
        return SetError(error, "decoded RecordId 0 is invalid");
    *out = id;
    ClearError(error);
    return true;
}

BlockIndexHashIndex::BlockIndexHashIndex()
    : state(new SharedState())
{
}

BlockIndexHashIndex::~BlockIndexHashIndex()
{
    Close();
    delete state;
    state = NULL;
}

BlockIndexHashIndex::BlockIndexHashIndex(BlockIndexHashIndex&& other)
    : state(other.state)
{
    other.state = new SharedState();
}

BlockIndexHashIndex& BlockIndexHashIndex::operator=(BlockIndexHashIndex&& other)
{
    if (this != &other)
    {
        Close();
        delete state;
        state = other.state;
        other.state = new SharedState();
    }
    return *this;
}

void BlockIndexHashIndex::Close()
{
    if (!state)
        return;
    LOCK(state->cs);
    if (state->db)
    {
        delete state->db;
        state->db = NULL;
    }
    delete state->options.filter_policy;
    state->options.filter_policy = NULL;
    delete state->options.block_cache;
    state->options.block_cache = NULL;
    state->open = false;
    state->logicalReadOnly = false;
    state->generation = 0;
    state->generationDir.clear();
    state->dbPath.clear();
}

bool BlockIndexHashIndex::IsOpen() const
{
    return state && state->open && state->db != NULL;
}

bool BlockIndexHashIndex::IsLogicalReadOnly() const
{
    return state && state->logicalReadOnly;
}

uint64_t BlockIndexHashIndex::Generation() const
{
    return state ? state->generation : 0;
}

bool BlockIndexHashIndex::WriteMetadata(std::string* error)
{
    LOCK(state->cs);
    std::string schemaValue;
    std::string generationValue;
    AppendU32LE(&schemaValue, BLOCK_INDEX_HASHINDEX_SCHEMA_VERSION);
    AppendU64LE(&generationValue, state->generation);

    leveldb::WriteOptions options;
    leveldb::Status status = state->db->Put(options, KEY_META_KIND, BLOCK_INDEX_HASHINDEX_KIND);
    if (!status.ok())
        return SetError(error, std::string("hashindex write meta kind failure: ") + status.ToString());
    status = state->db->Put(options, KEY_META_SCHEMA_VERSION, schemaValue);
    if (!status.ok())
        return SetError(error, std::string("hashindex write schema failure: ") + status.ToString());
    status = state->db->Put(options, KEY_META_GENERATION, generationValue);
    if (!status.ok())
        return SetError(error, std::string("hashindex write generation failure: ") + status.ToString());
    ClearError(error);
    return true;
}

bool BlockIndexHashIndex::ReadAndValidateMetadata(uint64_t expectedGeneration, std::string* error)
{
    LOCK(state->cs);
    bool notFound = false;
    std::string value;
    if (!LevelDbGet(state->db, KEY_META_KIND, &value, error, &notFound))
        return false;
    if (notFound)
        return SetError(error, "missing hashindex metadata kind key");
    if (value != BLOCK_INDEX_HASHINDEX_KIND)
        return SetError(error, "unsupported hashindex kind");

    if (!LevelDbGet(state->db, KEY_META_SCHEMA_VERSION, &value, error, &notFound))
        return false;
    if (notFound)
        return SetError(error, "missing hashindex schema version key");
    uint32_t schemaVersion = 0;
    if (!DecodeU32LE(value.data(), value.size(), &schemaVersion, error))
        return false;
    if (schemaVersion != BLOCK_INDEX_HASHINDEX_SCHEMA_VERSION)
        return SetError(error, "unsupported hashindex schema version");

    if (!LevelDbGet(state->db, KEY_META_GENERATION, &value, error, &notFound))
        return false;
    if (notFound)
        return SetError(error, "missing hashindex generation key");
    uint64_t generation = 0;
    if (!DecodeU64LE(value.data(), value.size(), &generation, error))
        return false;
    if (generation == 0)
        return SetError(error, "invalid hashindex generation 0");
    if (expectedGeneration != 0 && generation != expectedGeneration)
        return SetError(error, "hashindex generation mismatch");
    state->generation = generation;
    ClearError(error);
    return true;
}

bool BlockIndexHashIndex::OpenInternal(const std::string& generationDir, uint64_t generation, bool create, bool logicalReadOnly, std::string* error)
{
    Close();
    state->generationDir = fs::path(generationDir);
    if (state->generationDir.empty())
        return SetError(error, "empty generation directory path");
    state->dbPath = state->generationDir / BLOCK_INDEX_HASHINDEX_DIR_NAME;
    if (create)
    {
        if (generation == 0)
            return SetError(error, "hashindex generation 0 is invalid");
        fs::create_directories(state->generationDir);
        if (fs::exists(state->dbPath))
            return SetError(error, "hashindex directory already exists");
        fs::create_directories(state->dbPath);
    }
    else if (!fs::exists(state->dbPath))
    {
        return SetError(error, "missing hashindex directory");
    }

    state->options = MakeHashIndexOptions(create);
    leveldb::Status status = leveldb::DB::Open(state->options, state->dbPath.string(), &state->db);
    if (!status.ok())
        return SetError(error, std::string("hashindex open failure: ") + status.ToString());
    state->open = true;
    state->logicalReadOnly = logicalReadOnly;
    state->generation = generation;

    if (create)
    {
        if (!WriteMetadata(error))
            return false;
    }
    else
    {
        if (!ReadAndValidateMetadata(generation, error))
            return false;
    }
    ClearError(error);
    return true;
}

bool BlockIndexHashIndex::Create(const std::string& generationDir, uint64_t generation, BlockIndexHashIndex* out, std::string* error)
{
    if (!out)
        return SetError(error, "null hashindex output");
    BlockIndexHashIndex index;
    if (!index.OpenInternal(generationDir, generation, true, false, error))
        return false;
    *out = std::move(index);
    ClearError(error);
    return true;
}

bool BlockIndexHashIndex::Open(const std::string& generationDir, uint64_t expectedGeneration, BlockIndexHashIndex* out, std::string* error)
{
    if (!out)
        return SetError(error, "null hashindex output");
    BlockIndexHashIndex index;
    if (!index.OpenInternal(generationDir, expectedGeneration, false, true, error))
        return false;
    *out = std::move(index);
    ClearError(error);
    return true;
}

bool BlockIndexHashIndex::Put(const uint256& hash, BlockIndexId id, std::string* error)
{
    if (!IsOpen())
        return SetError(error, "hashindex is not open");
    if (state->logicalReadOnly)
        return SetError(error, "hashindex is logically read-only");
    LOCK(state->cs);
    std::string key;
    std::string value;
    if (!EncodeBlockIndexHashKey(hash, &key, error) || !EncodeBlockIndexRecordIdValue(id, &value, error))
        return false;

    bool notFound = false;
    std::string existing;
    if (!LevelDbGet(state->db, key, &existing, error, &notFound))
        return false;
    if (!notFound)
    {
        BlockIndexId existingId = BLOCK_INDEX_ID_INVALID;
        if (!DecodeBlockIndexRecordIdValue(existing.data(), existing.size(), &existingId, error))
            return false;
        if (existingId == id)
        {
            ClearError(error);
            return true;
        }
        return SetError(error, "conflicting hashindex mapping for hash");
    }

    leveldb::Status status = state->db->Put(leveldb::WriteOptions(), key, value);
    if (!status.ok())
        return SetError(error, std::string("hashindex put failure: ") + status.ToString());
    ClearError(error);
    return true;
}

BlockIndexHashLookupStatus BlockIndexHashIndex::Lookup(const uint256& hash, BlockIndexId* outId, std::string* error) const
{
    if (!IsOpen())
    {
        SetError(error, "hashindex is not open");
        return BLOCK_INDEX_HASH_LOOKUP_ERROR;
    }
    if (!outId)
    {
        SetError(error, "null hashindex lookup output id");
        return BLOCK_INDEX_HASH_LOOKUP_ERROR;
    }
    LOCK(state->cs);
    std::string key;
    if (!EncodeBlockIndexHashKey(hash, &key, error))
        return BLOCK_INDEX_HASH_LOOKUP_ERROR;
    bool notFound = false;
    std::string value;
    if (!LevelDbGet(state->db, key, &value, error, &notFound))
        return BLOCK_INDEX_HASH_LOOKUP_ERROR;
    if (notFound)
    {
        *outId = BLOCK_INDEX_ID_INVALID;
        ClearError(error);
        return BLOCK_INDEX_HASH_LOOKUP_NOT_FOUND;
    }
    if (!DecodeBlockIndexRecordIdValue(value.data(), value.size(), outId, error))
        return BLOCK_INDEX_HASH_LOOKUP_ERROR;
    ClearError(error);
    return BLOCK_INDEX_HASH_LOOKUP_FOUND;
}

FixedBlockIndexShadowLookup::FixedBlockIndexShadowLookup()
    : open(false)
{
}

bool FixedBlockIndexShadowLookup::Open(const std::string& generationDir, FixedBlockIndexShadowLookup* out, std::string* error)
{
    if (!out)
        return SetError(error, "null shadow lookup output");
    FixedBlockIndexShadowLookup tmp;
    FixedBlockIndexOpenOptions storeOptions;
    storeOptions.requireCompleteManifest = true;
    if (!FixedBlockIndexStore::OpenReadOnly(generationDir, storeOptions, &tmp.store, error))
        return false;
    const uint64_t generation = tmp.store.GetManifest().generation;
    if (!BlockIndexHashIndex::Open(generationDir, generation, &tmp.hashIndex, error))
        return false;
    if (tmp.hashIndex.Generation() != generation)
        return SetError(error, "shadow lookup generation mismatch");
    tmp.open = true;
    *out = std::move(tmp);
    ClearError(error);
    return true;
}

BlockIndexHashLookupStatus FixedBlockIndexShadowLookup::LookupByHash(const uint256& hash, BlockIndexRecord* outRecord, BlockIndexId* outId, std::string* error) const
{
    if (!open)
    {
        SetError(error, "shadow lookup is not open");
        return BLOCK_INDEX_HASH_LOOKUP_ERROR;
    }
    if (!outRecord || !outId)
    {
        SetError(error, "null shadow lookup outputs");
        return BLOCK_INDEX_HASH_LOOKUP_ERROR;
    }
    BlockIndexHashLookupStatus status = hashIndex.Lookup(hash, outId, error);
    if (status != BLOCK_INDEX_HASH_LOOKUP_FOUND)
        return status;
    if (!store.Read(*outId, outRecord, error))
        return BLOCK_INDEX_HASH_LOOKUP_ERROR;
    if (outRecord->hash != hash)
    {
        SetError(error, "hashindex/record hash mismatch");
        return BLOCK_INDEX_HASH_LOOKUP_ERROR;
    }
    ClearError(error);
    return BLOCK_INDEX_HASH_LOOKUP_FOUND;
}
