// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "blockindex_tip.h"

#include "util.h"

#include <boost/filesystem.hpp>
#include <fcntl.h>
#include <openssl/sha.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

namespace fs = boost::filesystem;

namespace {

static const char* const BLOCK_INDEX_TIP_DIR_NAME = "blockindex_tip";
static const char* const BLOCK_INDEX_TIP_META_FILE = "tip.meta";
static const char* const BLOCK_INDEX_TIP_RECORDS_FILE = "tip-records.dat";
static const char* const BLOCK_INDEX_TIP_ACTIVE_FILE = "tip-active.dat";
static const char* const BLOCK_INDEX_TIP_DERIVED_FILE = "tip-derived.dat";
static const char* const BLOCK_INDEX_TIP_INVALID_FILE = "tip-invalid.dat";

static const uint32_t BLOCK_INDEX_TIP_RECORDS_HEADER_SIZE = 48;
static const uint32_t BLOCK_INDEX_TIP_ACTIVE_HEADER_SIZE = 44;
static const uint32_t BLOCK_INDEX_TIP_DERIVED_HEADER_SIZE = 72;
// v2 mutable protocol: append-only operator-invalid log.
// header: magic(4) + version(4) + count(4); entry: hash(32) + intent(1).
static const uint32_t BLOCK_INDEX_TIP_INVALID_HEADER_SIZE = 12;
static const uint32_t BLOCK_INDEX_TIP_INVALID_ENTRY_SIZE = 33;
static const uint32_t BLOCK_INDEX_TIP_INVALID_MAGIC = 0x564E4931u; // "1INV"
static const uint32_t BLOCK_INDEX_TIP_INVALID_FILE_VERSION = 1;

// R3 test-only failpoint registry (armed only by tests; empty in production).
static std::set<std::string> g_tipFailpoints;

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

static void WriteRawLE64(std::vector<unsigned char>& out, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        out.push_back((unsigned char)((v >> (8 * i)) & 0xff));
}

static void WriteRawLE32(std::vector<unsigned char>& out, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out.push_back((unsigned char)((v >> (8 * i)) & 0xff));
}

static uint64_t ReadRawLE64(const unsigned char* p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= ((uint64_t)p[i]) << (8 * i);
    return v;
}

static uint32_t ReadRawLE32(const unsigned char* p)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= ((uint32_t)p[i]) << (8 * i);
    return v;
}

static bool EncodeTipMeta(const BlockIndexTipMeta& meta, std::string* out)
{
    std::vector<unsigned char> b;
    WriteRawLE32(b, meta.version);
    WriteRawLE64(b, meta.baseGeneration);
    WriteRawLE64(b, meta.baseRecordCount);
    b.insert(b.end(), (const unsigned char*)&meta.baseTipHeight,
             (const unsigned char*)&meta.baseTipHeight + sizeof(meta.baseTipHeight));
    b.insert(b.end(), (const unsigned char*)&meta.tipHeight,
             (const unsigned char*)&meta.tipHeight + sizeof(meta.tipHeight));
    b.insert(b.end(), meta.tipHash.begin(), meta.tipHash.end());
    WriteRawLE64(b, meta.tipRecordCount);
    WriteRawLE32(b, meta.activeFence);
    b.insert(b.end(), meta.contentDigest, meta.contentDigest + 32);
    // v2: durable operator-invalid log commitment (same commit point).
    WriteRawLE32(b, meta.invalidLogCount);
    b.insert(b.end(), meta.invalidDigest, meta.invalidDigest + 32);
    out->assign((const char*)&b[0], b.size());
    return true;
}

static bool DecodeTipMeta(const char* data, size_t size, BlockIndexTipMeta* out)
{
    // v1 layout: 4 + 8 + 8 + 4 + 4 + 32 + 8 + 4 + 32 = 104 bytes
    // v2 layout: v1 + invalidLogCount(4) + invalidDigest(32) = 140 bytes
    if (size < 104)
        return false;
    const unsigned char* p = (const unsigned char*)data;
    BlockIndexTipMeta m;
    m.version = ReadRawLE32(p + 0);
    m.baseGeneration = ReadRawLE64(p + 4);
    m.baseRecordCount = ReadRawLE64(p + 12);
    memcpy(&m.baseTipHeight, p + 20, sizeof(m.baseTipHeight));
    memcpy(&m.tipHeight, p + 24, sizeof(m.tipHeight));
    memcpy(m.tipHash.begin(), p + 28, 32);
    m.tipRecordCount = ReadRawLE64(p + 60);
    m.activeFence = (uint8_t)ReadRawLE32(p + 68);
    memcpy(m.contentDigest, p + 72, 32);
    // Backward-safe version handling. Accept the previous supported version
    // (v1: no operator-invalid authority -> EMPTY set). Accept the current
    // version (v2) only when its full layout is present. Anything else (an
    // unknown future version, a truncated v2) FAILS CLOSED -- never guessed.
    if (m.version == BLOCK_INDEX_TIP_META_VERSION_V1)
    {
        m.invalidLogCount = 0;
        memset(m.invalidDigest, 0, 32);
    }
    else if (m.version == BLOCK_INDEX_TIP_META_VERSION)
    {
        if (size < 140)
            return false;
        m.invalidLogCount = ReadRawLE32(p + 104);
        memcpy(m.invalidDigest, p + 108, 32);
    }
    else
    {
        return false;
    }
    *out = m;
    return true;
}

static void ComputeContentDigest(const BlockIndexTipMeta& meta,
                                 const std::vector<BlockIndexRecord>& records,
                                 const std::vector<BlockIndexDerivedEntry>& derived,
                                 const std::vector<BlockIndexId>& activeIds,
                                 unsigned char digest[32])
{
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    for (size_t i = 0; i < records.size(); ++i)
    {
        const BlockIndexRecord& r = records[i];
        SHA256_Update(&ctx, r.hash.begin(), 32);
        SHA256_Update(&ctx, r.hashPrev.begin(), 32);
        for (int j = 0; j < 4; ++j)
        {
            unsigned char b = (unsigned char)((r.height >> (8 * j)) & 0xff);
            SHA256_Update(&ctx, &b, 1);
        }
    }
    for (size_t i = 0; i < derived.size(); ++i)
    {
        const BlockIndexDerivedEntry& d = derived[i];
        SHA256_Update(&ctx, d.chainTrust.begin(), 32);
        for (int j = 0; j < 4; ++j)
        {
            unsigned char b = (unsigned char)((d.stakeModifierChecksum >> (8 * j)) & 0xff);
            SHA256_Update(&ctx, &b, 1);
        }
    }
    for (size_t i = 0; i < activeIds.size(); ++i)
    {
        for (int j = 0; j < 8; ++j)
        {
            unsigned char b = (unsigned char)((activeIds[i] >> (8 * j)) & 0xff);
            SHA256_Update(&ctx, &b, 1);
        }
    }
    SHA256_Update(&ctx, &meta.activeFence, 1);
    SHA256_Final(digest, &ctx);
}

// ---- v2 operator-invalid log ----
// An entry is one operator intent applied in append order: intent 1 marks the
// hash operator-invalid, intent 0 (reconsider) clears it. The derived set is
// the replay of the committed prefix (last intent wins). This keeps the store
// append-only so tip.meta (invalidLogCount) remains the sole commit point and
// Open truncates an uncommitted tail exactly like the other tip stores.
struct BlockIndexTipInvalidEntry
{
    uint256 hash;
    uint8_t intent; // 1 = invalidate, 0 = reconsider

    BlockIndexTipInvalidEntry() : hash(0), intent(0) {}
};

static void ComputeInvalidDigest(const std::vector<BlockIndexTipInvalidEntry>& entries,
                                 unsigned char digest[32])
{
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    unsigned char versionBytes[4] = {0, 0, 0, BLOCK_INDEX_TIP_INVALID_FILE_VERSION};
    (void)versionBytes; // version is a constant tag; bind the count + entries
    for (int j = 0; j < 4; ++j)
    {
        unsigned char b = (unsigned char)((BLOCK_INDEX_TIP_INVALID_FILE_VERSION >> (8 * j)) & 0xff);
        SHA256_Update(&ctx, &b, 1);
    }
    for (int j = 0; j < 4; ++j)
    {
        unsigned char b = (unsigned char)(((uint32_t)entries.size() >> (8 * j)) & 0xff);
        SHA256_Update(&ctx, &b, 1);
    }
    for (size_t i = 0; i < entries.size(); ++i)
    {
        SHA256_Update(&ctx, entries[i].hash.begin(), 32);
        SHA256_Update(&ctx, &entries[i].intent, 1);
    }
    SHA256_Final(digest, &ctx);
}

// R3 (PM1-P0-04): committed-region immutability. Every mutable tail store is
// written copy-on-write: a complete temporary file is flushed + fsynced, then
// atomically renamed over the target, then the containing directory is fsynced.
// The previously committed file is therefore never truncated or partially
// overwritten, so a crash at any point leaves the last committed bytes intact
// (only an orphaned temp file remains). This replaces the earlier in-place
// fopen("wb") full rewrite whose torn write could destroy the committed prefix
// and turn a recoverable crash into a fail-closed, unrecoverable short store.
static bool WriteFileCoW(const fs::path& path, const std::vector<unsigned char>& b,
                         std::string* error)
{
    const fs::path tmp = fs::path(path.string() + ".tmp");
    FILE* f = fopen(tmp.string().c_str(), "wb");
    if (!f)
    {
        if (error) *error = "open temp failed: " + tmp.string();
        return false;
    }
    if (!b.empty() && fwrite(&b[0], 1, b.size(), f) != b.size())
    {
        fclose(f);
        if (error) *error = "write temp failed: " + tmp.string();
        return false;
    }
    if (!FileCommitChecked(f, error))
    {
        fclose(f);
        return false;
    }
    if (fclose(f) != 0)
    {
        if (error) *error = "close temp failed: " + tmp.string();
        return false;
    }
    // Test-only crash injection: the complete temp file is durable but the
    // atomic rename has not happened; the committed file must survive.
    if (BlockIndexTipFailpointHit("FP_DURING_TAIL_UPDATE"))
    {
        if (error) *error = "failpoint FP_DURING_TAIL_UPDATE";
        return false;
    }
    if (!RenameOverChecked(tmp, path, error))
        return false;
    if (!SyncDirectoryChecked(path.parent_path(), error))
        return false;
    return true;
}

static bool WriteInvalidFile(const fs::path& path,
                             const std::vector<BlockIndexTipInvalidEntry>& entries,
                             std::string* error = NULL)
{
    std::vector<unsigned char> b;
    WriteRawLE32(b, BLOCK_INDEX_TIP_INVALID_MAGIC);
    WriteRawLE32(b, BLOCK_INDEX_TIP_INVALID_FILE_VERSION);
    WriteRawLE32(b, (uint32_t)entries.size());
    for (size_t i = 0; i < entries.size(); ++i)
    {
        b.insert(b.end(), entries[i].hash.begin(), entries[i].hash.end());
        b.push_back(entries[i].intent);
    }
    return WriteFileCoW(path, b, error);
}

// Parse the invalid log. Returns true on a well-formed file (magic + version
// match, bounded entry count); entries beyond the committed count are kept so
// the caller can truncate an uncommitted tail. Returns false on a malformed
// header (fail closed).
static bool ParseInvalidFile(const std::string& data,
                             std::vector<BlockIndexTipInvalidEntry>* out)
{
    if (data.size() < BLOCK_INDEX_TIP_INVALID_HEADER_SIZE)
        return false;
    const unsigned char* p = (const unsigned char*)data.data();
    if (ReadRawLE32(p + 0) != BLOCK_INDEX_TIP_INVALID_MAGIC)
        return false;
    if (ReadRawLE32(p + 4) != BLOCK_INDEX_TIP_INVALID_FILE_VERSION)
        return false;
    uint32_t count = ReadRawLE32(p + 8);
    size_t avail = (data.size() - BLOCK_INDEX_TIP_INVALID_HEADER_SIZE) /
                   BLOCK_INDEX_TIP_INVALID_ENTRY_SIZE;
    if (count > avail)
        return false; // header claims more committed entries than are present
    out->clear();
    for (uint32_t i = 0; i < count; ++i)
    {
        const unsigned char* e = p + BLOCK_INDEX_TIP_INVALID_HEADER_SIZE +
                                 (size_t)i * BLOCK_INDEX_TIP_INVALID_ENTRY_SIZE;
        BlockIndexTipInvalidEntry ent;
        memcpy(ent.hash.begin(), e, 32);
        ent.intent = e[32];
        if (ent.intent != 0 && ent.intent != 1)
            return false; // undefined intent -> deterministic reject
        out->push_back(ent);
    }
    return true;
}

static void DeriveInvalidSet(const std::vector<BlockIndexTipInvalidEntry>& entries,
                             std::set<uint256>* out)
{
    out->clear();
    for (size_t i = 0; i < entries.size(); ++i)
    {
        if (entries[i].intent == 1)
            out->insert(entries[i].hash);
        else
            out->erase(entries[i].hash);
    }
}

// ---- file helpers ----

static bool ReadWholeFile(const fs::path& path, std::string* out)
{
    FILE* f = fopen(path.string().c_str(), "rb");
    if (!f)
        return false;
    std::string data;
    unsigned char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        data.append((const char*)buf, n);
    fclose(f);
    *out = data;
    return true;
}

// ---- R4: incremental (append-only) tail persistence -------------------------
// A normal AppendBatch appends ONLY the newly encoded entries to the end of each
// mutable tail store. Each store is a fixed-size header followed by fixed-size
// entries, and Open decodes to EOF then truncates to the committed tip.meta
// counts, so an appended-but-uncommitted suffix is recovered exactly as before.
// Cost becomes O(new records) instead of O(retained tail).
//
// The count field of each header is the first LE64 after the LE32 header fields.
static const uint32_t BLOCK_INDEX_TIP_RECORDS_COUNT_OFFSET = 16;
static const uint32_t BLOCK_INDEX_TIP_DERIVED_COUNT_OFFSET = 16;
static const uint32_t BLOCK_INDEX_TIP_ACTIVE_COUNT_OFFSET  = 12;

// Truncate a store file to exactly headerSize + committedCount*entrySize. Used to
// discard a stale uncommitted suffix before appending (idempotent; a no-op after a
// clean shutdown) and during Open recovery. Fails closed if the file is SHORTER
// than the committed region.
static bool TruncateStoreToCommitted(const fs::path& path, uint64_t headerSize,
                                     uint64_t entrySize, uint64_t committedCount,
                                     std::string* error)
{
    const uint64_t want = headerSize + committedCount * entrySize;
    boost::system::error_code ec;
    uint64_t have = (uint64_t)fs::file_size(path, ec);
    if (ec)
    {
        if (error) *error = "stat store failed: " + path.string();
        return false;
    }
    if (have == want)
        return true;
    if (have < want)
    {
        if (error) *error = "store short of committed bytes: " + path.string();
        return false;
    }
    fs::resize_file(path, want, ec);
    if (ec)
    {
        if (error) *error = "truncate store failed: " + path.string();
        return false;
    }
    return true;
}

// Append bytes to the end of an existing store and fsync. b empty is a no-op.
// After the bytes are durable, honour the FP_DURING_TAIL_UPDATE failpoint (the
// append-path analogue of the copy-on-write "temp durable, not yet published"
// crash boundary): the caller aborts with the appended bytes uncommitted, and
// recovery truncates them.
static bool AppendBytesDurable(const fs::path& path, const std::vector<unsigned char>& b,
                               std::string* error)
{
    if (b.empty())
        return true;
    FILE* f = fopen(path.string().c_str(), "r+b");
    if (!f)
    {
        if (error) *error = "open append failed: " + path.string();
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0 ||
        (!b.empty() && fwrite(&b[0], 1, b.size(), f) != b.size()))
    {
        fclose(f);
        if (error) *error = "append write failed: " + path.string();
        return false;
    }
    if (!FileCommitChecked(f, error))
    {
        fclose(f);
        return false;
    }
    if (BlockIndexTipFailpointHit("FP_DURING_TAIL_UPDATE"))
    {
        fclose(f);
        if (error) *error = "failpoint FP_DURING_TAIL_UPDATE";
        return false;
    }
    if (fclose(f) != 0)
    {
        if (error) *error = "append close failed: " + path.string();
        return false;
    }
    return true;
}

// Rewrite only the 8-byte committed-entry count field of a store header, then
// fsync. The count is informational (Open bounds by tip.meta), but keeping it
// accurate preserves the on-disk invariant.
static bool UpdateStoreCount(const fs::path& path, uint32_t countOffset, uint64_t count,
                             std::string* error)
{
    FILE* f = fopen(path.string().c_str(), "r+b");
    if (!f)
    {
        if (error) *error = "open count update failed: " + path.string();
        return false;
    }
    unsigned char b[8];
    for (int j = 0; j < 8; ++j)
        b[j] = (unsigned char)((count >> (8 * j)) & 0xff);
    if (fseek(f, (long)countOffset, SEEK_SET) != 0 || fwrite(b, 1, 8, f) != 8)
    {
        fclose(f);
        if (error) *error = "count update write failed: " + path.string();
        return false;
    }
    if (!FileCommitChecked(f, error))
    {
        fclose(f);
        return false;
    }
    if (fclose(f) != 0)
    {
        if (error) *error = "count update close failed: " + path.string();
        return false;
    }
    return true;
}

static bool WriteRecordsFile(const fs::path& path, const std::vector<BlockIndexRecord>& records,
                             std::string* error = NULL)
{
    std::vector<unsigned char> hdr;
    WriteRawLE32(hdr, BLOCK_INDEX_FORMAT_VERSION);
    WriteRawLE32(hdr, BLOCK_INDEX_RECORD_VERSION);
    WriteRawLE32(hdr, BLOCK_INDEX_TIP_RECORDS_HEADER_SIZE);
    WriteRawLE32(hdr, BLOCK_INDEX_RECORD_SIZE_V1);
    WriteRawLE64(hdr, records.size());
    WriteRawLE64(hdr, 0); WriteRawLE64(hdr, 0); WriteRawLE64(hdr, 0);
    std::vector<unsigned char> b = hdr;
    for (size_t i = 0; i < records.size(); ++i)
    {
        std::vector<unsigned char> enc;
        if (!EncodeBlockIndexRecordV1(records[i], &enc, NULL))
        {
            if (error) *error = "encode tip-records entry failed";
            return false;
        }
        b.insert(b.end(), enc.begin(), enc.end());
    }
    return WriteFileCoW(path, b, error);
}

static bool WriteActiveFile(const fs::path& path, const std::vector<BlockIndexId>& ids,
                            std::string* error = NULL)
{
    std::vector<unsigned char> hdr;
    WriteRawLE32(hdr, BLOCK_INDEX_ACTIVE_SCHEMA_VERSION);
    WriteRawLE32(hdr, BLOCK_INDEX_TIP_ACTIVE_HEADER_SIZE);
    WriteRawLE32(hdr, BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1);
    WriteRawLE64(hdr, 0); WriteRawLE64(hdr, 0); WriteRawLE64(hdr, 0); WriteRawLE64(hdr, 0);
    std::vector<unsigned char> b = hdr;
    for (size_t i = 0; i < ids.size(); ++i)
    {
        std::string enc;
        if (!EncodeBlockIndexActiveEntry(ids[i], &enc, NULL))
        {
            if (error) *error = "encode tip-active entry failed";
            return false;
        }
        b.insert(b.end(), enc.begin(), enc.end());
    }
    return WriteFileCoW(path, b, error);
}

static bool WriteDerivedFile(const fs::path& path, const std::vector<BlockIndexDerivedEntry>& derived,
                             std::string* error = NULL)
{
    std::vector<unsigned char> hdr;
    WriteRawLE32(hdr, BLOCK_INDEX_DERIVED_FORMAT_VERSION);
    WriteRawLE32(hdr, BLOCK_INDEX_DERIVED_SCHEMA_VERSION);
    WriteRawLE32(hdr, BLOCK_INDEX_TIP_DERIVED_HEADER_SIZE);
    WriteRawLE32(hdr, BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2);
    WriteRawLE64(hdr, 0);
    WriteRawLE64(hdr, derived.size());
    for (int i = 0; i < 32; ++i) hdr.push_back(0);
    WriteRawLE64(hdr, 0);
    std::vector<unsigned char> b = hdr;
    for (size_t i = 0; i < derived.size(); ++i)
    {
        std::vector<unsigned char> enc;
        if (!EncodeBlockIndexDerivedEntry(derived[i], &enc, NULL))
        {
            if (error) *error = "encode tip-derived entry failed";
            return false;
        }
        b.insert(b.end(), enc.begin(), enc.end());
    }
    return WriteFileCoW(path, b, error);
}

} // namespace

// R3 test-only failpoints (definitions; declared in blockindex_tip.h).
void BlockIndexTipSetFailpointForTesting(const std::string& name, bool armed)
{
    if (armed) g_tipFailpoints.insert(name);
    else g_tipFailpoints.erase(name);
}
bool BlockIndexTipFailpointHit(const std::string& name)
{
    return g_tipFailpoints.count(name) != 0;
}

struct BlockIndexTipAuthority::Impl
{
    fs::path root;
    fs::path tipDir;
    fs::path metaPath;
    fs::path recordsPath;
    fs::path activePath;
    fs::path derivedPath;
    fs::path invalidPath;

    BlockIndexTipMeta meta;

    // Committed authority state (bounded to tip blocks only).
    std::vector<BlockIndexRecord> records;
    std::vector<BlockIndexDerivedEntry> derived;
    std::vector<BlockIndexId> activeIds; // dense: activeIds[h] = RecordId for height h
    std::map<uint256, BlockIndexId> hashToId;

    // v2 durable operator-invalid authority (by-value; no CBlockIndex).
    std::vector<BlockIndexTipInvalidEntry> invalidEntries; // committed log prefix
    std::set<uint256> invalidSet;                          // derived set

    bool open;

    Impl() : open(false) {}

    bool InitializePaths(const std::string& rootIn)
    {
        root = fs::path(rootIn);
        tipDir = root / BLOCK_INDEX_TIP_DIR_NAME;
        metaPath = tipDir / BLOCK_INDEX_TIP_META_FILE;
        recordsPath = tipDir / BLOCK_INDEX_TIP_RECORDS_FILE;
        activePath = tipDir / BLOCK_INDEX_TIP_ACTIVE_FILE;
        derivedPath = tipDir / BLOCK_INDEX_TIP_DERIVED_FILE;
        invalidPath = tipDir / BLOCK_INDEX_TIP_INVALID_FILE;
        return true;
    }

    uint64_t baseLocalToId(size_t localIndex) const
    {
        // localIndex 0-based into records; RecordId = baseRecordCount + local + 1
        return meta.baseRecordCount + (uint64_t)localIndex + 1;
    }

    bool WriteMeta(std::string* error)
    {
        std::string encoded;
        if (!EncodeTipMeta(meta, &encoded))
            return SetError(error, "encode tip.meta failed");
        const fs::path tmp = metaPath.string() + ".tmp";
        FILE* f = fopen(tmp.string().c_str(), "wb");
        if (!f)
            return SetError(error, "open tip.meta tmp failed");
        if (fwrite(encoded.data(), 1, encoded.size(), f) != encoded.size())
        {
            fclose(f);
            return SetError(error, "write tip.meta tmp failed");
        }
        // R3 (PM1-P0-04 / PM1-P1-02): the tip.meta rename is THE single logical
        // commit point for post-S authority. Every step is checked and must
        // propagate: an unchecked fsync or directory-sync failure here would
        // silently lose (or silently revert) an acknowledged transition.
        if (!FileCommitChecked(f, error))
        {
            fclose(f);
            return false;
        }
        if (fclose(f) != 0)
            return SetError(error, "close tip.meta tmp failed");
        if (BlockIndexTipFailpointHit("FP_BEFORE_META_RENAME"))
            return SetError(error, "failpoint FP_BEFORE_META_RENAME");
        if (!RenameOverChecked(tmp, metaPath, error))
            return false;
        if (BlockIndexTipFailpointHit("FP_AFTER_META_RENAME_BEFORE_DIRSYNC"))
            return SetError(error, "failpoint FP_AFTER_META_RENAME_BEFORE_DIRSYNC");
        // R3F: inject a real directory-sync failure at the meta commit point (the
        // rename already happened, so the new authority is visible; the protocol
        // reports failure rather than pretending the commit was durable).
        if (DurabilityFailpointConsumeForTesting("META_DIR_SYNC"))
            return SetError(error, "injected meta directory sync failure");
        if (!SyncDirectoryChecked(tipDir, error))
            return false;
        return true;
    }
};

BlockIndexTipAuthority::BlockIndexTipAuthority()
    : impl(new Impl())
{
}

BlockIndexTipAuthority::~BlockIndexTipAuthority()
{
    delete impl;
}

bool BlockIndexTipAuthority::Create(const std::string& root,
                                    uint64_t baseGeneration,
                                    uint64_t baseRecordCount,
                                    int32_t baseTipHeight,
                                    BlockIndexTipAuthority* out,
                                    std::string* error)
{
    if (!out)
        return SetError(error, "null tip authority output");
    Impl* i = out->impl;
    if (!i->InitializePaths(root))
        return SetError(error, "init tip paths failed");
    if (fs::exists(i->tipDir))
        return SetError(error, "blockindex_tip already exists");

    boost::system::error_code ec;
    if (!fs::create_directories(i->tipDir, ec) && ec)
        return SetError(error, "create blockindex_tip failed");

    i->meta = BlockIndexTipMeta();
    i->meta.version = BLOCK_INDEX_TIP_META_VERSION;
    i->meta.baseGeneration = baseGeneration;
    i->meta.baseRecordCount = baseRecordCount;
    i->meta.baseTipHeight = baseTipHeight;
    i->meta.tipHeight = baseTipHeight; // empty tip == base tip S
    i->meta.tipRecordCount = 0;
    // persist empty stores (headers only) via the write helpers with empty state
    if (!WriteRecordsFile(i->recordsPath, i->records))
        return SetError(error, "init tip-records failed");
    if (!WriteActiveFile(i->activePath, i->activeIds))
        return SetError(error, "init tip-active failed");
    if (!WriteDerivedFile(i->derivedPath, i->derived))
        return SetError(error, "init tip-derived failed");
    // v2: create the (empty) operator-invalid log so the store set is complete.
    if (!WriteInvalidFile(i->invalidPath, i->invalidEntries))
        return SetError(error, "init tip-invalid failed");
    // compute the content digest over the (empty) committed region so that a
    // subsequent Open's recomputation matches (it must not be all-zero).
    ComputeContentDigest(i->meta, i->records, i->derived, i->activeIds,
                         i->meta.contentDigest);
    ComputeInvalidDigest(i->invalidEntries, i->meta.invalidDigest);
    i->meta.invalidLogCount = 0;
    if (!i->WriteMeta(error))
        return false;
    i->open = true;
    ClearError(error);
    return true;
}

bool BlockIndexTipAuthority::Open(const std::string& root,
                                  uint64_t expectedBaseGeneration,
                                  BlockIndexTipAuthority* out,
                                  std::string* error)
{
    if (!out)
        return SetError(error, "null tip authority output");
    Impl* i = out->impl;
    if (!i->InitializePaths(root))
        return SetError(error, "init tip paths failed");
    if (!fs::exists(i->metaPath))
        return SetError(error, "tip.meta absent (no blockindex_tip to open)");
    if (!fs::exists(i->recordsPath) || !fs::exists(i->activePath) ||
        !fs::exists(i->derivedPath))
        return SetError(error, "blockindex_tip incomplete store set");

    // 1. Load tip.meta.
    std::string metadat;
    if (!ReadWholeFile(i->metaPath, &metadat))
        return SetError(error, "read tip.meta failed");
    BlockIndexTipMeta meta;
    if (!DecodeTipMeta(metadat.data(), metadat.size(), &meta))
        return SetError(error, "decode tip.meta failed");
    if (meta.baseGeneration != expectedBaseGeneration)
        return SetError(error, "tip.meta base-generation mismatch");
    i->meta = meta;

    // 2. Load tip-records.dat -> records vector.
    std::string recData;
    if (!ReadWholeFile(i->recordsPath, &recData))
        return SetError(error, "read tip-records failed");
    size_t recOff = BLOCK_INDEX_TIP_RECORDS_HEADER_SIZE;
    std::vector<BlockIndexRecord> records;
    while (recOff + BLOCK_INDEX_RECORD_SIZE_V1 <= recData.size())
    {
        BlockIndexRecord r;
        std::string rerr;
        if (!DecodeBlockIndexRecordV1((const unsigned char*)recData.data() + recOff,
                                      BLOCK_INDEX_RECORD_SIZE_V1, &r, &rerr))
            return SetError(error, "decode tip-records entry: " + rerr);
        records.push_back(r);
        recOff += BLOCK_INDEX_RECORD_SIZE_V1;
    }

    // 3. Load tip-derived.dat -> derived vector.
    std::string derData;
    if (!ReadWholeFile(i->derivedPath, &derData))
        return SetError(error, "read tip-derived failed");
    size_t derOff = BLOCK_INDEX_TIP_DERIVED_HEADER_SIZE;
    std::vector<BlockIndexDerivedEntry> derived;
    while (derOff + BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2 <= derData.size())
    {
        BlockIndexDerivedEntry d;
        std::string derr;
        if (!DecodeBlockIndexDerivedEntry((const unsigned char*)derData.data() + derOff,
                                          BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2, &d, &derr))
            return SetError(error, "decode tip-derived entry: " + derr);
        derived.push_back(d);
        derOff += BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2;
    }

    // 4. Load tip-active.dat -> activeIds vector.
    std::string actData;
    if (!ReadWholeFile(i->activePath, &actData))
        return SetError(error, "read tip-active failed");
    size_t actOff = BLOCK_INDEX_TIP_ACTIVE_HEADER_SIZE;
    std::vector<BlockIndexId> activeIds;
    while (actOff + BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1 <= actData.size())
    {
        BlockIndexId id;
        std::string aerr;
        if (!DecodeBlockIndexActiveEntry(actData.data() + actOff,
                                         BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1, &id, &aerr))
            return SetError(error, "decode tip-active entry: " + aerr);
        activeIds.push_back(id);
        actOff += BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1;
    }

    // 5. Reconcilce: the committed tip is meta.tipRecordCount records and
    //    meta.tipHeight+1 active members. Any bytes beyond that are an
    //    uncommitted tail (crash mid-append) and must be truncated to the
    //    committed tip (fail-safe, deterministic, O(tail)).
    bool repaired = false;
    if (records.size() > meta.tipRecordCount)
    {
        records.resize(meta.tipRecordCount);
        repaired = true;
    }
    if (derived.size() > meta.tipRecordCount)
    {
        derived.resize(meta.tipRecordCount);
        repaired = true;
    }
    // activeIds is RELATIVE: committed count = tipHeight - baseTipHeight.
    const int64_t expectedActive = (int64_t)meta.tipHeight - (int64_t)meta.baseTipHeight;
    if (expectedActive < 0)
        return SetError(error, "tip.meta tipHeight below baseTipHeight (corrupt)");
    size_t expActiveSize = (size_t)expectedActive;
    if (activeIds.size() > expActiveSize)
    {
        activeIds.resize(expActiveSize);
        repaired = true;
    }
    if (records.size() < meta.tipRecordCount ||
        derived.size() < meta.tipRecordCount ||
        activeIds.size() < expActiveSize)
        return SetError(error, "tip stores short of committed tip.meta (corrupt)");

    // R4: a suffix beyond tip.meta is uncommitted residue and must be PHYSICALLY
    // removed, not merely dropped in memory: a later append lands at EOF, so a
    // stale suffix left on disk would place new entries after the residue and
    // make the sequential decode read the wrong records. Truncate exactly to the
    // committed boundary (bytes beyond tip.meta are never authoritative).
    if (repaired)
    {
        if (!TruncateStoreToCommitted(i->recordsPath, BLOCK_INDEX_TIP_RECORDS_HEADER_SIZE,
                                      BLOCK_INDEX_RECORD_SIZE_V1, meta.tipRecordCount, error) ||
            !TruncateStoreToCommitted(i->derivedPath, BLOCK_INDEX_TIP_DERIVED_HEADER_SIZE,
                                      BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2, meta.tipRecordCount, error) ||
            !TruncateStoreToCommitted(i->activePath, BLOCK_INDEX_TIP_ACTIVE_HEADER_SIZE,
                                      BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1, expActiveSize, error))
            return false;
        // Keep the informational header counts consistent with the truncated files.
        std::string cerr;
        if (!UpdateStoreCount(i->recordsPath, BLOCK_INDEX_TIP_RECORDS_COUNT_OFFSET, meta.tipRecordCount, &cerr) ||
            !UpdateStoreCount(i->derivedPath, BLOCK_INDEX_TIP_DERIVED_COUNT_OFFSET, meta.tipRecordCount, &cerr) ||
            !UpdateStoreCount(i->activePath, BLOCK_INDEX_TIP_ACTIVE_COUNT_OFFSET, expActiveSize, &cerr))
            return SetError(error, "tip header count repair failed: " + cerr);
    }

    // 6. Rebuild hashToId.
    i->hashToId.clear();
    for (size_t j = 0; j < records.size(); ++j)
        i->hashToId[records[j].hash] = i->baseLocalToId(j);

    // 7. Validate content digest. On mismatch: if we repaired, rewrite stores +
    //    meta to the committed state; if not repaired it indicates corruption.
    unsigned char digest[32];
    ComputeContentDigest(meta, records, derived, activeIds, digest);
    if (memcmp(digest, meta.contentDigest, 32) != 0)
    {
        if (!repaired)
            return SetError(error, "tip store content digest mismatch (corrupt)");
        // repaired an uncommitted tail: re-persist stores + recompute digest.
        if (!WriteRecordsFile(i->recordsPath, records))
            return SetError(error, "repair tip-records failed");
        if (!WriteActiveFile(i->activePath, activeIds))
            return SetError(error, "repair tip-active failed");
        if (!WriteDerivedFile(i->derivedPath, derived))
            return SetError(error, "repair tip-derived failed");
        i->meta.contentDigest[0] = 0;
    }

    // 8. v2: load + validate the committed operator-invalid log. tip.meta's
    //    invalidLogCount is the commit point for the log, exactly as
    //    tipRecordCount is for tip-records: a longer file is an uncommitted
    //    tail (crash mid-commit) and is truncated; a shorter file, a digest
    //    mismatch, or a malformed header FAIL CLOSED. A v1 tip carries an
    //    EMPTY set (backward compatible; no rewrite merely to read it).
    std::vector<BlockIndexTipInvalidEntry> invalidEntries;
    if (meta.invalidLogCount > 0)
    {
        std::string invData;
        if (!ReadWholeFile(i->invalidPath, &invData))
            return SetError(error, "read tip-invalid failed (committed set missing)");
        if (!ParseInvalidFile(invData, &invalidEntries))
            return SetError(error, "parse tip-invalid failed (corrupt)");
        if (invalidEntries.size() < meta.invalidLogCount)
            return SetError(error, "tip-invalid short of committed count (corrupt)");
        if (invalidEntries.size() > meta.invalidLogCount)
            invalidEntries.resize(meta.invalidLogCount); // drop uncommitted tail
        unsigned char invDigest[32];
        ComputeInvalidDigest(invalidEntries, invDigest);
        if (memcmp(invDigest, meta.invalidDigest, 32) != 0)
            return SetError(error, "tip-invalid digest mismatch (corrupt)");
    }

    i->records = records;
    i->derived = derived;
    i->activeIds = activeIds;
    i->invalidEntries = invalidEntries;
    DeriveInvalidSet(i->invalidEntries, &i->invalidSet);
    i->open = true;
    ClearError(error);
    return true;
}

BlockIndexTipStatus BlockIndexTipAuthority::Append(const BlockIndexTipAppend& blk,
                                                   int32_t activeHeight,
                                                   std::string* error)
{
    std::vector<BlockIndexTipAppend> blocks(1, blk);
    std::vector<int32_t> heights(1, activeHeight);
    return AppendBatch(blocks, heights, error);
}

BlockIndexTipStatus BlockIndexTipAuthority::AppendBatch(
    const std::vector<BlockIndexTipAppend>& blocks,
    const std::vector<int32_t>& activeHeights,
    std::string* error)
{
    if (!impl->open)
        return SetError(error, "tip not open"), BLOCK_INDEX_TIP_IO_ERROR;
    if (blocks.size() != activeHeights.size())
        return SetError(error, "append batch size mismatch"), BLOCK_INDEX_TIP_CORRUPT;

    Impl* i = impl;
    std::vector<BlockIndexRecord> newRecords;
    std::vector<BlockIndexDerivedEntry> newDerived;
    std::vector<BlockIndexId> newActive;

    for (size_t k = 0; k < blocks.size(); ++k)
    {
        if (i->hashToId.count(blocks[k].record.hash))
            continue; // idempotent replay
        const int32_t ah = activeHeights[k];
        if (ah >= 0)
        {
            // must be dense next active height +1 over committed tip
            if (ah != i->meta.tipHeight + 1)
                return SetError(error, "non-dense active append"), BLOCK_INDEX_TIP_CORRUPT;
        }
        newRecords.push_back(blocks[k].record);
        newDerived.push_back(blocks[k].derived);
        if (ah >= 0)
            newActive.push_back(i->baseLocalToId(i->records.size() + newRecords.size() - 1));
    }

    if (newRecords.empty())
    {
        ClearError(error);
        return BLOCK_INDEX_TIP_OK; // all duplicates: no-op
    }

    // 1. Persist (R4 incremental): append ONLY the newly encoded entries to the
    //    end of each mutable tail store instead of re-encoding + rewriting the
    //    whole retained tail. tip.meta remains the commit point; the appended
    //    bytes are an uncommitted suffix until it is written, and Open truncates
    //    any suffix beyond tip.meta's counts (fail-safe, deterministic).
    std::vector<unsigned char> recBytes, derBytes, actBytes;
    for (size_t k = 0; k < newRecords.size(); ++k)
    {
        std::vector<unsigned char> enc;
        if (!EncodeBlockIndexRecordV1(newRecords[k], &enc, NULL))
            return SetError(error, "encode tip-records entry failed"), BLOCK_INDEX_TIP_IO_ERROR;
        recBytes.insert(recBytes.end(), enc.begin(), enc.end());
    }
    for (size_t k = 0; k < newDerived.size(); ++k)
    {
        std::vector<unsigned char> enc;
        if (!EncodeBlockIndexDerivedEntry(newDerived[k], &enc, NULL))
            return SetError(error, "encode tip-derived entry failed"), BLOCK_INDEX_TIP_IO_ERROR;
        derBytes.insert(derBytes.end(), enc.begin(), enc.end());
    }
    for (size_t k = 0; k < newActive.size(); ++k)
    {
        std::string enc;
        if (!EncodeBlockIndexActiveEntry(newActive[k], &enc, NULL))
            return SetError(error, "encode tip-active entry failed"), BLOCK_INDEX_TIP_IO_ERROR;
        actBytes.insert(actBytes.end(), enc.begin(), enc.end());
    }

    const uint64_t committedRecords = i->records.size();
    const uint64_t committedDerived = i->derived.size();
    const uint64_t committedActive = i->activeIds.size();
    std::string werr;
    // Drop any stale uncommitted suffix first so the new bytes land exactly at the
    // committed boundary (idempotent; no-op after a clean shutdown).
    if (!TruncateStoreToCommitted(i->recordsPath, BLOCK_INDEX_TIP_RECORDS_HEADER_SIZE,
                                  BLOCK_INDEX_RECORD_SIZE_V1, committedRecords, &werr) ||
        !TruncateStoreToCommitted(i->derivedPath, BLOCK_INDEX_TIP_DERIVED_HEADER_SIZE,
                                  BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2, committedDerived, &werr) ||
        !TruncateStoreToCommitted(i->activePath, BLOCK_INDEX_TIP_ACTIVE_HEADER_SIZE,
                                  BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1, committedActive, &werr))
        return SetError(error, "append tip truncate failed: " + werr), BLOCK_INDEX_TIP_IO_ERROR;
    if (!AppendBytesDurable(i->recordsPath, recBytes, &werr))
        return SetError(error, "append tip-records failed: " + werr), BLOCK_INDEX_TIP_IO_ERROR;
    if (!AppendBytesDurable(i->derivedPath, derBytes, &werr))
        return SetError(error, "append tip-derived failed: " + werr), BLOCK_INDEX_TIP_IO_ERROR;
    if (!AppendBytesDurable(i->activePath, actBytes, &werr))
        return SetError(error, "append tip-active failed: " + werr), BLOCK_INDEX_TIP_IO_ERROR;
    if (!UpdateStoreCount(i->recordsPath, BLOCK_INDEX_TIP_RECORDS_COUNT_OFFSET,
                          committedRecords + newRecords.size(), &werr) ||
        !UpdateStoreCount(i->derivedPath, BLOCK_INDEX_TIP_DERIVED_COUNT_OFFSET,
                          committedDerived + newDerived.size(), &werr) ||
        !UpdateStoreCount(i->activePath, BLOCK_INDEX_TIP_ACTIVE_COUNT_OFFSET,
                          committedActive + newActive.size(), &werr))
        return SetError(error, "append tip count update failed: " + werr), BLOCK_INDEX_TIP_IO_ERROR;
    // v2: keep the store set complete and upgrade an opened v1 tip
    // deterministically on the first legitimate new commit.
    if (!WriteInvalidFile(i->invalidPath, i->invalidEntries, &werr))
        return SetError(error, "append tip-invalid failed: " + werr), BLOCK_INDEX_TIP_IO_ERROR;

    // Test-only: all tail stores durable, crash before the tip.meta commit.
    if (BlockIndexTipFailpointHit("FP_AFTER_TAIL_DURABLE_BEFORE_META"))
        return SetError(error, "failpoint FP_AFTER_TAIL_DURABLE_BEFORE_META"), BLOCK_INDEX_TIP_IO_ERROR;

    // 2. Advance tip.meta.
    BlockIndexTipMeta newMeta = i->meta;
    newMeta.version = BLOCK_INDEX_TIP_META_VERSION; // v1 -> v2 upgrade on write
    newMeta.tipRecordCount = committedRecords + newRecords.size();
    const uint64_t totalActive = committedActive + newActive.size();
    if (totalActive == 0)
    {
        newMeta.tipHeight = i->meta.baseTipHeight; // no tip active blocks yet
        newMeta.tipHash = uint256(0);
    }
    else if (!newActive.empty())
    {
        // activeIds is dense RELATIVE to baseTipHeight: the committed append
        // extends it to (committedActive + newActive.size()) entries covering
        // global heights [baseTipHeight+1, baseTipHeight + that count].
        newMeta.tipHeight = i->meta.baseTipHeight + (int32_t)totalActive;
        // Resolve the tip hash by slot arithmetic over the appended window only.
        const BlockIndexId tipId = newActive.back();
        const BlockIndexId firstNew = i->baseLocalToId(committedRecords);
        if (tipId < firstNew || (size_t)(tipId - firstNew) >= newRecords.size())
            return SetError(error, "tip id outside appended window"), BLOCK_INDEX_TIP_CORRUPT;
        newMeta.tipHash = newRecords[(size_t)(tipId - firstNew)].hash;
    }
    else
    {
        // Side-only append: the ACTIVE tip is unchanged (newMeta already holds
        // the committed tip height/hash).
        newMeta.tipHeight = i->meta.tipHeight;
        newMeta.tipHash = i->meta.tipHash;
    }

    // 3. Commit: mutate the in-memory state INCREMENTALLY (O(new)), compute the
    //    content digest, then publish tip.meta. A failed commit rolls the
    //    in-memory append back so authority never runs ahead of the last
    //    committed tip.meta (the appended disk suffix stays uncommitted).
    const BlockIndexTipMeta savedMeta = i->meta;
    i->records.insert(i->records.end(), newRecords.begin(), newRecords.end());
    i->derived.insert(i->derived.end(), newDerived.begin(), newDerived.end());
    i->activeIds.insert(i->activeIds.end(), newActive.begin(), newActive.end());
    for (size_t k = 0; k < newRecords.size(); ++k)
        i->hashToId[newRecords[k].hash] = i->baseLocalToId(committedRecords + k);
    ComputeContentDigest(newMeta, i->records, i->derived, i->activeIds, newMeta.contentDigest);
    i->meta = newMeta;
    if (!i->WriteMeta(error))
    {
        i->meta = savedMeta;
        i->records.resize(committedRecords);
        i->derived.resize(committedDerived);
        i->activeIds.resize(committedActive);
        for (size_t k = 0; k < newRecords.size(); ++k)
            i->hashToId.erase(newRecords[k].hash);
        return BLOCK_INDEX_TIP_IO_ERROR;
    }
    ClearError(error);
    return BLOCK_INDEX_TIP_OK;
}

BlockIndexTipStatus BlockIndexTipAuthority::TruncateActiveTo(int32_t height, std::string* error)
{
    if (!impl->open)
        return SetError(error, "tip not open"), BLOCK_INDEX_TIP_IO_ERROR;
    Impl* i = impl;
    // height is a GLOBAL active height. The empty tip == baseTipHeight (S).
    const int32_t baseTip = i->meta.baseTipHeight;
    if (height < baseTip || height >= baseTip + (int32_t)i->activeIds.size())
        return SetError(error, "truncate height out of range"), BLOCK_INDEX_TIP_CORRUPT;

    // relative active count after truncate = height - baseTip
    std::vector<BlockIndexId> allActive;
    if (height == baseTip)
        allActive.clear();
    else
        allActive.assign(i->activeIds.begin(), i->activeIds.begin() + (height - baseTip));

    if (!WriteActiveFile(i->activePath, allActive))
        return SetError(error, "truncate tip-active failed"), BLOCK_INDEX_TIP_IO_ERROR;

    if (BlockIndexTipFailpointHit("FP_AFTER_TAIL_DURABLE_BEFORE_META"))
        return SetError(error, "failpoint FP_AFTER_TAIL_DURABLE_BEFORE_META"), BLOCK_INDEX_TIP_IO_ERROR;

    BlockIndexTipMeta newMeta = i->meta;
    newMeta.activeFence++;
    newMeta.tipHeight = height;
    if (allActive.empty())
        newMeta.tipHash = uint256(0);
    else
    {
        BlockIndexId tipId = allActive.back();
        for (size_t j = 0; j < i->records.size(); ++j)
            if (i->baseLocalToId(j) == tipId)
                newMeta.tipHash = i->records[j].hash;
    }
    ComputeContentDigest(newMeta, i->records, i->derived, allActive, newMeta.contentDigest);

    const BlockIndexTipMeta savedMeta = i->meta;
    const std::vector<BlockIndexId> savedActive = i->activeIds;
    i->meta = newMeta;
    i->activeIds = allActive;
    if (!i->WriteMeta(error))
    {
        i->meta = savedMeta;
        i->activeIds = savedActive;
        return BLOCK_INDEX_TIP_IO_ERROR;
    }
    ClearError(error);
    return BLOCK_INDEX_TIP_OK;
}

// Reorg the ACTIVE chain to the reconnect branch (fork+1..newTip). This is the
// live-tail's own reorg: it reclassifies records that already exist as SIDE into
// active membership AND appends branch records not yet present, so the active
// chain (dense over [baseTipHeight, baseTipHeight+branch.size()]) exactly equals
// the branch the Reorganize committed. Idempotent Append/AppendBatch cannot do
// this (they skip existing hashes). Fail closed on any inconsistency: the tip is
// left at forkHeight (TruncateActiveTo committed), recoverable by re-running.
BlockIndexTipStatus BlockIndexTipAuthority::ReorgActiveTo(
    int32_t forkHeight,
    const std::vector<BlockIndexTipAppend>& branch,
    const std::vector<int32_t>& branchHeights,
    std::string* error)
{
    if (!impl->open)
        return SetError(error, "tip not open"), BLOCK_INDEX_TIP_IO_ERROR;
    if (branch.size() != branchHeights.size())
        return SetError(error, "reorg branch size mismatch"), BLOCK_INDEX_TIP_CORRUPT;
    Impl* i = impl;
    const int32_t baseTip = i->meta.baseTipHeight;

    // 1. Truncate the ACTIVE chain to the fork (keeps side records, removes the
    //    disconnected branch from active membership). Dense: activeIds covers
    //    [baseTip+1, baseTip+activeIds.size()], so fork must be in range.
    if (forkHeight < baseTip || forkHeight >= baseTip + (int32_t)i->activeIds.size())
    {
        if (forkHeight == baseTip)
        {
            // Already at the empty fork: activeIds empty is fine.
        }
        else
        {
            return SetError(error, "reorg fork height out of range"), BLOCK_INDEX_TIP_CORRUPT;
        }
    }
    std::vector<BlockIndexId> baseActive;
    if (forkHeight > baseTip)
        baseActive.assign(i->activeIds.begin(), i->activeIds.begin() + (forkHeight - baseTip));
    else
        baseActive.clear();

    // 2. Build the new full records/derived/active state. An existing record is
    //    REUSED (regexist side -> promoted to active at its branch height); a
    //    branch record not yet in the tip is APPENDED. Every branch block must be
    //    post-fork and map to the expected dense height.
    std::vector<BlockIndexRecord> allRecords = i->records;
    std::vector<BlockIndexDerivedEntry> allDerived = i->derived;
    std::vector<BlockIndexId> newActive = baseActive; // fork prefix
    std::vector<BlockIndexId> allActive = newActive.empty()
        ? std::vector<BlockIndexId>() : newActive;
    std::set<uint256> seenBranch;
    for (size_t k = 0; k < branch.size(); ++k)
    {
        const BlockIndexRecord& rec = branch[k].record;
        const int32_t expectedHeight = baseTip + (int32_t)newActive.size() + 1;
        if (branchHeights[k] != expectedHeight)
            return SetError(error, "reorg branch non-dense height"), BLOCK_INDEX_TIP_CORRUPT;
        if (!seenBranch.insert(rec.hash).second)
            return SetError(error, "reorg branch duplicate"), BLOCK_INDEX_TIP_CORRUPT;
        std::map<uint256, BlockIndexId>::iterator it = i->hashToId.find(rec.hash);
        if (it != i->hashToId.end())
        {
            // Already present (side record): promote to active.
            newActive.push_back(it->second);
        }
        else
        {
            BlockIndexId newId = i->baseLocalToId(allRecords.size());
            allRecords.push_back(rec);
            allDerived.push_back(branch[k].derived);
            newActive.push_back(newId);
        }
    }
    allActive = newActive;
    // Dense active prefix check: every active member corresponds to a record.
    for (size_t h = 0; h < allActive.size(); ++h)
    {
        bool found=false;
        for (size_t j = 0; j < allRecords.size(); ++j)
            if (i->baseLocalToId(j) == allActive[h]) { found=true; break; }
        if (!found)
            return SetError(error, "reorg active record missing"), BLOCK_INDEX_TIP_CORRUPT;
    }

    // 3. Persist stores (full commit-write); tip.meta stays the commit point.
    if (!WriteRecordsFile(i->recordsPath, allRecords))
        return SetError(error, "reorg tip-records failed"), BLOCK_INDEX_TIP_IO_ERROR;
    if (!WriteDerivedFile(i->derivedPath, allDerived))
        return SetError(error, "reorg tip-derived failed"), BLOCK_INDEX_TIP_IO_ERROR;
    if (!WriteActiveFile(i->activePath, allActive))
        return SetError(error, "reorg tip-active failed"), BLOCK_INDEX_TIP_IO_ERROR;

    // 4. Advance tip.meta (dense tipHeight + tipHash), rebuild hashToId.
    BlockIndexTipMeta newMeta = i->meta;
    newMeta.activeFence++;
    newMeta.tipRecordCount = allRecords.size();
    newMeta.tipHeight = baseTip + (int32_t)allActive.size();
    if (allActive.empty())
        newMeta.tipHash = uint256(0);
    else
    {
        BlockIndexId tipId = allActive.back();
        for (size_t j = 0; j < allRecords.size(); ++j)
            if (i->baseLocalToId(j) == tipId) newMeta.tipHash = allRecords[j].hash;
    }
    ComputeContentDigest(newMeta, allRecords, allDerived, allActive, newMeta.contentDigest);
    // Commit point: adopt the new meta BEFORE writing it, exactly as
    // AppendBatch/TruncateActiveTo do. Writing meta while i->meta still holds the
    // pre-reorg state would commit the NEW stores (records/derived/active) against
    // the OLD meta (tipRecordCount/tipHeight/contentDigest), so the next Open
    // truncates the freshly-published reorg branch back to the pre-reorg commit
    // point and reports a content-digest mismatch (silent loss of the cutover).
    // Test-only: all tail stores durable, crash before the tip.meta commit.
    if (BlockIndexTipFailpointHit("FP_AFTER_TAIL_DURABLE_BEFORE_META"))
        return SetError(error, "failpoint FP_AFTER_TAIL_DURABLE_BEFORE_META"), BLOCK_INDEX_TIP_IO_ERROR;

    const BlockIndexTipMeta savedMeta = i->meta;
    const std::vector<BlockIndexRecord> savedRecords = i->records;
    const std::vector<BlockIndexDerivedEntry> savedDerived = i->derived;
    const std::vector<BlockIndexId> savedActive = i->activeIds;
    i->meta = newMeta;
    if (!i->WriteMeta(error))
    {
        i->meta = savedMeta;
        i->records = savedRecords;
        i->derived = savedDerived;
        i->activeIds = savedActive;
        i->hashToId.clear();
        for (size_t j = 0; j < i->records.size(); ++j)
            i->hashToId[i->records[j].hash] = i->baseLocalToId(j);
        return BLOCK_INDEX_TIP_IO_ERROR;
    }
    i->records = allRecords;
    i->derived = allDerived;
    i->activeIds = allActive;
    i->hashToId.clear();
    for (size_t j = 0; j < allRecords.size(); ++j)
        i->hashToId[allRecords[j].hash] = i->baseLocalToId(j);
    ClearError(error);
    return BLOCK_INDEX_TIP_OK;
}

BlockIndexTipStatus BlockIndexTipAuthority::ApplyOperatorInvalidAndReorg(
    const uint256& hash, bool invalidate,
    int32_t forkHeight,
    const std::vector<BlockIndexTipAppend>& branch,
    const std::vector<int32_t>& branchHeights,
    std::string* error)
{
    if (!impl->open)
        return SetError(error, "tip not open"), BLOCK_INDEX_TIP_IO_ERROR;
    if (branch.size() != branchHeights.size())
        return SetError(error, "fused reorg branch size mismatch"), BLOCK_INDEX_TIP_CORRUPT;
    Impl* i = impl;
    const int32_t baseTip = i->meta.baseTipHeight;

    // (a) operator-invalid intent -- idempotent (no log entry if unchanged).
    std::vector<BlockIndexTipInvalidEntry> allEntries = i->invalidEntries;
    const bool currentlyInvalid = i->invalidSet.count(hash) != 0;
    if (currentlyInvalid != invalidate)
    {
        BlockIndexTipInvalidEntry ent;
        ent.hash = hash;
        ent.intent = invalidate ? 1 : 0;
        allEntries.push_back(ent);
    }

    // (b) resulting active membership (fused from ReorgActiveTo steps 1-2).
    if (forkHeight < baseTip || forkHeight >= baseTip + (int32_t)i->activeIds.size())
    {
        if (forkHeight != baseTip)
            return SetError(error, "fused reorg fork height out of range"), BLOCK_INDEX_TIP_CORRUPT;
    }
    std::vector<BlockIndexId> baseActive;
    if (forkHeight > baseTip)
        baseActive.assign(i->activeIds.begin(), i->activeIds.begin() + (forkHeight - baseTip));

    std::vector<BlockIndexRecord> allRecords = i->records;
    std::vector<BlockIndexDerivedEntry> allDerived = i->derived;
    std::vector<BlockIndexId> newActive = baseActive;
    std::set<uint256> seenBranch;
    for (size_t k = 0; k < branch.size(); ++k)
    {
        const BlockIndexRecord& rec = branch[k].record;
        const int32_t expectedHeight = baseTip + (int32_t)newActive.size() + 1;
        if (branchHeights[k] != expectedHeight)
            return SetError(error, "fused reorg branch non-dense height"), BLOCK_INDEX_TIP_CORRUPT;
        if (!seenBranch.insert(rec.hash).second)
            return SetError(error, "fused reorg branch duplicate"), BLOCK_INDEX_TIP_CORRUPT;
        std::map<uint256, BlockIndexId>::iterator it = i->hashToId.find(rec.hash);
        if (it != i->hashToId.end())
            newActive.push_back(it->second); // promote existing side record
        else
        {
            BlockIndexId newId = i->baseLocalToId(allRecords.size());
            allRecords.push_back(rec);
            allDerived.push_back(branch[k].derived);
            newActive.push_back(newId);
        }
    }
    std::vector<BlockIndexId> allActive = newActive;

    // Persist all four stores. tip.meta below is the SOLE commit point for BOTH
    // the invalid log and the active membership (uncommitted tails are ignored).
    if (!WriteRecordsFile(i->recordsPath, allRecords))
        return SetError(error, "fused tip-records failed"), BLOCK_INDEX_TIP_IO_ERROR;
    if (!WriteDerivedFile(i->derivedPath, allDerived))
        return SetError(error, "fused tip-derived failed"), BLOCK_INDEX_TIP_IO_ERROR;
    if (!WriteActiveFile(i->activePath, allActive))
        return SetError(error, "fused tip-active failed"), BLOCK_INDEX_TIP_IO_ERROR;
    if (!WriteInvalidFile(i->invalidPath, allEntries))
        return SetError(error, "fused tip-invalid failed"), BLOCK_INDEX_TIP_IO_ERROR;

    // ONE tip.meta commit: invalid intent AND the resulting active tip together.
    const BlockIndexTipMeta savedMeta = i->meta;
    const std::vector<BlockIndexRecord> savedRecords = i->records;
    const std::vector<BlockIndexDerivedEntry> savedDerived = i->derived;
    const std::vector<BlockIndexId> savedActive = i->activeIds;
    const std::vector<BlockIndexTipInvalidEntry> savedEntries = i->invalidEntries;

    BlockIndexTipMeta newMeta = i->meta;
    newMeta.version = BLOCK_INDEX_TIP_META_VERSION;
    newMeta.activeFence++;
    newMeta.tipRecordCount = allRecords.size();
    newMeta.tipHeight = baseTip + (int32_t)allActive.size();
    if (allActive.empty())
        newMeta.tipHash = uint256(0);
    else
    {
        BlockIndexId tipId = allActive.back();
        for (size_t j = 0; j < allRecords.size(); ++j)
            if (i->baseLocalToId(j) == tipId) newMeta.tipHash = allRecords[j].hash;
    }
    ComputeContentDigest(newMeta, allRecords, allDerived, allActive, newMeta.contentDigest);
    newMeta.invalidLogCount = (uint32_t)allEntries.size();
    ComputeInvalidDigest(allEntries, newMeta.invalidDigest);

    if (BlockIndexTipFailpointHit("FP_AFTER_TAIL_DURABLE_BEFORE_META"))
        return SetError(error, "failpoint FP_AFTER_TAIL_DURABLE_BEFORE_META"), BLOCK_INDEX_TIP_IO_ERROR;

    i->meta = newMeta;
    i->invalidEntries = allEntries;
    DeriveInvalidSet(i->invalidEntries, &i->invalidSet);
    if (!i->WriteMeta(error))
    {
        i->meta = savedMeta;
        i->records = savedRecords;
        i->derived = savedDerived;
        i->activeIds = savedActive;
        i->invalidEntries = savedEntries;
        DeriveInvalidSet(i->invalidEntries, &i->invalidSet);
        return BLOCK_INDEX_TIP_IO_ERROR;
    }
    i->records = allRecords;
    i->derived = allDerived;
    i->activeIds = allActive;
    i->hashToId.clear();
    for (size_t j = 0; j < allRecords.size(); ++j)
        i->hashToId[allRecords[j].hash] = i->baseLocalToId(j);
    ClearError(error);
    return BLOCK_INDEX_TIP_OK;
}

BlockIndexTipRead BlockIndexTipAuthority::GetTip() const
{
    BlockIndexTipRead r;
    if (!impl->open)
    {
        r.status = BLOCK_INDEX_TIP_IO_ERROR;
        return r;
    }
    if (impl->activeIds.empty())
    {
        r.status = BLOCK_INDEX_TIP_NOT_FOUND;
        return r;
    }
    BlockIndexId tipId = impl->activeIds.back();
    for (size_t j = 0; j < impl->records.size(); ++j)
        if (impl->baseLocalToId(j) == tipId)
        {
            r.status = BLOCK_INDEX_TIP_OK;
            r.record = impl->records[j];
            r.derived = impl->derived[j];
            r.active = true;
            r.height = impl->records[j].height;
            return r;
        }
    r.status = BLOCK_INDEX_TIP_CORRUPT;
    return r;
}

BlockIndexTipRead BlockIndexTipAuthority::LookupByHash(const uint256& hash, std::string* error) const
{
    BlockIndexTipRead r;
    if (!impl->open)
    {
        r.status = BLOCK_INDEX_TIP_IO_ERROR;
        return r;
    }
    std::map<uint256, BlockIndexId>::const_iterator it = impl->hashToId.find(hash);
    if (it == impl->hashToId.end())
    {
        r.status = BLOCK_INDEX_TIP_NOT_FOUND;
        return r;
    }
    // baseLocalToId(j) == baseRecordCount + j + 1 is an affine map, so the record
    // slot for a RecordId is a DIRECT computation. The former per-call linear
    // scan over the whole records vector (and a second scan over activeIds)
    // dominated fresh-IBD CPU: each by-value lookup walked the entire retained
    // tail. Results are identical; only the lookup cost changes (O(N) -> O(1)).
    const BlockIndexId id = it->second;
    const uint64_t base = impl->meta.baseRecordCount;
    if (id <= base)
    {
        r.status = BLOCK_INDEX_TIP_CORRUPT;
        return r;
    }
    const uint64_t slot = id - base - 1;
    if (slot >= impl->records.size() || slot >= impl->derived.size())
    {
        r.status = BLOCK_INDEX_TIP_CORRUPT;
        return r;
    }
    r.status = BLOCK_INDEX_TIP_OK;
    r.record = impl->records[(size_t)slot];
    r.derived = impl->derived[(size_t)slot];
    r.height = impl->records[(size_t)slot].height;
    // Active membership: activeIds is dense by relative index (activeIds[rel] is
    // the active RecordId at global height baseTipHeight + rel + 1), so a record
    // is active iff it occupies its own height's slot. Equivalent to the former
    // scan over activeIds, without the O(activeIds) cost.
    r.active = false;
    const int32_t h = impl->records[(size_t)slot].height;
    if (h > impl->meta.baseTipHeight)
    {
        const uint64_t rel = (uint64_t)(h - impl->meta.baseTipHeight - 1);
        if (rel < impl->activeIds.size() && impl->activeIds[(size_t)rel] == id)
            r.active = true;
    }
    return r;
}

BlockIndexTipRead BlockIndexTipAuthority::LookupActiveByHeight(int32_t height, std::string* error) const
{
    BlockIndexTipRead r;
    if (!impl->open)
    {
        r.status = BLOCK_INDEX_TIP_IO_ERROR;
        return r;
    }
    // height is a GLOBAL active height. activeIds[rel] == global height
    // baseTipHeight + rel + 1. So rel = height - baseTipHeight - 1.
    const int32_t baseTip = impl->meta.baseTipHeight;
    const int64_t rel = (int64_t)height - (int64_t)baseTip - 1;
    if (rel < 0 || rel >= (int64_t)impl->activeIds.size())
    {
        r.status = BLOCK_INDEX_TIP_NOT_FOUND;
        return r;
    }
    // R5: the id->record mapping is pure arithmetic (id == baseRecordCount +
    // slot + 1), exactly like LookupByHash. The previous O(records) linear scan
    // ran on every by-height lookup (the hot live-tail/seam path) and dominated
    // the block-connect profile after the append repair landed.
    const BlockIndexId id = impl->activeIds[(size_t)rel];
    const int64_t slot = (int64_t)id - (int64_t)impl->meta.baseRecordCount - 1;
    if (slot < 0 || slot >= (int64_t)impl->records.size())
    {
        r.status = BLOCK_INDEX_TIP_CORRUPT;
        return r;
    }
    r.status = BLOCK_INDEX_TIP_OK;
    r.record = impl->records[(size_t)slot];
    r.derived = impl->derived[(size_t)slot];
    r.active = true;
    r.height = impl->records[(size_t)slot].height;
    return r;
}

BlockIndexTipStatus BlockIndexTipAuthority::AllRecords(std::vector<BlockIndexTipRead>* out,
                                                       std::string* error) const
{
    if (out) out->clear();
    if (!impl->open)
    {
        if (error) *error = "tip not open";
        return BLOCK_INDEX_TIP_IO_ERROR;
    }
    for (size_t j = 0; j < impl->records.size(); ++j)
    {
        BlockIndexId id = impl->baseLocalToId(j);
        BlockIndexTipRead r;
        r.status = BLOCK_INDEX_TIP_OK;
        r.record = impl->records[j];
        r.derived = impl->derived[j];
        r.height = impl->records[j].height;
        r.active = false;
        for (size_t h = 0; h < impl->activeIds.size(); ++h)
            if (impl->activeIds[h] == id) { r.active = true; break; }
        if (out) out->push_back(r);
    }
    return BLOCK_INDEX_TIP_OK;
}

BlockIndexTipStatus BlockIndexTipAuthority::SelectBestEligibleBranch(
    int32_t forkHeight, const uint256& pendingHash, bool pendingInvalidate,
    std::vector<BlockIndexTipAppend>* outBranch, std::vector<int32_t>* outHeights,
    uint256* outBestHash, int32_t* outBestHeight, std::string* error) const
{
    if (outBranch) outBranch->clear();
    if (outHeights) outHeights->clear();
    if (outBestHash) *outBestHash = 0;
    if (outBestHeight) *outBestHeight = forkHeight;
    if (!impl->open)
    {
        if (error) *error = "tip not open";
        return BLOCK_INDEX_TIP_IO_ERROR;
    }

    std::map<uint256, size_t> localOf;
    for (size_t j = 0; j < impl->records.size(); ++j)
        localOf[impl->records[j].hash] = j;

    bool found = false;
    size_t bestJ = 0;
    for (size_t j = 0; j < impl->records.size(); ++j)
    {
        const BlockIndexRecord& rec = impl->records[j];
        if (rec.height <= forkHeight) continue;          // at/below the fork
        // Effective eligibility: the PENDING intent overrides the committed set for
        // the target hash (invalidate => treat as invalid; reconsider => treat as
        // eligible), so a reconsider can re-select the branch it restores.
        bool selfInvalid = (rec.hash == pendingHash)
                               ? pendingInvalidate
                               : (impl->invalidSet.count(rec.hash) != 0);
        if (selfInvalid) continue;
        // Authenticate ancestry up to the fork: every intermediate must be present
        // in the tip window, height-consistent, and not operator-invalid.
        int32_t h = rec.height;
        uint256 cur = rec.hashPrev;
        bool ok = true;
        // Walk down to the fork-adjacent node (height forkHeight+1). Its parent IS
        // the fork itself, which may be the immutable base tip and is therefore NOT
        // in the tip record window -- do not require it to be present here.
        while (h > forkHeight + 1)
        {
            bool ancInvalid = (cur == pendingHash)
                                  ? pendingInvalidate
                                  : (impl->invalidSet.count(cur) != 0);
            if (ancInvalid) { ok = false; break; }
            std::map<uint256, size_t>::const_iterator it = localOf.find(cur);
            if (it == localOf.end()) { ok = false; break; }  // leaves the tip window
            const BlockIndexRecord& p = impl->records[it->second];
            if (p.height + 1 != h) { ok = false; break; }    // inconsistent linkage
            cur = p.hashPrev;
            h = p.height;
        }
        if (!ok) continue;
        if (!found ||
            impl->derived[j].chainTrust > impl->derived[bestJ].chainTrust ||
            (impl->derived[j].chainTrust == impl->derived[bestJ].chainTrust &&
             rec.hash < impl->records[bestJ].hash))
        {
            found = true;
            bestJ = j;
        }
    }

    if (!found)
        return BLOCK_INDEX_TIP_OK;   // no eligible candidate above fork -> tip = fork

    // Reconstruct the branch (fork+1 .. bestTip) ascending into a temp vector.
    std::vector<size_t> chain;
    size_t j = bestJ;
    while (true)
    {
        chain.push_back(j);
        if (impl->records[j].height <= forkHeight + 1)
            break;   // fork-adjacent node: its parent is the fork (not in window)
        std::map<uint256, size_t>::const_iterator it = localOf.find(impl->records[j].hashPrev);
        if (it == localOf.end())
        {
            if (error) *error = "branch walk lost parent below tip window";
            return BLOCK_INDEX_TIP_CORRUPT;
        }
        j = it->second;
    }
    for (size_t k = chain.size(); k-- > 0; )
    {
        size_t idx = chain[k];
        BlockIndexTipAppend ap;
        ap.record = impl->records[idx];
        ap.derived = impl->derived[idx];
        if (outBranch) outBranch->push_back(ap);
        if (outHeights) outHeights->push_back(impl->records[idx].height);
    }
    if (outBestHash) *outBestHash = impl->records[bestJ].hash;
    if (outBestHeight) *outBestHeight = impl->records[bestJ].height;
    return BLOCK_INDEX_TIP_OK;
}

BlockIndexTipRead BlockIndexTipAuthority::LookupParent(const uint256& childHash, std::string* error) const
{
    BlockIndexTipRead child = LookupByHash(childHash, error);
    if (child.status != BLOCK_INDEX_TIP_OK)
        return child;
    return LookupByHash(child.record.hashPrev, error);
}

BlockIndexTipRead BlockIndexTipAuthority::LookupNextActive(const uint256& curHash, std::string* error) const
{
    BlockIndexTipRead cur = LookupByHash(curHash, error);
    if (cur.status != BLOCK_INDEX_TIP_OK || !cur.active)
    {
        BlockIndexTipRead r;
        r.status = BLOCK_INDEX_TIP_NOT_ACTIVE;
        return r;
    }
    return LookupActiveByHeight(cur.height + 1, error);
}

uint64_t BlockIndexTipAuthority::BaseGeneration() const { return impl->meta.baseGeneration; }
uint64_t BlockIndexTipAuthority::BaseRecordCount() const { return impl->meta.baseRecordCount; }
int32_t BlockIndexTipAuthority::TipHeight() const { return impl->meta.tipHeight; }
uint256 BlockIndexTipAuthority::TipHash() const { return impl->meta.tipHash; }
uint64_t BlockIndexTipAuthority::TipRecordCount() const { return impl->meta.tipRecordCount; }
uint8_t BlockIndexTipAuthority::ActiveFence() const { return impl->meta.activeFence; }
bool BlockIndexTipAuthority::IsOpen() const { return impl->open; }
bool BlockIndexTipAuthority::IsEmpty() const { return impl->open && impl->records.empty(); }

// ---- v2 durable operator-invalid authority ----
uint32_t BlockIndexTipAuthority::InvalidLogCount() const
{
    return impl->meta.invalidLogCount;
}

bool BlockIndexTipAuthority::IsOperatorInvalid(const uint256& hash) const
{
    return impl->invalidSet.count(hash) != 0;
}

std::set<uint256> BlockIndexTipAuthority::OperatorInvalidSet() const
{
    return impl->invalidSet;
}

BlockIndexTipStatus BlockIndexTipAuthority::SetOperatorInvalid(const uint256& hash,
                                                               bool invalidate,
                                                               std::string* error)
{
    if (!impl->open)
        return SetError(error, "tip not open"), BLOCK_INDEX_TIP_IO_ERROR;
    Impl* i = impl;

    // Idempotent: an intent that does not change the derived state is a no-op
    // (no log entry, no commit). Keeps the log bounded by real transitions.
    const bool currentlyInvalid = i->invalidSet.count(hash) != 0;
    if (currentlyInvalid == invalidate)
    {
        ClearError(error);
        return BLOCK_INDEX_TIP_OK;
    }

    // Append the intent. The log is append-only and replay-ordered (last intent
    // for a hash wins), so this keeps tip.meta as the sole commit point.
    std::vector<BlockIndexTipInvalidEntry> allEntries = i->invalidEntries;
    BlockIndexTipInvalidEntry ent;
    ent.hash = hash;
    ent.intent = invalidate ? 1 : 0;
    allEntries.push_back(ent);

    // 1. Persist the extended log. Still guarded by the OLD tip.meta commit
    //    point: if the meta write below fails, Open truncates the extra entry.
    if (!WriteInvalidFile(i->invalidPath, allEntries))
        return SetError(error, "write tip-invalid failed"), BLOCK_INDEX_TIP_IO_ERROR;

    // 2. Commit point: advance tip.meta with the new committed length + digest.
    //    The operator-invalid set is thereby part of the SAME mutable authority
    //    publication as the committed tip.
    const BlockIndexTipMeta savedMeta = i->meta;
    const std::vector<BlockIndexTipInvalidEntry> savedEntries = i->invalidEntries;
    BlockIndexTipMeta newMeta = i->meta;
    newMeta.version = BLOCK_INDEX_TIP_META_VERSION;
    newMeta.invalidLogCount = (uint32_t)allEntries.size();
    ComputeInvalidDigest(allEntries, newMeta.invalidDigest);

    if (BlockIndexTipFailpointHit("FP_AFTER_TAIL_DURABLE_BEFORE_META"))
        return SetError(error, "failpoint FP_AFTER_TAIL_DURABLE_BEFORE_META"), BLOCK_INDEX_TIP_IO_ERROR;

    i->meta = newMeta;
    i->invalidEntries = allEntries;
    DeriveInvalidSet(i->invalidEntries, &i->invalidSet);
    if (!i->WriteMeta(error))
    {
        // Roll the in-memory authority back so it matches the committed disk
        // state (the caller sees a failed publish, not a phantom one).
        i->meta = savedMeta;
        i->invalidEntries = savedEntries;
        DeriveInvalidSet(i->invalidEntries, &i->invalidSet);
        return BLOCK_INDEX_TIP_IO_ERROR;
    }
    ClearError(error);
    return BLOCK_INDEX_TIP_OK;
}

void BlockIndexTipAuthority::Close()
{
    impl->open = false;
}