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

static bool WriteInvalidFile(const fs::path& path,
                             const std::vector<BlockIndexTipInvalidEntry>& entries)
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
    FILE* f = fopen(path.string().c_str(), "wb");
    if (!f)
        return false;
    if (!b.empty() && fwrite(&b[0], 1, b.size(), f) != b.size())
    {
        fclose(f);
        return false;
    }
    FileCommit(f);
    fclose(f);
    return true;
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

// Re-encode records.dat / active.dat / derived.dat committed region from the
// in-memory committed state (used when repairing an uncommitted tail).
static bool WriteRecordsFile(const fs::path& path, const std::vector<BlockIndexRecord>& records)
{
    FILE* f = fopen(path.string().c_str(), "wb");
    if (!f)
        return false;
    std::vector<unsigned char> hdr;
    WriteRawLE32(hdr, BLOCK_INDEX_FORMAT_VERSION);
    WriteRawLE32(hdr, BLOCK_INDEX_RECORD_VERSION);
    WriteRawLE32(hdr, BLOCK_INDEX_TIP_RECORDS_HEADER_SIZE);
    WriteRawLE32(hdr, BLOCK_INDEX_RECORD_SIZE_V1);
    WriteRawLE64(hdr, records.size());
    WriteRawLE64(hdr, 0); WriteRawLE64(hdr, 0); WriteRawLE64(hdr, 0);
    if (fwrite(&hdr[0], 1, hdr.size(), f) != hdr.size()) { fclose(f); return false; }
    for (size_t i = 0; i < records.size(); ++i)
    {
        std::vector<unsigned char> enc;
        if (!EncodeBlockIndexRecordV1(records[i], &enc, NULL))
        {
            fclose(f);
            return false;
        }
        if (fwrite(&enc[0], 1, enc.size(), f) != enc.size()) { fclose(f); return false; }
    }
    FileCommit(f);
    fclose(f);
    return true;
}

static bool WriteActiveFile(const fs::path& path, const std::vector<BlockIndexId>& ids)
{
    FILE* f = fopen(path.string().c_str(), "wb");
    if (!f)
        return false;
    std::vector<unsigned char> hdr;
    WriteRawLE32(hdr, BLOCK_INDEX_ACTIVE_SCHEMA_VERSION);
    WriteRawLE32(hdr, BLOCK_INDEX_TIP_ACTIVE_HEADER_SIZE);
    WriteRawLE32(hdr, BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1);
    WriteRawLE64(hdr, 0); WriteRawLE64(hdr, 0); WriteRawLE64(hdr, 0); WriteRawLE64(hdr, 0);
    if (fwrite(&hdr[0], 1, hdr.size(), f) != hdr.size()) { fclose(f); return false; }
    for (size_t i = 0; i < ids.size(); ++i)
    {
        std::string enc;
        if (!EncodeBlockIndexActiveEntry(ids[i], &enc, NULL))
        {
            fclose(f);
            return false;
        }
        if (fwrite(enc.data(), 1, enc.size(), f) != enc.size()) { fclose(f); return false; }
    }
    FileCommit(f);
    fclose(f);
    return true;
}

static bool WriteDerivedFile(const fs::path& path, const std::vector<BlockIndexDerivedEntry>& derived)
{
    FILE* f = fopen(path.string().c_str(), "wb");
    if (!f)
        return false;
    std::vector<unsigned char> hdr;
    WriteRawLE32(hdr, BLOCK_INDEX_DERIVED_FORMAT_VERSION);
    WriteRawLE32(hdr, BLOCK_INDEX_DERIVED_SCHEMA_VERSION);
    WriteRawLE32(hdr, BLOCK_INDEX_TIP_DERIVED_HEADER_SIZE);
    WriteRawLE32(hdr, BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2);
    WriteRawLE64(hdr, 0);
    WriteRawLE64(hdr, derived.size());
    for (int i = 0; i < 32; ++i) hdr.push_back(0);
    WriteRawLE64(hdr, 0);
    if (fwrite(&hdr[0], 1, hdr.size(), f) != hdr.size()) { fclose(f); return false; }
    for (size_t i = 0; i < derived.size(); ++i)
    {
        std::vector<unsigned char> enc;
        if (!EncodeBlockIndexDerivedEntry(derived[i], &enc, NULL))
        {
            fclose(f);
            return false;
        }
        if (fwrite(&enc[0], 1, enc.size(), f) != enc.size()) { fclose(f); return false; }
    }
    FileCommit(f);
    fclose(f);
    return true;
}

} // namespace

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
        FileCommit(f);
        fclose(f);
        if (!RenameOver(tmp, metaPath))
            return SetError(error, "rename tip.meta failed");
        int fd = ::open(tipDir.string().c_str(), O_RDONLY);
        if (fd >= 0)
        {
            fsync(fd);
            ::close(fd);
        }
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

    // 1. Persist stores (full commit-write of the extended committed region).
    //    tip.meta remains the commit point; if a later write or the meta write
    //    fails, the stores are ahead of tip.meta and Open truncates them back
    //    to the committed tip (fail-safe, deterministic).
    std::vector<BlockIndexRecord> allRecords = i->records;
    allRecords.insert(allRecords.end(), newRecords.begin(), newRecords.end());
    std::vector<BlockIndexDerivedEntry> allDerived = i->derived;
    allDerived.insert(allDerived.end(), newDerived.begin(), newDerived.end());
    std::vector<BlockIndexId> allActive = i->activeIds;
    allActive.insert(allActive.end(), newActive.begin(), newActive.end());

    if (!WriteRecordsFile(i->recordsPath, allRecords))
        return SetError(error, "append tip-records failed"), BLOCK_INDEX_TIP_IO_ERROR;
    if (!WriteDerivedFile(i->derivedPath, allDerived))
        return SetError(error, "append tip-derived failed"), BLOCK_INDEX_TIP_IO_ERROR;
    if (!WriteActiveFile(i->activePath, allActive))
        return SetError(error, "append tip-active failed"), BLOCK_INDEX_TIP_IO_ERROR;
    // v2: keep the store set complete and upgrade an opened v1 tip
    // deterministically on the first legitimate new commit.
    if (!WriteInvalidFile(i->invalidPath, i->invalidEntries))
        return SetError(error, "append tip-invalid failed"), BLOCK_INDEX_TIP_IO_ERROR;

    // 2. Advance tip.meta.
    BlockIndexTipMeta newMeta = i->meta;
    newMeta.version = BLOCK_INDEX_TIP_META_VERSION; // v1 -> v2 upgrade on write
    newMeta.tipRecordCount = allRecords.size();
    if (!allActive.empty())
    {
        // activeIds is dense RELATIVE to baseTipHeight: allActive.size() entries
        // cover global heights [baseTipHeight+1, baseTipHeight+allActive.size()].
        newMeta.tipHeight = i->meta.baseTipHeight + (int32_t)allActive.size();
        BlockIndexId tipId = allActive.back();
        for (size_t j = 0; j < allRecords.size(); ++j)
            if (i->baseLocalToId(j) == tipId)
                newMeta.tipHash = allRecords[j].hash;
    }
    else
    {
        newMeta.tipHeight = i->meta.baseTipHeight; // no tip active blocks yet
        newMeta.tipHash = uint256(0);
    }
    ComputeContentDigest(newMeta, allRecords, allDerived, allActive, newMeta.contentDigest);

    // 3. Commit point: write tip.meta.
    i->meta = newMeta;
    i->records = allRecords;
    i->derived = allDerived;
    i->activeIds = allActive;
    for (size_t k = 0; k < newRecords.size(); ++k)
        i->hashToId[newRecords[k].hash] = i->baseLocalToId(i->records.size() - newRecords.size() + k);
    if (!i->WriteMeta(error))
        return BLOCK_INDEX_TIP_IO_ERROR;
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

    i->meta = newMeta;
    i->activeIds = allActive;
    if (!i->WriteMeta(error))
        return BLOCK_INDEX_TIP_IO_ERROR;
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
    i->meta = newMeta;
    if (!i->WriteMeta(error))
        return BLOCK_INDEX_TIP_IO_ERROR;
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
    BlockIndexId id = it->second;
    for (size_t j = 0; j < impl->records.size(); ++j)
        if (impl->baseLocalToId(j) == id)
        {
            r.status = BLOCK_INDEX_TIP_OK;
            r.record = impl->records[j];
            r.derived = impl->derived[j];
            r.height = impl->records[j].height;
            r.active = false;
            for (size_t h = 0; h < impl->activeIds.size(); ++h)
                if (impl->activeIds[h] == id)
                {
                    r.active = true;
                    break;
                }
            return r;
        }
    r.status = BLOCK_INDEX_TIP_CORRUPT;
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
    BlockIndexId id = impl->activeIds[(size_t)rel];
    for (size_t j = 0; j < impl->records.size(); ++j)
        if (impl->baseLocalToId(j) == id)
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