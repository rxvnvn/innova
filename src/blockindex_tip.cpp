// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "blockindex_tip.h"

#include "blockindex_tip_bounded_window.h"
#include "blockindex_hashindex.h"

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
    else if (m.version == BLOCK_INDEX_TIP_META_VERSION ||
             m.version == BLOCK_INDEX_TIP_META_VERSION_V3)
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

// ---- Repair #3: bounded-memory tip store windows ----------------------------
// Caps: the suffix ring holds at most TIP_RING_CAP_ENTRIES newest entries of
// each store (the live-tail working set); the LRU caps TOTAL decoded bytes
// across entries. Both are compile-time ABI constants so tip-authority RAM
// is provably bounded: O(ring + lru + new appends), never O(chain height).
static const size_t BLOCK_INDEX_TIP_RING_CAP_ENTRIES = 65536;
static const size_t BLOCK_INDEX_TIP_LRU_BYTES_CAP = 2 * 1048576; // 2 MB

// Concrete window instantiations (decode via the existing store codecs).
typedef TipStoreWindow<BlockIndexRecord> TipStoreRecordsWindow;
typedef TipStoreWindow<BlockIndexDerivedEntry> TipStoreDerivedWindow;
typedef TipStoreWindow<BlockIndexId> TipStoreActiveWindow;

static bool TipDecodeRecord(const unsigned char* data, size_t size,
                            BlockIndexRecord* out, std::string* error)
{
    return DecodeBlockIndexRecordV1(data, size, out, error);
}
static bool TipDecodeDerived(const unsigned char* data, size_t size,
                             BlockIndexDerivedEntry* out, std::string* error)
{
    return DecodeBlockIndexDerivedEntry(data, size, out, error);
}
static bool TipDecodeActive(const unsigned char* data, size_t size,
                            BlockIndexId* out, std::string* error)
{
    return DecodeBlockIndexActiveEntry((const char*)data, size, out, error);
}

// ---- Repair #3: durable hash->RecordId side index ----------------------------
// The index is DERIVED state and NEVER a second commit point: tip.meta remains
// the single durable authority. LevelDB directory <tipDir>/tip-hashindex maps
// the tip namespace hashes of slots [0, tipRecordCount) to RecordIds
// (id == baseRecordCount + slot + 1).
//
// Indexed-count marker: the wrapper BlockIndexHashIndex is a frozen primitive
// (its Put/Lookup channel covers only 'h'-prefixed 33-byte hash keys and its
// meta keys meta:* are validated at Open). Rather than fork it, the marker is
// a small CoW sidecar FILE <tipDir>/tip-hashindex.marker (magic + version +
// LE64 count, tmp+fsync+rename+dirsync, same discipline as every tip store).
// A missing/stale/mismatched marker simply forces a wider rebuild; it is
// never authoritative.
//
// Generation binding: BlockIndexHashIndex validates its own meta:generation.
// The tip index uses a PID-UNIQUE nonce at Create; on Open the expected
// generation is passed 0 (wildcard) and the reconcile below treats an
// unfriendly directory as "rebuild".
static const char* const BLOCK_INDEX_TIP_HASHINDEX_DIR_NAME = "tip-hashindex";
static const char* const BLOCK_INDEX_TIP_HASHINDEX_MARKER_FILE = "tip-hashindex.marker";
static const uint32_t BLOCK_INDEX_TIP_HASHINDEX_MARKER_MAGIC = 0x494E4E54; // "TNNI"
static const uint32_t BLOCK_INDEX_TIP_HASHINDEX_MARKER_VERSION = 1;
static const size_t BLOCK_INDEX_TIP_HASHINDEX_MARKER_SIZE = 4 + 4 + 8; // magic+ver+count

// Records: hash(32) || hashPrev(32) || height(4 LE).  Derived: chainTrust(32) ||
// stakeModifierChecksum(4 LE).  Active: RecordId(8 LE). Byte-for-byte identical
// to the original inline v2 emissions -- the v2 digest value is unchanged.
static size_t EncodeRecordDigestBytes(const BlockIndexRecord& r, unsigned char* out)
{
    memcpy(out, r.hash.begin(), 32);
    memcpy(out + 32, r.hashPrev.begin(), 32);
    for (int j = 0; j < 4; ++j)
        out[64 + j] = (unsigned char)((r.height >> (8 * j)) & 0xff);
    return 68;
}

static size_t EncodeDerivedDigestBytes(const BlockIndexDerivedEntry& d, unsigned char* out)
{
    memcpy(out, d.chainTrust.begin(), 32);
    for (int j = 0; j < 4; ++j)
        out[32 + j] = (unsigned char)((d.stakeModifierChecksum >> (8 * j)) & 0xff);
    return 36;
}

static void EncodeActiveDigestBytes(BlockIndexId id, unsigned char* out)
{
    for (int j = 0; j < 8; ++j)
        out[j] = (unsigned char)((id >> (8 * j)) & 0xff);
}

// v2 streaming digest: SHA256( R_all || D_all || A_all || fence ). UNCHANGED.
static void ComputeContentDigest(const BlockIndexTipMeta& meta,
                                 const std::vector<BlockIndexRecord>& records,
                                 const std::vector<BlockIndexDerivedEntry>& derived,
                                 const std::vector<BlockIndexId>& activeIds,
                                 unsigned char digest[32])
{
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    unsigned char buf[68];
    for (size_t i = 0; i < records.size(); ++i)
    {
        size_t n = EncodeRecordDigestBytes(records[i], buf);
        SHA256_Update(&ctx, buf, n);
    }
    for (size_t i = 0; i < derived.size(); ++i)
    {
        size_t n = EncodeDerivedDigestBytes(derived[i], buf);
        SHA256_Update(&ctx, buf, n);
    }
    for (size_t i = 0; i < activeIds.size(); ++i)
    {
        EncodeActiveDigestBytes(activeIds[i], buf);
        SHA256_Update(&ctx, buf, 8);
    }
    SHA256_Update(&ctx, &meta.activeFence, 1);
    SHA256_Final(digest, &ctx);
}

// ---- v3 chained append accumulator (Repair #2) ----------------------------
static const char BLOCK_INDEX_TIP_V3_DOMAIN_R[] = "INNOVA-TIP-DIGEST-V3/R";
static const char BLOCK_INDEX_TIP_V3_DOMAIN_D[] = "INNOVA-TIP-DIGEST-V3/D";
static const char BLOCK_INDEX_TIP_V3_DOMAIN_A[] = "INNOVA-TIP-DIGEST-V3/A";

static void DigestChainInit(unsigned char out[32], const char* domain)
{
    SHA256_CTX c;
    SHA256_Init(&c);
    SHA256_Update(&c, domain, strlen(domain));
    SHA256_Final(out, &c);
}

static void DigestChainExtend(unsigned char state[32], const unsigned char* data, size_t n)
{
    SHA256_CTX c;
    SHA256_Init(&c);
    SHA256_Update(&c, state, 32);
    SHA256_Update(&c, data, n);
    SHA256_Final(state, &c);   // chain := SHA256(prev || element bytes)
}

static void DigestV3Finalize(const unsigned char R[32], const unsigned char D[32],
                             const unsigned char A[32], uint8_t fence, unsigned char out[32])
{
    SHA256_CTX c;
    SHA256_Init(&c);
    SHA256_Update(&c, R, 32);
    SHA256_Update(&c, D, 32);
    SHA256_Update(&c, A, 32);
    SHA256_Update(&c, &fence, 1);
    SHA256_Final(out, &c);
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

// ---- Repair #3: streaming CoW rewrite helpers -------------------------------
// Copy the retained PREFIX of a fixed-record store into a temp file (with
// header) and fsync it, so a reorg/truncate publishes the retained prefix by
// tmp+rename WITHOUT materializing the store in RAM (O(N) I/O, O(1) RAM).
// The source stores already hold the current COMMITTED bytes; a final entry
// override (the reorg's appended branch records, in id order) is emitted via
// the existing Encode* codec entries at matching slots.
static bool SetupStoreHeader(std::vector<unsigned char>* hdr, uint32_t which,
                             uint64_t count)
{
    hdr->clear();
    if (which == 0) // records
    {
        WriteRawLE32(*hdr, BLOCK_INDEX_FORMAT_VERSION);
        WriteRawLE32(*hdr, BLOCK_INDEX_RECORD_VERSION);
        WriteRawLE32(*hdr, BLOCK_INDEX_TIP_RECORDS_HEADER_SIZE);
        WriteRawLE32(*hdr, BLOCK_INDEX_RECORD_SIZE_V1);
        WriteRawLE64(*hdr, count);
        WriteRawLE64(*hdr, 0); WriteRawLE64(*hdr, 0); WriteRawLE64(*hdr, 0);
        return hdr->size() == BLOCK_INDEX_TIP_RECORDS_HEADER_SIZE;
    }
    if (which == 1) // derived
    {
        WriteRawLE32(*hdr, BLOCK_INDEX_DERIVED_FORMAT_VERSION);
        WriteRawLE32(*hdr, BLOCK_INDEX_DERIVED_SCHEMA_VERSION);
        WriteRawLE32(*hdr, BLOCK_INDEX_TIP_DERIVED_HEADER_SIZE);
        WriteRawLE32(*hdr, BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2);
        WriteRawLE64(*hdr, 0);
        WriteRawLE64(*hdr, count);
        for (int i = 0; i < 32; ++i) hdr->push_back(0);
        WriteRawLE64(*hdr, 0);
        return hdr->size() == BLOCK_INDEX_TIP_DERIVED_HEADER_SIZE;
    }
    // active
    WriteRawLE32(*hdr, BLOCK_INDEX_ACTIVE_SCHEMA_VERSION);
    WriteRawLE32(*hdr, BLOCK_INDEX_TIP_ACTIVE_HEADER_SIZE);
    WriteRawLE32(*hdr, BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1);
    WriteRawLE64(*hdr, 0); WriteRawLE64(*hdr, 0); WriteRawLE64(*hdr, 0); WriteRawLE64(*hdr, 0);
    return hdr->size() == BLOCK_INDEX_TIP_ACTIVE_HEADER_SIZE;
}

// Clamp a copy buffer to <= 64 KiB for the streaming helpers.
static inline size_t keepActiveBufSize(uint64_t entries, uint32_t entrySize)
{
    const uint64_t bytes = entries * entrySize;
    return (size_t)std::min<uint64_t>(bytes ? bytes : 1, 65536);
}

// Stream the retained active prefix [0, keepCount) of tip-active.dat into a
// CoW temp file (writes header + entries bytewise; O(count) I/O, O(1) RAM),
// leaving the temp DURABLE but unrenamed (the caller renames + dirsyncs).
static bool StreamActiveFileTo(const fs::path& activePath,
                               uint64_t keepCount, std::string* error)
{
    const fs::path tmp = fs::path(activePath.string() + ".tmp");
    FILE* f = fopen(tmp.string().c_str(), "wb");
    if (!f)
        return SetError(error, "open active tmp failed: " + tmp.string());
    std::vector<unsigned char> hdr;
    if (!SetupStoreHeader(&hdr, 2, keepCount) ||
        fwrite(&hdr[0], 1, hdr.size(), f) != hdr.size())
    {
        fclose(f);
        return SetError(error, "write active tmp header failed");
    }
    // Bytewise copy from the source file (header + keepCount entries).
    FILE* s = fopen(activePath.string().c_str(), "rb");
    if (!s)
    {
        fclose(f);
        return SetError(error, "open active source failed: " + activePath.string());
    }
    if (fseek(s, (long)BLOCK_INDEX_TIP_ACTIVE_HEADER_SIZE, SEEK_SET) != 0)
    {
        fclose(s); fclose(f);
        return SetError(error, "seek active source past header failed");
    }
    std::vector<char> buf(keepActiveBufSize(keepCount, BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1));
    const size_t want = (size_t)keepCount * BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1;
    size_t got = 0;
    while (got < want)
    {
        size_t chunk = std::min<size_t>(buf.size(), want - got);
        size_t r = fread(&buf[0], 1, chunk, s);
        if (r == 0)
            break;
        if (fwrite(&buf[0], 1, r, f) != r)
        {
            fclose(s); fclose(f);
            return SetError(error, "write active tmp copy failed");
        }
        got += r;
    }
    fclose(s);
    if (got != want)
    {
        fclose(f);
        return SetError(error, "active stream source short");
    }
    if (!FileCommitChecked(f, error))
    {
        fclose(f);
        return false;
    }
    if (fclose(f) != 0)
        return SetError(error, "close active tmp failed");
    return true;
}

// ---- Repair #3: hash-index marker sidecar (DERIVED, never authoritative) -----
// tip-hashindex.marker: magic(4) | version(4) | indexedCount(8). Crash-safe
// via the same WriteFileCoW discipline as the tail stores. It only ever
// Widens/Narrows the reconciliation delta; a corrupt/missing/absent marker
// simply forces a full rebuild of the index from the committed records.
static bool WriteHashIndexMarker(const fs::path& markerPath, uint64_t count,
                                 std::string* error)
{
    std::vector<unsigned char> b;
    WriteRawLE32(b, BLOCK_INDEX_TIP_HASHINDEX_MARKER_MAGIC);
    WriteRawLE32(b, BLOCK_INDEX_TIP_HASHINDEX_MARKER_VERSION);
    WriteRawLE64(b, count);
    return WriteFileCoW(markerPath, b, error);
}

static bool ReadHashIndexMarker(const fs::path& markerPath, uint64_t* countOut,
                                bool* present)
{
    *present = false;
    boost::system::error_code ec;
    if (!fs::exists(markerPath, ec))
        return true; // absent marker: rebuild as a FULL rebuild
    std::string data;
    if (!ReadWholeFile(markerPath, &data))
        return true; // unreadable: full rebuild
    if (data.size() < BLOCK_INDEX_TIP_HASHINDEX_MARKER_SIZE)
        return true; // torn/stale marker: full rebuild
    const unsigned char* p = (const unsigned char*)data.data();
    if (ReadRawLE32(p + 0) != BLOCK_INDEX_TIP_HASHINDEX_MARKER_MAGIC)
        return true;
    if (ReadRawLE32(p + 4) != BLOCK_INDEX_TIP_HASHINDEX_MARKER_VERSION)
        return true;
    *countOut = ReadRawLE64(p + 8);
    *present = true;
    return true;
}

// ---- Repair #3: reorg store streamer (RewriteStoresForReorg is defined
// after the Impl definition so it can reach Impl members directly):
// publishes the reorg-resulting stores WITHOUT materializing them in RAM --
// records/derived: committed prefix copied BYTEWISE + branch appends encoded
// at the end; active: the reorg-resulting dense active ids. Each store goes
// through a complete tmp -> fsync -> rename -> dirsync CoW publication (same
// R3 discipline; FP_DURING_TAIL_UPDATE is honoured at the same boundary) and
// tip.meta is written LAST by the caller (the single commit point).
// ---- Repair #3: TipSideIndex — LevelDB hash->RecordId side index -----------
// DERIVED STATE ONLY: tip.meta remains the single durable commit point.
// Open reconciles it against the committed authoritative stores:
//   tip.meta.tipRecordCount N + first N records of tip-records.dat define the
//   truth: the index must map exactly record[slot].hash -> id for slots
//   [0,N). Missing / partially-written / stale / extra entries are REBUILT
//   (index truncated back to the committed count, then repaired forward).
//   Inconsistency NEVER changes the authoritative chain and NEVER silently
//   corrupts validation result.
class TipSideIndex
{
public:
    TipSideIndex() {}
    ~TipSideIndex() { Close(); }

    // Open (or first-time create) the LevelDB gem under <tipDir>/"hashindex"
    // (the BlockIndexHashIndex primitive fixes its own directory name; the
    // tip namespace is what distinguishes it from other generations).
    bool OpenOrCreate(const fs::path& tipDir, std::string* error)
    {
        Close();
        tipDir_ = tipDir;
        dirPath_ = tipDir / BLOCK_INDEX_HASHINDEX_DIR_NAME; // wrapper-fixed name
        boost::system::error_code ec;
        index_ = BlockIndexHashIndex(); // start from a clean state object
        if (!fs::exists(dirPath_, ec))
        {
            BlockIndexHashIndex fresh;
            const uint64_t nonce = (uint64_t)getpid() * 2654435761ULL + 0x9E37;
            std::string cerr;
            if (!BlockIndexHashIndex::Create(tipDir_.string(), nonce, &fresh, &cerr))
                return SetError(error, "tip-hashindex create failed: " + cerr);
            index_ = std::move(fresh);
            return true;
        }
        {
            BlockIndexHashIndex opened;
            std::string oerr;
            if (!BlockIndexHashIndex::Open(tipDir_.string(), 0, &opened, &oerr))
                return SetError(error, "tip-hashindex open failed: " + oerr);
            index_ = std::move(opened);
        }
        return true;
    }

    // Destroy the LevelDB dir on disk so a later OpenOrCreate recreates it.
    bool DestroyDir(std::string* error)
    {
        Close();
        boost::system::error_code ec;
        fs::remove_all(dirPath_, ec);
        if (ec)
            return SetError(error, "remove stale tip-hashindex failed: " + ec.message());
        boost::system::error_code ec2;
        fs::remove(markerPath(), ec2);
        return true;
    }

    void Close() { index_.Close(); }
    bool IsOpen() const { return index_.IsOpen(); }

    // Writable open for the REPAIR path: the plain Open() path above yields a
    // logically read-only handle (BlockIndexHashIndex::Open enforces read-only),
    // but reconciliation (forward repair / full rebuild) must be able to Put.
    // Opens create-if-missing with a writable LevelDB dir. The open generation
    // is NOT validated (wildcard) -- the reconcile logic below IS the check.
    bool OpenOrCreateWritable(const fs::path& tipDir, std::string* error)
    {
        Close();
        tipDir_ = tipDir;
        dirPath_ = tipDir / BLOCK_INDEX_HASHINDEX_DIR_NAME;
        boost::system::error_code ec;
        BlockIndexHashIndex idx;
        const bool create = !fs::exists(dirPath_, ec);
        const uint64_t nonce = (uint64_t)getpid() * 2654435761ULL + 0x9E37;
        std::string werr;
        if (!idx.OpenInternal(tipDir_.string(), create ? nonce : 0, create, false, &werr))
            return SetError(error, "tip-hashindex writable open failed: " + werr);
        index_ = std::move(idx);
        return true;
    }

    bool Put(const uint256& hash, BlockIndexId id, std::string* error)
    {
        return index_.Put(hash, id, error);
    }

    // Drop an UNCOMMITTED residue entry placed by an aborted commit: delete
    // only when the stored id is exactly `expectedId` (never attacker- or
    // crash-ambiguous). Best-effort: errors are reported through `error` but
    // the caller treats residue as absent (bounded count check) anyway.
    bool DeleteIfCommitted(const uint256& hash, BlockIndexId expectedId,
                           std::string* error)
    {
        if (!IsOpen())
            return SetError(error, "tip-hashindex not open");
        return index_.Delete(hash, expectedId, error);
    }
    BlockIndexHashLookupStatus Lookup(const uint256& hash, BlockIndexId* outId,
                                      std::string* error) const
    {
        return index_.Lookup(hash, outId, error);
    }

    fs::path markerPath() const { return tipDir_ / BLOCK_INDEX_TIP_HASHINDEX_MARKER_FILE; }

private:
    BlockIndexHashIndex index_;
    fs::path tipDir_;
    fs::path dirPath_;
};

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
    fs::path hashIndexPath;
    fs::path hashIndexMarkerPath;

    BlockIndexTipMeta meta;

    // Repair #3: bounded positional windows over the three fixed-record
    // stores + the durable hash->RecordId LevelDB side index (all bounded).
    TipStoreRecordsWindow recordsWin;
    TipStoreDerivedWindow derivedWin;
    TipStoreActiveWindow activeWin;
    TipSideIndex sideIndex;

    // v2 durable operator-invalid authority (by-value; no CBlockIndex).
    std::vector<BlockIndexTipInvalidEntry> invalidEntries; // committed log prefix
    std::set<uint256> invalidSet;                          // derived set

    bool open;

    // Repair #2: v3 chained digest state. RAM-ONLY, never persisted, never an
    // independent authority -- rebuilt at Open and on any non-append mutation.
    unsigned char chainR[32];
    unsigned char chainD[32];
    unsigned char chainA[32];
    bool chainsValid;

    Impl()
        : recordsWin(fs::path(), BLOCK_INDEX_TIP_RECORDS_HEADER_SIZE,
                     BLOCK_INDEX_RECORD_SIZE_V1, BLOCK_INDEX_TIP_RING_CAP_ENTRIES,
                     BLOCK_INDEX_TIP_LRU_BYTES_CAP, TipDecodeRecord),
          derivedWin(fs::path(), BLOCK_INDEX_TIP_DERIVED_HEADER_SIZE,
                     BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2,
                     BLOCK_INDEX_TIP_RING_CAP_ENTRIES,
                     BLOCK_INDEX_TIP_LRU_BYTES_CAP, TipDecodeDerived),
          activeWin(fs::path(), BLOCK_INDEX_TIP_ACTIVE_HEADER_SIZE,
                    BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1,
                    BLOCK_INDEX_TIP_RING_CAP_ENTRIES,
                    BLOCK_INDEX_TIP_LRU_BYTES_CAP, TipDecodeActive),
          open(false), chainsValid(false) {}

    // Rebuild the three index-order chains from a committed vector set.
    void BuildChainsFor(const std::vector<BlockIndexRecord>& recs,
                        const std::vector<BlockIndexDerivedEntry>& ders,
                        const std::vector<BlockIndexId>& acts)
    {
        unsigned char b[68];
        DigestChainInit(chainR, BLOCK_INDEX_TIP_V3_DOMAIN_R);
        for (size_t j = 0; j < recs.size(); ++j)
        { size_t n = EncodeRecordDigestBytes(recs[j], b); DigestChainExtend(chainR, b, n); }
        DigestChainInit(chainD, BLOCK_INDEX_TIP_V3_DOMAIN_D);
        for (size_t j = 0; j < ders.size(); ++j)
        { size_t n = EncodeDerivedDigestBytes(ders[j], b); DigestChainExtend(chainD, b, n); }
        DigestChainInit(chainA, BLOCK_INDEX_TIP_V3_DOMAIN_A);
        for (size_t j = 0; j < acts.size(); ++j)
        { EncodeActiveDigestBytes(acts[j], b); DigestChainExtend(chainA, b, 8); }
        chainsValid = true;
    }
    void EnsureChains()
    {
        if (!chainsValid)
            BuildChainsForStreaming();
    }

    // Repair #3: reconcile the durable tip-hashindex against the committed
    // truth at Open. Truth = slots [0, tipRecordCount) of the committed
    // records store; expected id for slot s = baseRecordCount + s + 1.
    // Marker (sidecar file) records how many entries are proven indexed.
    // Crash cases handled:
    //   (a) records appended + index not updated   -> marker < N: repair fwd
    //   (b) index written + tip.meta not committed -> N < marker: truncate
    //       (rebuild from 0 handles stale entries above/below)
    //   (c) tip.meta committed + index update short -> marker/lookup mismatch
    //   (d) partial index write -> lookup mismatch at some slot
    //   (e) stale entries after truncation -> N < marker: rebuild
    //   (f) restart after interrupted reorg -> same checks (counts + spot)
    // (all cases converge to Repair #3 fix2: a FULL content verification of
    // the [0,N) committed range against the bounded records window — O(N)
    // time, O(1) auxiliary RAM, the same cost class as the mandatory
    // streaming digest validation that Open already pays. Anything the
    // verification cannot accept is deterministically rebuilt.)
    bool ReconcileSideIndexOnOpen(std::string* error)
    {
        const uint64_t N = meta.tipRecordCount;
        // Open WRITABLE from the start: only this authority owns the dir at
        // Open time, and reconciliation (forward repair / rebuild / stale
        // truncation) must be able to Put. The plain Open() wrapper is
        // logically read-only by contract and cannot serve the repair path.
        std::string idxerr;
        if (!sideIndex.OpenOrCreateWritable(tipDir, &idxerr))
            return SetError(error, "tip-hashindex open failed: " + idxerr);
        uint64_t marker = 0;
        bool markerPresent = false;
        ReadHashIndexMarker(hashIndexMarkerPath, &marker, &markerPresent);
        const bool countsAgree = markerPresent && marker == N;
        bool needFullRebuild = !countsAgree;
        // Repair #3 fix2: a marker==N count alone does not prove the index
        // CONTENT. A torn/partial publication or a LevelDB-store defect can
        // leave a middle hash MISSING or pointing at a WRONG RecordId while
        // the count and BOTH boundary probes stay perfect — and the derived
        // index would then silently return a wrong historical result (a
        // LookupByHash miss reads as NOT_FOUND; a wrong id reads as a
        // different block). Truncate/forward-repair cannot detect that
        // deterministically, so reconcile verifies EVERY committed entry:
        // slot-by-slot against the bounded records window (O(N) time, O(1)
        // auxiliary RAM — the same cost class as the mandatory streaming
        // digest validation that Open already pays). ANY mismatch (missing /
        // wrong id) forces the deterministic full rebuild from the committed
        // stores. The derived index stays exactly derived: tip.meta is
        // untouched, not read, and never overridden by index content.
        if (!needFullRebuild && N > 0)
        {
            std::string verr;
            bool midOk = true;
            const bool verified = recordsWin.StreamVisit(0, N,
                [this, &midOk](uint64_t slot, const BlockIndexRecord& r) {
                    BlockIndexId got = 0;
                    if (sideIndex.Lookup(r.hash, &got, NULL) !=
                            BLOCK_INDEX_HASH_LOOKUP_FOUND ||
                        got != baseLocalToId(slot))
                    {
                        midOk = false;
                        return false; // stop the stream at the first mismatch
                    }
                    return true;
                }, &verr);
            if (!verified && midOk)
                return SetError(error, "tip-hashindex verify stream: " + verr);
            if (!midOk)
                needFullRebuild = true;
        }
        if (needFullRebuild)
        {
            // Deterministically rebuild from the committed records store.
            std::string rerr;
            if (!sideIndex.DestroyDir(&rerr))
                return SetError(error, "tip-hashindex rebuild reset: " + rerr);
            std::string oerr;
            if (!sideIndex.OpenOrCreateWritable(tipDir, &oerr))
                return SetError(error, "tip-hashindex recreate: " + oerr);
            std::string perr;
            const bool built = recordsWin.StreamVisit(0, N,
                [this](uint64_t slot, const BlockIndexRecord& r) {
                    BlockIndexId id = baseLocalToId(slot);
                    return sideIndex.Put(r.hash, id, NULL);
                }, &perr);
            if (!built)
                return SetError(error, "tip-hashindex rebuild: " + perr);
            std::string merr;
            if (!WriteHashIndexMarker(hashIndexMarkerPath, N, &merr))
                return SetError(error, "tip-hashindex marker write: " + merr);
        }
        return true;
    }

    // Repair #3: streaming rebuild of the three index-order chains straight
    // from the committed store files (O(N) time, O(1) auxiliary RAM; the
    // ring/window caches are untouched -- cache-neutral). Used by Open and by
    // any non-append mutation after its SetCount() calls (the stores already
    // reflect the resulting committed state at that point).
    bool BuildChainsForStreaming()
    {
        DigestChainInit(chainR, BLOCK_INDEX_TIP_V3_DOMAIN_R);
        DigestChainInit(chainD, BLOCK_INDEX_TIP_V3_DOMAIN_D);
        DigestChainInit(chainA, BLOCK_INDEX_TIP_V3_DOMAIN_A);
        std::string werr;
        lastStreamError.clear();
        unsigned char b[68];
        const uint64_t rc = recordsWin.Count();
        const uint64_t dc = derivedWin.Count();
        const uint64_t ac = activeWin.Count();
        bool rok = recordsWin.StreamVisit(0, rc,
            [&b, this](uint64_t, const BlockIndexRecord& r) {
                size_t n = EncodeRecordDigestBytes(r, b);
                DigestChainExtend(chainR, b, n);
                return true;
            }, &werr);
        bool dok = derivedWin.StreamVisit(0, dc,
            [&b, this](uint64_t, const BlockIndexDerivedEntry& d) {
                size_t n = EncodeDerivedDigestBytes(d, b);
                DigestChainExtend(chainD, b, n);
                return true;
            }, &werr);
        bool aok = activeWin.StreamVisit(0, ac,
            [&b, this](uint64_t, const BlockIndexId& id) {
                EncodeActiveDigestBytes(id, b);
                DigestChainExtend(chainA, b, 8);
                return true;
            }, &werr);
        chainsValid = true;
        if (!rok) lastStreamError = "records stream: " + werr;
        else if (!dok) lastStreamError = "derived stream: " + werr;
        else if (!aok) lastStreamError = "active stream: " + werr;
        return rok && dok && aok;
    }
    std::string lastStreamError;

    // Repair #3: helper accessors over the bounded windows. Every read is
    // by-value; a miss streams ONE positional pread from the store file.
    bool GetRecordBySlot(uint64_t slot, BlockIndexRecord* out, std::string* err) const
    { return recordsWin.Get(slot, out, err); }
    bool GetDerivedBySlot(uint64_t slot, BlockIndexDerivedEntry* out, std::string* err) const
    { return derivedWin.Get(slot, out, err); }
    bool GetActiveByRel(uint64_t rel, BlockIndexId* out, std::string* err) const
    { return activeWin.Get(rel, out, err); }

    bool InitializePaths(const std::string& rootIn)
    {
        root = fs::path(rootIn);
        tipDir = root / BLOCK_INDEX_TIP_DIR_NAME;
        metaPath = tipDir / BLOCK_INDEX_TIP_META_FILE;
        recordsPath = tipDir / BLOCK_INDEX_TIP_RECORDS_FILE;
        activePath = tipDir / BLOCK_INDEX_TIP_ACTIVE_FILE;
        derivedPath = tipDir / BLOCK_INDEX_TIP_DERIVED_FILE;
        invalidPath = tipDir / BLOCK_INDEX_TIP_INVALID_FILE;
        hashIndexPath = tipDir / BLOCK_INDEX_TIP_HASHINDEX_DIR_NAME;
        hashIndexMarkerPath = tipDir / BLOCK_INDEX_TIP_HASHINDEX_MARKER_FILE;
        // Re-point the positional windows at the (possibly new) store paths.
        ResetWindowPaths();
        return true;
    }

    // Re-construct the three bounded windows with the CURRENT store paths
    // (paths are fixed at construction in TipStoreWindow, so InitializePaths
    // swaps them via a full rebuild -- caps/decoders are identical).
    void ResetWindowPaths()
    {
        recordsWin = TipStoreRecordsWindow(recordsPath,
            BLOCK_INDEX_TIP_RECORDS_HEADER_SIZE, BLOCK_INDEX_RECORD_SIZE_V1,
            BLOCK_INDEX_TIP_RING_CAP_ENTRIES, BLOCK_INDEX_TIP_LRU_BYTES_CAP,
            TipDecodeRecord);
        derivedWin = TipStoreDerivedWindow(derivedPath,
            BLOCK_INDEX_TIP_DERIVED_HEADER_SIZE, BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2,
            BLOCK_INDEX_TIP_RING_CAP_ENTRIES, BLOCK_INDEX_TIP_LRU_BYTES_CAP,
            TipDecodeDerived);
        activeWin = TipStoreActiveWindow(activePath,
            BLOCK_INDEX_TIP_ACTIVE_HEADER_SIZE, BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1,
            BLOCK_INDEX_TIP_RING_CAP_ENTRIES, BLOCK_INDEX_TIP_LRU_BYTES_CAP,
            TipDecodeActive);
    }

    uint64_t baseLocalToId(size_t localIndex) const
    {
        // localIndex 0-based into records; RecordId = baseRecordCount + local + 1
        return meta.baseRecordCount + (uint64_t)localIndex + 1;
    }

    // Repair #3: stream the reorg-resulting stores (see definition after the
    // struct; keeps ReorgActiveTo/ApplyOperatorInvalidAndReorg O(new) RAM).
    bool RewriteStoresForReorg(uint64_t keepActive, uint64_t committedRecords,
                               const std::vector<BlockIndexRecord>& appendRecords,
                               const std::vector<BlockIndexDerivedEntry>& appendDerived,
                               const std::vector<BlockIndexId>& newActive,
                               std::string* error);

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
}; // struct Impl

// ---- Repair #3: reorg store streamer (Impl member) --------------------------
bool BlockIndexTipAuthority::Impl::RewriteStoresForReorg(
    uint64_t keepActive, uint64_t committedRecords,
    const std::vector<BlockIndexRecord>& appendRecords,
    const std::vector<BlockIndexDerivedEntry>& appendDerived,
    const std::vector<BlockIndexId>& newActive, std::string* error)
{
    // ---- records: committed prefix bytewise + branch appends encoded ----
    {
        const std::string tmp = recordsPath.string() + ".tmp";
        FILE* f = fopen(tmp.c_str(), "wb");
        if (!f) return SetError(error, "open reorg records tmp failed");
        std::vector<unsigned char> hdr;
        if (!SetupStoreHeader(&hdr, 0, committedRecords + appendRecords.size()) ||
            fwrite(&hdr[0], 1, hdr.size(), f) != hdr.size())
        { fclose(f); return SetError(error, "write reorg records hdr failed"); }
        FILE* s = fopen(recordsPath.string().c_str(), "rb");
        if (!s) { fclose(f); return SetError(error, "open reorg records src failed"); }
        if (fseek(s, (long)BLOCK_INDEX_TIP_RECORDS_HEADER_SIZE, SEEK_SET) != 0)
        { fclose(s); fclose(f); return SetError(error, "seek reorg records src failed"); }
        std::vector<char> buf(65536);
        const uint64_t want = committedRecords * BLOCK_INDEX_RECORD_SIZE_V1;
        uint64_t got = 0;
        while (got < want)
        {
            size_t chunk = (size_t)std::min<uint64_t>(want - got, buf.size());
            size_t r = fread(&buf[0], 1, chunk, s);
            if (r == 0) break;
            if (fwrite(&buf[0], 1, r, f) != r)
            { fclose(s); fclose(f); return SetError(error, "write reorg records copy failed"); }
            got += r;
        }
        fclose(s);
        if (got != want)
        { fclose(f); return SetError(error, "reorg records stream short"); }
        for (size_t k = 0; k < appendRecords.size(); ++k)
        {
            std::vector<unsigned char> enc;
            if (!EncodeBlockIndexRecordV1(appendRecords[k], &enc, NULL) ||
                fwrite(&enc[0], 1, enc.size(), f) != enc.size())
            { fclose(f); return SetError(error, "encode reorg records tail failed"); }
        }
        if (!FileCommitChecked(f, error)) { fclose(f); return false; }
        if (fclose(f) != 0) return SetError(error, "close reorg records tmp failed");
        if (BlockIndexTipFailpointHit("FP_DURING_TAIL_UPDATE"))
            return SetError(error, "failpoint FP_DURING_TAIL_UPDATE");
        if (!RenameOverChecked(fs::path(tmp), recordsPath, error)) return false;
        if (!SyncDirectoryChecked(tipDir, error)) return false;
        std::string cerr;
        if (!UpdateStoreCount(recordsPath, BLOCK_INDEX_TIP_RECORDS_COUNT_OFFSET,
                              committedRecords + appendRecords.size(), &cerr))
            return SetError(error, "reorg records count update failed: " + cerr);
    }
    // ---- derived: committed prefix bytewise + branch appends encoded ----
    {
        const std::string tmp = derivedPath.string() + ".tmp";
        FILE* f = fopen(tmp.c_str(), "wb");
        if (!f) return SetError(error, "open reorg derived tmp failed");
        std::vector<unsigned char> hdr;
        if (!SetupStoreHeader(&hdr, 1, committedRecords + appendDerived.size()) ||
            fwrite(&hdr[0], 1, hdr.size(), f) != hdr.size())
        { fclose(f); return SetError(error, "write reorg derived hdr failed"); }
        FILE* s = fopen(derivedPath.string().c_str(), "rb");
        if (!s) { fclose(f); return SetError(error, "open reorg derived src failed"); }
        if (fseek(s, (long)BLOCK_INDEX_TIP_DERIVED_HEADER_SIZE, SEEK_SET) != 0)
        { fclose(s); fclose(f); return SetError(error, "seek reorg derived src failed"); }
        std::vector<char> buf(65536);
        const uint64_t want = committedRecords * BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2;
        uint64_t got = 0;
        while (got < want)
        {
            size_t chunk = (size_t)std::min<uint64_t>(want - got, buf.size());
            size_t r = fread(&buf[0], 1, chunk, s);
            if (r == 0) break;
            if (fwrite(&buf[0], 1, r, f) != r)
            { fclose(s); fclose(f); return SetError(error, "write reorg derived copy failed"); }
            got += r;
        }
        fclose(s);
        if (got != want)
        { fclose(f); return SetError(error, "reorg derived stream short"); }
        for (size_t k = 0; k < appendDerived.size(); ++k)
        {
            std::vector<unsigned char> enc;
            if (!EncodeBlockIndexDerivedEntry(appendDerived[k], &enc, NULL) ||
                fwrite(&enc[0], 1, enc.size(), f) != enc.size())
            { fclose(f); return SetError(error, "encode reorg derived tail failed"); }
        }
        if (!FileCommitChecked(f, error)) { fclose(f); return false; }
        if (fclose(f) != 0) return SetError(error, "close reorg derived tmp failed");
        if (BlockIndexTipFailpointHit("FP_DURING_TAIL_UPDATE"))
            return SetError(error, "failpoint FP_DURING_TAIL_UPDATE");
        if (!RenameOverChecked(fs::path(tmp), derivedPath, error)) return false;
        if (!SyncDirectoryChecked(tipDir, error)) return false;
        std::string cerr;
        if (!UpdateStoreCount(derivedPath, BLOCK_INDEX_TIP_DERIVED_COUNT_OFFSET,
                              committedRecords + appendDerived.size(), &cerr))
            return SetError(error, "reorg derived count update failed: " + cerr);
    }
    // ---- active: dense result ids (fork prefix copied when byte-identical,
    //      branch ids encoded) ----
    {
        const std::string tmp = activePath.string() + ".tmp";
        FILE* f = fopen(tmp.c_str(), "wb");
        if (!f) return SetError(error, "open reorg active tmp failed");
        std::vector<unsigned char> hdr;
        if (!SetupStoreHeader(&hdr, 2, newActive.size()) ||
            fwrite(&hdr[0], 1, hdr.size(), f) != hdr.size())
        { fclose(f); return SetError(error, "write reorg active hdr failed"); }
        const bool prefixCopyable =
            keepActive > 0 && newActive.size() >= keepActive;
        if (prefixCopyable)
        {
            FILE* s = fopen(activePath.string().c_str(), "rb");
            if (!s) { fclose(f); return SetError(error, "open reorg active src failed"); }
            if (fseek(s, (long)BLOCK_INDEX_TIP_ACTIVE_HEADER_SIZE, SEEK_SET) != 0)
            { fclose(s); fclose(f); return SetError(error, "seek reorg active src failed"); }
            std::vector<char> abuf(65536);
            const uint64_t want = keepActive * BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1;
            uint64_t got = 0;
            while (got < want)
            {
                size_t chunk = (size_t)std::min<uint64_t>(want - got, abuf.size());
                size_t r = fread(&abuf[0], 1, chunk, s);
                if (r == 0) break;
                if (fwrite(&abuf[0], 1, r, f) != r)
                { fclose(s); fclose(f); return SetError(error, "write reorg active copy failed"); }
                got += r;
            }
            fclose(s);
            if (got != want)
            { fclose(f); return SetError(error, "reorg active stream short"); }
            for (size_t h = keepActive; h < newActive.size(); ++h)
            {
                std::string enc;
                if (!EncodeBlockIndexActiveEntry(newActive[h], &enc, NULL) ||
                    fwrite(enc.data(), 1, enc.size(), f) != enc.size())
                { fclose(f); return SetError(error, "encode reorg active tail failed"); }
            }
        }
        else
        {
            for (size_t h = 0; h < newActive.size(); ++h)
            {
                std::string enc;
                if (!EncodeBlockIndexActiveEntry(newActive[h], &enc, NULL) ||
                    fwrite(enc.data(), 1, enc.size(), f) != enc.size())
                { fclose(f); return SetError(error, "encode reorg active failed"); }
            }
        }
        if (!FileCommitChecked(f, error)) { fclose(f); return false; }
        if (fclose(f) != 0) return SetError(error, "close reorg active tmp failed");
        if (BlockIndexTipFailpointHit("FP_DURING_TAIL_UPDATE"))
            return SetError(error, "failpoint FP_DURING_TAIL_UPDATE");
        if (!RenameOverChecked(fs::path(tmp), activePath, error)) return false;
        if (!SyncDirectoryChecked(tipDir, error)) return false;
        std::string cerr;
        if (!UpdateStoreCount(activePath, BLOCK_INDEX_TIP_ACTIVE_COUNT_OFFSET,
                              newActive.size(), &cerr))
            return SetError(error, "reorg active count update failed: " + cerr);
    }
    return true;
}

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
    std::string sidxerr;
    unsigned char digestEmpty[32];
    // Persist the empty stores (headers only) as static empties.
    const std::vector<BlockIndexRecord> emptyRecords;
    const std::vector<BlockIndexDerivedEntry> emptyDerived;
    const std::vector<BlockIndexId> emptyActive;
    if (!WriteRecordsFile(i->recordsPath, emptyRecords))
        return SetError(error, "init tip-records failed");
    if (!WriteActiveFile(i->activePath, emptyActive))
        return SetError(error, "init tip-active failed");
    if (!WriteDerivedFile(i->derivedPath, emptyDerived))
        return SetError(error, "init tip-derived failed");
    // v2: create the (empty) operator-invalid log so the store set is complete.
    if (!WriteInvalidFile(i->invalidPath, i->invalidEntries))
        return SetError(error, "init tip-invalid failed");
    // Repair #3: create the (empty) hash-index side store + marker.
    i->sideIndex.Close();
    if (!i->sideIndex.OpenOrCreateWritable(i->tipDir, &sidxerr))
        return SetError(error, "init tip-hashindex failed: " + sidxerr);
    std::string errMarker;
    if (!WriteHashIndexMarker(i->hashIndexMarkerPath, 0, &errMarker))
        return SetError(error, "init tip-hashindex marker failed: " + errMarker);
    // initialize the bounded windows over the fresh empty stores.
    std::string werrA, werrB, werrC;
    if (!i->recordsWin.InitializeCount(0, &werrA) ||
        !i->derivedWin.InitializeCount(0, &werrB) ||
        !i->activeWin.InitializeCount(0, &werrC))
        return SetError(error, "init tip windows failed: " + werrA + werrB + werrC);
    // Compute the content digest over the (empty) committed region so a
    // subsequent Open's recomputation matches (it must not be all-zero).
    // The freshly created meta is v2, so seed its digest in the v2 (streaming)
    // flavour over the empty region -- identical to Open's v2 recompute path.
    {
        SHA256_CTX ctx;
        SHA256_Init(&ctx);
        SHA256_Update(&ctx, &i->meta.activeFence, 1);
        SHA256_Final(digestEmpty, &ctx);
        memcpy(i->meta.contentDigest, digestEmpty, 32);
    }
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

    // 2-5 (Repair #3): reconcile the committed stores WITHOUT materializing
    // them in RAM. File sizes give the PHYSICAL entry counts; any physical
    // suffix beyond tip.meta's committed counts is an uncommitted tail and is
    // physically truncated exactly as before (O(1) resize + header count
    // fix). A store SHORTER than its committed region fails closed.
    boost::system::error_code ecSize;
    const uintmax_t recBytes = fs::file_size(i->recordsPath, ecSize);
    if (ecSize) return SetError(error, "stat tip-records failed");
    const uintmax_t derBytes = fs::file_size(i->derivedPath, ecSize);
    if (ecSize) return SetError(error, "stat tip-derived failed");
    const uintmax_t actBytes = fs::file_size(i->activePath, ecSize);
    if (ecSize) return SetError(error, "stat tip-active failed");
    const uint64_t expectedActive = ((int64_t)meta.tipHeight >= (int64_t)meta.baseTipHeight)
        ? (uint64_t)((int64_t)meta.tipHeight - (int64_t)meta.baseTipHeight) : 0;
    if ((int64_t)meta.tipHeight < (int64_t)meta.baseTipHeight)
        return SetError(error, "tip.meta tipHeight below baseTipHeight (corrupt)");
    const uint64_t physRecords = (recBytes > BLOCK_INDEX_TIP_RECORDS_HEADER_SIZE)
        ? (uint64_t)(recBytes - BLOCK_INDEX_TIP_RECORDS_HEADER_SIZE) / BLOCK_INDEX_RECORD_SIZE_V1 : 0;
    const uint64_t physDerived = (derBytes > BLOCK_INDEX_TIP_DERIVED_HEADER_SIZE)
        ? (uint64_t)(derBytes - BLOCK_INDEX_TIP_DERIVED_HEADER_SIZE) / BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2 : 0;
    const uint64_t physActive = (actBytes > BLOCK_INDEX_TIP_ACTIVE_HEADER_SIZE)
        ? (uint64_t)(actBytes - BLOCK_INDEX_TIP_ACTIVE_HEADER_SIZE) / BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1 : 0;
    bool repaired = false;
    if (physRecords > meta.tipRecordCount || physDerived > meta.tipRecordCount ||
        physActive > expectedActive)
        repaired = true;
    if (physRecords < meta.tipRecordCount || physDerived < meta.tipRecordCount ||
        physActive < expectedActive)
        return SetError(error, "tip stores short of committed tip.meta (corrupt)");

    if (repaired)
    {
        if (!TruncateStoreToCommitted(i->recordsPath, BLOCK_INDEX_TIP_RECORDS_HEADER_SIZE,
                                      BLOCK_INDEX_RECORD_SIZE_V1, meta.tipRecordCount, error) ||
            !TruncateStoreToCommitted(i->derivedPath, BLOCK_INDEX_TIP_DERIVED_HEADER_SIZE,
                                      BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2, meta.tipRecordCount, error) ||
            !TruncateStoreToCommitted(i->activePath, BLOCK_INDEX_TIP_ACTIVE_HEADER_SIZE,
                                      BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1, expectedActive, error))
            return false;
        // Keep the informational header counts consistent with the truncated files.
        std::string cerr;
        if (!UpdateStoreCount(i->recordsPath, BLOCK_INDEX_TIP_RECORDS_COUNT_OFFSET, meta.tipRecordCount, &cerr) ||
            !UpdateStoreCount(i->derivedPath, BLOCK_INDEX_TIP_DERIVED_COUNT_OFFSET, meta.tipRecordCount, &cerr) ||
            !UpdateStoreCount(i->activePath, BLOCK_INDEX_TIP_ACTIVE_COUNT_OFFSET, expectedActive, &cerr))
            return SetError(error, "tip header count repair failed: " + cerr);
    }

    // Initialize the bounded windows over the committed region (ring warmed
    // with the newest min(cap, count) entries) + open the read handle.
    std::string werrW;
    if (!i->recordsWin.InitializeCount(meta.tipRecordCount, &werrW))
        return SetError(error, "tip records window init failed: " + werrW);
    if (!i->derivedWin.InitializeCount(meta.tipRecordCount, &werrW))
        return SetError(error, "tip derived window init failed: " + werrW);
    if (!i->activeWin.InitializeCount(expectedActive, &werrW))
        return SetError(error, "tip active window init failed: " + werrW);

    // 6 (Repair #3): reconcile the durable hash-index side store against the
    // committed truth (tipRecordCount records). NEVER authoritative, NEVER a
    // second commit point: any inconsistency is repaired here, before the
    // authority is served.
    if (!i->ReconcileSideIndexOnOpen(&werrW))
        return SetError(error, "tip hash-index reconcile failed: " + werrW);

    // 7. Validate content digest. v3 metas use the chained accumulator; v1/v2
    //    metas use the UNCHANGED streaming digest. Both run STREAMING from the
    //    committed stores (O(N) time, O(1) auxiliary RAM).
    const bool metaIsV3 = (meta.version == BLOCK_INDEX_TIP_META_VERSION_V3);
    unsigned char digest[32];
    if (!i->BuildChainsForStreaming())
        return SetError(error, "tip store streaming scan failed (corrupt short store)");
    if (metaIsV3)
    {
        DigestV3Finalize(i->chainR, i->chainD, i->chainA, meta.activeFence, digest);
    }
    else
    {
        // v2 streaming digest over the SAME committed region (identical byte
        // emissions to the original inline computation).
        SHA256_CTX ctx;
        SHA256_Init(&ctx);
        unsigned char buf[68];
        const uint64_t nR = meta.tipRecordCount, nD = meta.tipRecordCount,
                       nA = expectedActive;
        {
            BlockIndexRecord rr;
            for (uint64_t s = 0; s < nR; ++s)
            {
                std::string gerr;
                if (!i->recordsWin.StreamVisit(s, s + 1,
                        [&rr, &buf, &ctx](uint64_t, const BlockIndexRecord& r) {
                            size_t n = EncodeRecordDigestBytes(r, buf);
                            SHA256_Update(&ctx, buf, n);
                            return true;
                        }, &gerr))
                    return SetError(error, "tip-records streaming digest failed");
            }
            BlockIndexDerivedEntry dd;
            for (uint64_t s = 0; s < nD; ++s)
            {
                std::string gerr;
                if (!i->derivedWin.StreamVisit(s, s + 1,
                        [&dd, &buf, &ctx](uint64_t, const BlockIndexDerivedEntry& d) {
                            size_t n = EncodeDerivedDigestBytes(d, buf);
                            SHA256_Update(&ctx, buf, n);
                            return true;
                        }, &gerr))
                    return SetError(error, "tip-derived streaming digest failed");
            }
            BlockIndexId aa;
            for (uint64_t s = 0; s < nA; ++s)
            {
                std::string gerr;
                if (!i->activeWin.StreamVisit(s, s + 1,
                        [&aa, &buf, &ctx](uint64_t, const BlockIndexId& id) {
                            EncodeActiveDigestBytes(id, buf);
                            SHA256_Update(&ctx, buf, 8);
                            return true;
                        }, &gerr))
                    return SetError(error, "tip-active streaming digest failed");
            }
        }
        SHA256_Update(&ctx, &meta.activeFence, 1);
        SHA256_Final(digest, &ctx);
    }
    if (memcmp(digest, meta.contentDigest, 32) != 0)
    {
        if (!repaired)
            return SetError(error, "tip store content digest mismatch (corrupt)");
        // repaired an uncommitted tail: recompute the digest and re-publish
        // tip.meta so the committed state stays consistent. The authoritative
        // bytes are ALREADY exactly the committed region (the tail was
        // physically truncated above), so only meta's digest field changes.
        unsigned char newDigest[32];
        memcpy(newDigest, digest, 32);
        i->meta.contentDigest[0] = 0; // marker: publish below via a v3 write
        // NOTE (Repair #3): the earlier full re-write of the stores here was
        // byte-identical to what is already committed (they decoded to the
        // same vectors and re-encoded through the same codecs); the O(N)
        // rewrite is unnecessary. Only tip.meta is re-published.
        BlockIndexTipMeta fixedMeta = meta;
        fixedMeta.version = BLOCK_INDEX_TIP_META_VERSION_V3;
        memcpy(fixedMeta.contentDigest, newDigest, 32);
        i->meta = fixedMeta;
        if (!i->WriteMeta(error))
        {
            i->meta = meta;
            return SetError(error, "repair tip.meta re-publish failed");
        }
        i->meta = fixedMeta;
    }

    // For a non-v3 meta, the RAM chains are now built above (streaming); they
    // stay valid for the first O(new) append extension.
    i->chainsValid = true;

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
        // Idempotent replay check via the durable side index (O(log N) LevelDB
        // lookup, never O(N)); the index answers for the COMMITTED tip
        // namespace, which is the only namespace Append may skip on.
        {
            BlockIndexId existing = 0;
            if (i->sideIndex.Lookup(blocks[k].record.hash, &existing, NULL) ==
                    BLOCK_INDEX_HASH_LOOKUP_FOUND)
                continue; // idempotent replay
        }
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
            newActive.push_back(i->baseLocalToId(i->meta.tipRecordCount +
                                                 newRecords.size() - 1));
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

    const uint64_t committedRecords = i->meta.tipRecordCount;
    const uint64_t committedDerived = i->meta.tipRecordCount;
    const uint64_t committedActive = (uint64_t)(i->meta.tipHeight > i->meta.baseTipHeight)
        ? (uint64_t)(i->meta.tipHeight - i->meta.baseTipHeight) : 0;
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
    // Repair #3: durable side index Put for each new record. The entries are
    // an UNCOMMITTED suffix of the index until tip.meta advances (the same
    // fail-safe tier as the tail stores); Open reconciles index entries past
    // the committed count.
    for (size_t k = 0; k < newRecords.size(); ++k)
    {
        std::string ierr;
        if (!i->sideIndex.Put(newRecords[k].hash,
                              i->baseLocalToId(committedRecords + k), &ierr))
            return SetError(error, "append tip-hashindex put failed: " + ierr),
                   BLOCK_INDEX_TIP_IO_ERROR;
    }
    {
        std::string merr;
        if (!WriteHashIndexMarker(i->hashIndexMarkerPath,
                                  committedRecords + newRecords.size(), &merr))
            return SetError(error, "append tip-hashindex marker failed: " + merr),
                   BLOCK_INDEX_TIP_IO_ERROR;
    }

    // Test-only: all tail stores durable, crash before the tip.meta commit.
    if (BlockIndexTipFailpointHit("FP_AFTER_TAIL_DURABLE_BEFORE_META"))
    {
        // The side-index entries published above are an UNCOMMITTED delta;
        // the in-session index must not answer for them (they are also
        // reconciled away at the next Open).
        for (size_t k = 0; k < newRecords.size(); ++k)
        {
            std::string derr;
            i->sideIndex.DeleteIfCommitted(newRecords[k].hash,
                                           i->baseLocalToId(committedRecords + k),
                                           &derr);
        }
        return SetError(error, "failpoint FP_AFTER_TAIL_DURABLE_BEFORE_META"), BLOCK_INDEX_TIP_IO_ERROR;
    }

    // 2. Advance tip.meta.
    BlockIndexTipMeta newMeta = i->meta;
    newMeta.version = BLOCK_INDEX_TIP_META_VERSION_V3; // upgrade to v3 on write
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

    // 3. Commit: advance the bounded windows INCREMENTALLY (O(new)), extend
    //    the v3 digest chains by ONLY the new entries (O(new)), then publish
    //    tip.meta. A failed commit rolls the windows AND the chains back so
    //    authority never runs ahead of the last committed tip.meta; the side
    //    index entries published above stay (they are reconciled at Open).
    i->EnsureChains();
    const BlockIndexTipMeta savedMeta = i->meta;
    unsigned char savedR[32], savedD[32], savedA[32];
    memcpy(savedR, i->chainR, 32); memcpy(savedD, i->chainD, 32); memcpy(savedA, i->chainA, 32);
    const uint64_t savedRecordsCount = i->recordsWin.Count();
    const uint64_t savedDerivedCount = i->derivedWin.Count();
    const uint64_t savedActiveCount = i->activeWin.Count();
    i->recordsWin.PushBack(&newRecords[0], newRecords.size());
    i->derivedWin.PushBack(&newDerived[0], newDerived.size());
    if (!newActive.empty())
        i->activeWin.PushBack(&newActive[0], newActive.size());
    {
        unsigned char b[68];
        for (size_t k = 0; k < newRecords.size(); ++k)
        { size_t n = EncodeRecordDigestBytes(newRecords[k], b); DigestChainExtend(i->chainR, b, n); }
        for (size_t k = 0; k < newDerived.size(); ++k)
        { size_t n = EncodeDerivedDigestBytes(newDerived[k], b); DigestChainExtend(i->chainD, b, n); }
        for (size_t k = 0; k < newActive.size(); ++k)
        { EncodeActiveDigestBytes(newActive[k], b); DigestChainExtend(i->chainA, b, 8); }
        DigestV3Finalize(i->chainR, i->chainD, i->chainA, newMeta.activeFence, newMeta.contentDigest);
    }
    i->meta = newMeta;
    if (!i->WriteMeta(error))
    {
        // Roll the windows back to the committed counts (ring suffix shrink).
        while (i->recordsWin.Count() > savedRecordsCount) i->recordsWin.Pop();
        while (i->derivedWin.Count() > savedDerivedCount) i->derivedWin.Pop();
        while (i->activeWin.Count() > savedActiveCount) i->activeWin.Pop();
        i->meta = savedMeta;
        memcpy(i->chainR, savedR, 32); memcpy(i->chainD, savedD, 32); memcpy(i->chainA, savedA, 32);
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
    std::string werr;
    // height is a GLOBAL active height. The empty tip == baseTipHeight (S).
    const int32_t baseTip = i->meta.baseTipHeight;
    const uint64_t activeCount = (uint64_t)(i->meta.tipHeight > baseTip)
        ? (uint64_t)(i->meta.tipHeight - baseTip) : 0;
    if (height < baseTip || height >= baseTip + (int32_t)activeCount)
        return SetError(error, "truncate height out of range"), BLOCK_INDEX_TIP_CORRUPT;

    // relative active count after truncate = height - baseTip. The retained
    // active prefix is STREAMED straight into the temp file (O(count), O(1)
    // auxiliary RAM) and atomically renamed over the store (CoW commit).
    const uint64_t keepActive = (uint64_t)(height - baseTip);
    {
        const fs::path tmp = fs::path(i->activePath.string() + ".tmp");
        if (!StreamActiveFileTo(i->activePath, keepActive, &werr))
            return SetError(error, "truncate tip-active stream failed: " + werr),
                   BLOCK_INDEX_TIP_IO_ERROR;
        // CoW boundary (test-only crash point), mirroring the records path:
        // the complete temp file is durable but NOT yet renamed over.
        if (BlockIndexTipFailpointHit("FP_DURING_TAIL_UPDATE"))
            return SetError(error, "failpoint FP_DURING_TAIL_UPDATE"),
                   BLOCK_INDEX_TIP_IO_ERROR;
        if (!RenameOverChecked(tmp, i->activePath, error) ||
            !SyncDirectoryChecked(i->tipDir, error))
            return SetError(error, "truncate tip-active publish failed"), BLOCK_INDEX_TIP_IO_ERROR;
        std::string cerr;
        if (!UpdateStoreCount(i->activePath, BLOCK_INDEX_TIP_ACTIVE_COUNT_OFFSET,
                              keepActive, &cerr))
            return SetError(error, "truncate tip-active count update failed: " + cerr),
                   BLOCK_INDEX_TIP_IO_ERROR;
    }

    // Resolve the new tip hash by ONE positional read of the kept tip record
    // (slot = tipId - baseRecordCount - 1) -- no O(records) scan.
    BlockIndexTipMeta newMeta = i->meta;
    newMeta.version = BLOCK_INDEX_TIP_META_VERSION_V3;
    newMeta.activeFence++;
    newMeta.tipHeight = height;
    if (keepActive == 0)
        newMeta.tipHash = uint256(0);
    else
    {
        BlockIndexId tipId = 0;
        std::string gerr;
        if (!i->activeWin.Get(keepActive - 1, &tipId, &gerr))
            return SetError(error, "truncate tip hash resolve failed: " + gerr),
                   BLOCK_INDEX_TIP_CORRUPT;
        if (tipId <= i->meta.baseRecordCount)
            return SetError(error, "truncate tip id invalid"), BLOCK_INDEX_TIP_CORRUPT;
        const uint64_t tipSlot = tipId - i->meta.baseRecordCount - 1;
        BlockIndexRecord rr;
        if (!i->recordsWin.Get(tipSlot, &rr, &gerr))
            return SetError(error, "truncate tip record read failed: " + gerr),
                   BLOCK_INDEX_TIP_CORRUPT;
        newMeta.tipHash = rr.hash;
    }
    // Non-append mutation: rebuild the v3 chains STREAMING (records/derived
    // unchanged; active chain shortened). O(N), O(1) RAM.
    std::string serrA, serrB, serrC;
    if (!i->recordsWin.SetCount(i->meta.tipRecordCount, &serrA) ||
        !i->derivedWin.SetCount(i->meta.tipRecordCount, &serrB) ||
        !i->activeWin.SetCount(keepActive, &serrC))
        return SetError(error, "truncate window reset failed: " + serrA + serrB + serrC),
               BLOCK_INDEX_TIP_IO_ERROR;
    {
        if (!i->BuildChainsForStreaming())
            return SetError(error, "truncate digest rebuild failed: " +
                                   i->lastStreamError),
                   BLOCK_INDEX_TIP_IO_ERROR;
    }
    DigestV3Finalize(i->chainR, i->chainD, i->chainA, newMeta.activeFence, newMeta.contentDigest);

    const BlockIndexTipMeta savedMeta = i->meta;
    const uint64_t savedActiveCount = i->activeWin.Count();
    const BlockIndexTipMeta saved = i->meta;
    (void)saved;
    i->meta = newMeta;
    if (!i->WriteMeta(error))
    {
        i->meta = savedMeta;
        // roll the active window back to the committed count
        std::string werrR;
        i->activeWin.SetCount(savedActiveCount, &werrR); // full resync (Pop is only for single-entry rollback after PushBack)
        i->chainsValid = false;
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
    //    disconnected branch from active membership). Dense rel count = tip -
    //    baseTip; fork must be in range.
    const uint64_t activeCount = (uint64_t)(i->meta.tipHeight > baseTip)
        ? (uint64_t)(i->meta.tipHeight - baseTip) : 0;
    if (forkHeight < baseTip || forkHeight >= baseTip + (int32_t)activeCount)
    {
        if (forkHeight != baseTip)
            return SetError(error, "reorg fork height out of range"), BLOCK_INDEX_TIP_CORRUPT;
    }
    const uint64_t keepActive = (uint64_t)(forkHeight > baseTip)
        ? (uint64_t)(forkHeight - baseTip) : 0;
    const uint64_t committedRecords = i->meta.tipRecordCount;

    // 2. Build the new full records/derived/active state (Repair #3: bounded
    //    RAM). An existing record is REUSED (a committed side record is
    //    promoted to active at its branch height, with its reorg-supplied
    //    copy VALIDATED against the committed bytes -- never re-encoded); a
    //    branch record not yet in the tip is APPENDED. Promotion resolves BY
    //    HASH through the durable side index. newActive = fork prefix ids +
    //    one RecordId per branch block (promoted or appended).
    std::vector<BlockIndexId> forkPrefixIds;
    if (keepActive > 0)
        forkPrefixIds.resize(keepActive);
    std::string aerr;
    for (uint64_t rel = 0; rel < keepActive; ++rel)
    {
        if (!i->activeWin.Get(rel, &forkPrefixIds[rel], &aerr))
            return SetError(error, "reorg fork prefix read failed: " + aerr),
                   BLOCK_INDEX_TIP_IO_ERROR;
    }
    std::vector<BlockIndexId> newActive = forkPrefixIds; // branch ids appended below
    // The ids of NEW records to append to the tail stores (slot-ordered).
    std::vector<BlockIndexRecord> appendRecords;
    std::vector<BlockIndexDerivedEntry> appendDerived;
    std::set<uint256> seenBranch;
    for (size_t k = 0; k < branch.size(); ++k)
    {
        const BlockIndexRecord& rec = branch[k].record;
        if (branchHeights[k] != baseTip + (int32_t)newActive.size() + 1)
            return SetError(error, "reorg branch non-dense height"), BLOCK_INDEX_TIP_CORRUPT;
        if (!seenBranch.insert(rec.hash).second)
            return SetError(error, "reorg branch duplicate"), BLOCK_INDEX_TIP_CORRUPT;
        // Promote-or-append via the durable side index's committed view.
        BlockIndexId foundId = 0;
        const BlockIndexHashLookupStatus lstat =
            i->sideIndex.Lookup(rec.hash, &foundId, NULL);
        if (lstat == BLOCK_INDEX_HASH_LOOKUP_ERROR)
            return SetError(error, "reorg side-index lookup failed"), BLOCK_INDEX_TIP_IO_ERROR;
        if (lstat == BLOCK_INDEX_HASH_LOOKUP_FOUND)
        {
            // Already present -> promote. Validate the branch record's content
            // matches the committed record at that id (fail closed, no swap).
            if (foundId <= i->meta.baseRecordCount)
                return SetError(error, "reorg promoted id invalid"), BLOCK_INDEX_TIP_CORRUPT;
            const uint64_t slot = foundId - i->meta.baseRecordCount - 1;
            if (slot >= committedRecords)
                return SetError(error, "reorg promoted id out of range"), BLOCK_INDEX_TIP_CORRUPT;
            BlockIndexRecord committed;
            if (!i->recordsWin.Get(slot, &committed, &aerr))
                return SetError(error, "reorg promoted record read failed: " + aerr),
                       BLOCK_INDEX_TIP_IO_ERROR;
            if (committed.hashPrev != rec.hashPrev || committed.height != rec.height)
                return SetError(error, "reorg promoted record mismatch"), BLOCK_INDEX_TIP_CORRUPT;
            newActive.push_back(foundId);
        }
        else
        {
            const BlockIndexId newId = i->baseLocalToId(committedRecords +
                                                        appendRecords.size());
            appendRecords.push_back(rec);
            appendDerived.push_back(branch[k].derived);
            newActive.push_back(newId);
        }
    }
    // Dense active prefix check: every active member = fork prefix ids (read
    // from the committed store, so trivially valid) or a branch id (validated
    // above to point at a committed or freshly-appended record).
    for (size_t h = forkPrefixIds.size(); h < newActive.size(); ++h)
    {
        const BlockIndexId id = newActive[h];
        if (id <= i->meta.baseRecordCount)
            return SetError(error, "reorg branch id invalid"), BLOCK_INDEX_TIP_CORRUPT;
        const uint64_t slot = id - i->meta.baseRecordCount - 1;
        if (slot >= committedRecords + appendRecords.size())
            return SetError(error, "reorg active record missing"), BLOCK_INDEX_TIP_CORRUPT;
    }

    // 3. Persist all three stores by STREAMING complete CoW temp files: the
    //    retained committed prefix is copied BYTEWISE (slots are stable), then
    //    the branch appends are encoded at the end. Each temp is fsynced,
    //    renamed over the authoritative file, and the dir synced (fail
    //    closed on any error); tip.meta is published LAST (single commit).
    std::string werr;
    if (!i->RewriteStoresForReorg(keepActive, committedRecords,
                                  appendRecords, appendDerived, newActive,
                                  &werr))
        return SetError(error, "reorg store stream failed: " + werr), BLOCK_INDEX_TIP_IO_ERROR;

    // 3b. Side index: Put the appended records' hashes (O(new)); entries are
    //     uncommitted until tip.meta advances (reconciled at Open).
    for (size_t k = 0; k < appendRecords.size(); ++k)
    {
        std::string ierr;
        if (!i->sideIndex.Put(appendRecords[k].hash,
                              i->baseLocalToId(committedRecords + k), &ierr))
            return SetError(error, "reorg tip-hashindex put failed: " + ierr),
                   BLOCK_INDEX_TIP_IO_ERROR;
    }
    {
        std::string merr;
        if (!WriteHashIndexMarker(i->hashIndexMarkerPath,
                                  committedRecords + appendRecords.size(), &merr))
            return SetError(error, "reorg tip-hashindex marker failed: " + merr),
                   BLOCK_INDEX_TIP_IO_ERROR;
    }

    // 4. Advance tip.meta (dense tipHeight + tipHash) + rebuild the v3 chains
    //    STREAMING (O(N) time, O(1) RAM) BEFORE the meta publish.
    BlockIndexTipMeta newMeta = i->meta;
    newMeta.version = BLOCK_INDEX_TIP_META_VERSION_V3;
    newMeta.activeFence++;
    newMeta.tipRecordCount = committedRecords + appendRecords.size();
    newMeta.tipHeight = baseTip + (int32_t)newActive.size();
    if (newActive.empty())
        newMeta.tipHash = uint256(0);
    else
    {
        BlockIndexId tipId = newActive.back();
        if (tipId <= i->meta.baseRecordCount)
            return SetError(error, "reorg tip id invalid"), BLOCK_INDEX_TIP_CORRUPT;
        const uint64_t tipSlot = tipId - i->meta.baseRecordCount - 1;
        if (tipSlot >= newMeta.tipRecordCount)
            return SetError(error, "reorg tip id out of range"), BLOCK_INDEX_TIP_CORRUPT;
        if (tipSlot >= committedRecords)
        {
            if (tipSlot - committedRecords >= appendRecords.size())
                return SetError(error, "reorg tip append slot invalid"), BLOCK_INDEX_TIP_CORRUPT;
            newMeta.tipHash = appendRecords[(size_t)(tipSlot - committedRecords)].hash;
        }
        else
        {
            BlockIndexRecord rr;
            if (!i->recordsWin.Get(tipSlot, &rr, &aerr))
                return SetError(error, "reorg tip hash resolve failed: " + aerr),
                       BLOCK_INDEX_TIP_IO_ERROR;
            newMeta.tipHash = rr.hash;
        }
    }
    // SetCount()s re-point the windows at the freshly swapped files; they are
    // lazy (ring dropped), keeping the RAM bounded through the whole reorg.
    std::string werrWA, werrWB, werrWC;
    if (!i->recordsWin.SetCount(newMeta.tipRecordCount, &werrWA) ||
        !i->derivedWin.SetCount(newMeta.tipRecordCount, &werrWB) ||
        !i->activeWin.SetCount((uint64_t)newActive.size(), &werrWC))
        return SetError(error, "reorg window reset failed"), BLOCK_INDEX_TIP_IO_ERROR;
    if (!i->BuildChainsForStreaming())
        return SetError(error, "reorg digest rebuild failed"), BLOCK_INDEX_TIP_IO_ERROR;
    DigestV3Finalize(i->chainR, i->chainD, i->chainA, newMeta.activeFence, newMeta.contentDigest);
    // Commit point: adopt the new meta BEFORE writing it, exactly as
    // AppendBatch/TruncateActiveTo do (see the original note: publishing the
    // new stores against the OLD meta would silently lose the cutover).
    // Test-only: all tail stores durable, crash before the tip.meta commit.
    if (BlockIndexTipFailpointHit("FP_AFTER_TAIL_DURABLE_BEFORE_META"))
        return SetError(error, "failpoint FP_AFTER_TAIL_DURABLE_BEFORE_META"), BLOCK_INDEX_TIP_IO_ERROR;

    const BlockIndexTipMeta savedMeta = i->meta;
    i->meta = newMeta;
    if (!i->WriteMeta(error))
    {
        i->meta = savedMeta;
        // roll windows back to the PRE-reorg committed state (lazy re-point)
        std::string werrRA, werrRB, werrRC;
        i->recordsWin.SetCount(savedMeta.tipRecordCount, &werrRA);
        i->derivedWin.SetCount(savedMeta.tipRecordCount, &werrRB);
        const uint64_t savedActiveCount = (uint64_t)(savedMeta.tipHeight > baseTip)
            ? (uint64_t)(savedMeta.tipHeight - baseTip) : 0;
        i->activeWin.SetCount(savedActiveCount, &werrRC);
        i->chainsValid = false;
        return BLOCK_INDEX_TIP_IO_ERROR;
    }
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

    // (b) resulting active membership (fused from ReorgActiveTo steps 1-2;
    //     Repair #3: bounded-RAM equivalent via the durable side index).
    const uint64_t activeCount = (uint64_t)(i->meta.tipHeight > baseTip)
        ? (uint64_t)(i->meta.tipHeight - baseTip) : 0;
    if (forkHeight < baseTip || forkHeight >= baseTip + (int32_t)activeCount)
    {
        if (forkHeight != baseTip)
            return SetError(error, "fused reorg fork height out of range"), BLOCK_INDEX_TIP_CORRUPT;
    }
    const uint64_t keepActive = (uint64_t)(forkHeight > baseTip)
        ? (uint64_t)(forkHeight - baseTip) : 0;
    const uint64_t committedRecords = i->meta.tipRecordCount;
    std::vector<BlockIndexId> forkPrefixIds;
    if (keepActive > 0)
        forkPrefixIds.resize(keepActive);
    std::string aerr;
    for (uint64_t rel = 0; rel < keepActive; ++rel)
    {
        if (!i->activeWin.Get(rel, &forkPrefixIds[rel], &aerr))
            return SetError(error, "fused fork prefix read failed: " + aerr),
                   BLOCK_INDEX_TIP_IO_ERROR;
    }
    std::vector<BlockIndexId> allActive = forkPrefixIds;
    std::vector<BlockIndexRecord> appendRecords;
    std::vector<BlockIndexDerivedEntry> appendDerived;
    std::set<uint256> seenBranch;
    for (size_t k = 0; k < branch.size(); ++k)
    {
        const BlockIndexRecord& rec = branch[k].record;
        if (branchHeights[k] != baseTip + (int32_t)allActive.size() + 1)
            return SetError(error, "fused reorg branch non-dense height"), BLOCK_INDEX_TIP_CORRUPT;
        if (!seenBranch.insert(rec.hash).second)
            return SetError(error, "fused reorg branch duplicate"), BLOCK_INDEX_TIP_CORRUPT;
        BlockIndexId foundId = 0;
        const BlockIndexHashLookupStatus lstat =
            i->sideIndex.Lookup(rec.hash, &foundId, NULL);
        if (lstat == BLOCK_INDEX_HASH_LOOKUP_ERROR)
            return SetError(error, "fused side-index lookup failed"), BLOCK_INDEX_TIP_IO_ERROR;
        if (lstat == BLOCK_INDEX_HASH_LOOKUP_FOUND)
        {
            // promote existing side record (content validated against the
            // committed record at that id; fail closed, no swap)
            if (foundId <= i->meta.baseRecordCount)
                return SetError(error, "fused promoted id invalid"), BLOCK_INDEX_TIP_CORRUPT;
            const uint64_t slot = foundId - i->meta.baseRecordCount - 1;
            if (slot >= committedRecords)
                return SetError(error, "fused promoted id out of range"), BLOCK_INDEX_TIP_CORRUPT;
            BlockIndexRecord committed;
            if (!i->recordsWin.Get(slot, &committed, &aerr))
                return SetError(error, "fused promoted record read failed: " + aerr),
                       BLOCK_INDEX_TIP_IO_ERROR;
            if (committed.hashPrev != rec.hashPrev || committed.height != rec.height)
                return SetError(error, "fused promoted record mismatch"), BLOCK_INDEX_TIP_CORRUPT;
            allActive.push_back(foundId);
        }
        else
        {
            const BlockIndexId newId = i->baseLocalToId(committedRecords +
                                                        appendRecords.size());
            appendRecords.push_back(rec);
            appendDerived.push_back(branch[k].derived);
            allActive.push_back(newId);
        }
    }
    for (size_t h = forkPrefixIds.size(); h < allActive.size(); ++h)
    {
        const BlockIndexId id = allActive[h];
        if (id <= i->meta.baseRecordCount)
            return SetError(error, "fused branch id invalid"), BLOCK_INDEX_TIP_CORRUPT;
        if (id - i->meta.baseRecordCount - 1 >= committedRecords + appendRecords.size())
            return SetError(error, "fused active record missing"), BLOCK_INDEX_TIP_CORRUPT;
    }

    // Persist all four stores. tip.meta below is the SOLE commit point for BOTH
    // the invalid log and the active membership (uncommitted tails are ignored).
    std::string werr;
    if (!i->RewriteStoresForReorg(keepActive, committedRecords,
                                  appendRecords, appendDerived, allActive, &werr))
        return SetError(error, "fused store stream failed: " + werr), BLOCK_INDEX_TIP_IO_ERROR;
    if (!WriteInvalidFile(i->invalidPath, allEntries))
        return SetError(error, "fused tip-invalid failed"), BLOCK_INDEX_TIP_IO_ERROR;
    for (size_t k = 0; k < appendRecords.size(); ++k)
    {
        std::string ierr;
        if (!i->sideIndex.Put(appendRecords[k].hash,
                              i->baseLocalToId(committedRecords + k), &ierr))
            return SetError(error, "fused tip-hashindex put failed: " + ierr),
                   BLOCK_INDEX_TIP_IO_ERROR;
    }
    {
        std::string merr;
        if (!WriteHashIndexMarker(i->hashIndexMarkerPath,
                                  committedRecords + appendRecords.size(), &merr))
            return SetError(error, "fused tip-hashindex marker failed: " + merr),
                   BLOCK_INDEX_TIP_IO_ERROR;
    }

    // ONE tip.meta commit: invalid intent AND the resulting active tip together.
    const BlockIndexTipMeta savedMeta = i->meta;
    const std::vector<BlockIndexTipInvalidEntry> savedEntries = i->invalidEntries;

    BlockIndexTipMeta newMeta = i->meta;
    newMeta.version = BLOCK_INDEX_TIP_META_VERSION_V3;
    newMeta.activeFence++;
    newMeta.tipRecordCount = committedRecords + appendRecords.size();
    newMeta.tipHeight = baseTip + (int32_t)allActive.size();
    if (allActive.empty())
        newMeta.tipHash = uint256(0);
    else
    {
        const BlockIndexId tipId = allActive.back();
        if (tipId <= i->meta.baseRecordCount)
            return SetError(error, "fused tip id invalid"), BLOCK_INDEX_TIP_CORRUPT;
        const uint64_t tipSlot = tipId - i->meta.baseRecordCount - 1;
        if (tipSlot >= newMeta.tipRecordCount)
            return SetError(error, "fused tip id out of range"), BLOCK_INDEX_TIP_CORRUPT;
        if (tipSlot >= committedRecords)
        {
            // freshly appended branch tip: resolve from the local append buffer
            // (O(new)); the windows still hold the OLD committed count here.
            if (tipSlot - committedRecords >= appendRecords.size())
                return SetError(error, "fused tip append slot invalid"), BLOCK_INDEX_TIP_CORRUPT;
            newMeta.tipHash = appendRecords[(size_t)(tipSlot - committedRecords)].hash;
        }
        else
        {
            BlockIndexRecord rr;
            if (!i->recordsWin.Get(tipSlot, &rr, &aerr))
                return SetError(error, "fused tip hash resolve failed: " + aerr),
                       BLOCK_INDEX_TIP_IO_ERROR;
            newMeta.tipHash = rr.hash;
        }
    }
    // Non-append mutation: rebuild the v3 chains STREAMING (O(N), O(1) RAM)
    // over the freshly-swapped store files.
    std::string werrWA, werrWB, werrWC;
    if (!i->recordsWin.SetCount(newMeta.tipRecordCount, &werrWA) ||
        !i->derivedWin.SetCount(newMeta.tipRecordCount, &werrWB) ||
        !i->activeWin.SetCount((uint64_t)allActive.size(), &werrWC))
        return SetError(error, "fused window reset failed"), BLOCK_INDEX_TIP_IO_ERROR;
    if (!i->BuildChainsForStreaming())
        return SetError(error, "fused digest rebuild failed"), BLOCK_INDEX_TIP_IO_ERROR;
    DigestV3Finalize(i->chainR, i->chainD, i->chainA, newMeta.activeFence, newMeta.contentDigest);
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
        i->invalidEntries = savedEntries;
        DeriveInvalidSet(i->invalidEntries, &i->invalidSet);
        std::string werrRA, werrRB, werrRC;
        i->recordsWin.SetCount(savedMeta.tipRecordCount, &werrRA);
        i->derivedWin.SetCount(savedMeta.tipRecordCount, &werrRB);
        const uint64_t savedActiveCount = (uint64_t)(savedMeta.tipHeight > baseTip)
            ? (uint64_t)(savedMeta.tipHeight - baseTip) : 0;
        i->activeWin.SetCount(savedActiveCount, &werrRC);
        i->chainsValid = false;
        return BLOCK_INDEX_TIP_IO_ERROR;
    }
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
    Impl* i = impl;
    if (i->activeWin.Count() == 0)
    {
        r.status = BLOCK_INDEX_TIP_NOT_FOUND;
        return r;
    }
    // The tip active id is the LAST committed active entry (slot arithmetic);
    // O(1) via one window/pread read, never an O(records) scan.
    BlockIndexId tipId = 0;
    std::string gerr;
    if (!i->activeWin.Get(i->activeWin.Count() - 1, &tipId, &gerr))
    {
        r.status = BLOCK_INDEX_TIP_IO_ERROR;
        return r;
    }
    if (tipId <= i->meta.baseRecordCount)
    {
        r.status = BLOCK_INDEX_TIP_CORRUPT;
        return r;
    }
    const uint64_t slot = tipId - i->meta.baseRecordCount - 1;
    if (slot >= i->recordsWin.Count() || slot >= i->derivedWin.Count())
    {
        r.status = BLOCK_INDEX_TIP_CORRUPT;
        return r;
    }
    BlockIndexRecord rr;
    BlockIndexDerivedEntry dd;
    if (!i->recordsWin.Get(slot, &rr, &gerr) || !i->derivedWin.Get(slot, &dd, &gerr))
    {
        r.status = BLOCK_INDEX_TIP_IO_ERROR;
        return r;
    }
    r.status = BLOCK_INDEX_TIP_OK;
    r.record = rr;
    r.derived = dd;
    r.active = true;
    r.height = rr.height;
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
    // Repair #3: the durable side index answers the hash -> RecordId mapping
    // (O(log N) LevelDB lookup; never an O(N) RAM map or scan). Slot
    // arithmetic (id == baseRecordCount + slot + 1) is unchanged.
    BlockIndexId id = 0;
    const BlockIndexHashLookupStatus lstat =
        impl->sideIndex.Lookup(hash, &id, NULL);
    if (lstat == BLOCK_INDEX_HASH_LOOKUP_NOT_FOUND)
    {
        r.status = BLOCK_INDEX_TIP_NOT_FOUND;
        return r;
    }
    if (lstat == BLOCK_INDEX_HASH_LOOKUP_ERROR)
    {
        r.status = BLOCK_INDEX_TIP_IO_ERROR;
        return r;
    }
    const uint64_t base = impl->meta.baseRecordCount;
    if (id <= base)
    {
        r.status = BLOCK_INDEX_TIP_CORRUPT;
        return r;
    }
    const uint64_t slot = id - base - 1;
    if (slot >= impl->recordsWin.Count() || slot >= impl->derivedWin.Count())
    {
        // Repair #3: an index entry past the COMMITTED record count is an
        // uncommitted residue (crash between the index Put and the tip.meta
        // commit). The index is never authoritative; only the committed
        // stores answer. Treat it exactly like an absent hash.
        r.status = BLOCK_INDEX_TIP_NOT_FOUND;
        return r;
    }
    BlockIndexRecord rr;
    BlockIndexDerivedEntry dd;
    std::string gerr;
    if (!impl->recordsWin.Get(slot, &rr, &gerr) ||
        !impl->derivedWin.Get(slot, &dd, &gerr))
    {
        r.status = BLOCK_INDEX_TIP_IO_ERROR;
        return r;
    }
    r.status = BLOCK_INDEX_TIP_OK;
    r.record = rr;
    r.derived = dd;
    r.height = rr.height;
    // Active membership: activeIds is dense by relative index (activeIds[rel]
    // is the active RecordId at global height baseTipHeight + rel + 1), so a
    // record is active iff it occupies its own height's slot.
    r.active = false;
    const int32_t h = rr.height;
    if (h > impl->meta.baseTipHeight)
    {
        const uint64_t rel = (uint64_t)(h - impl->meta.baseTipHeight - 1);
        if (rel < impl->activeWin.Count())
        {
            BlockIndexId aId = 0;
            if (!impl->activeWin.Get(rel, &aId, &gerr))
                r.status = BLOCK_INDEX_TIP_IO_ERROR;
            else if (aId == id)
                r.active = true;
        }
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
    if (rel < 0 || rel >= (int64_t)impl->activeWin.Count())
    {
        r.status = BLOCK_INDEX_TIP_NOT_FOUND;
        return r;
    }
    // R5 + Repair #3: the id->record mapping is pure arithmetic (id ==
    // baseRecordCount + slot + 1); the entry itself comes from the bounded
    // window / one positional pread (OS page cache keeps the hot suffix cheap).
    BlockIndexId id = 0;
    std::string gerr;
    if (!impl->activeWin.Get((uint64_t)rel, &id, &gerr))
    {
        r.status = BLOCK_INDEX_TIP_IO_ERROR;
        return r;
    }
    const int64_t slot = (int64_t)id - (int64_t)impl->meta.baseRecordCount - 1;
    if (slot < 0 || slot >= (int64_t)impl->recordsWin.Count())
    {
        r.status = BLOCK_INDEX_TIP_CORRUPT;
        return r;
    }
    BlockIndexRecord rr;
    BlockIndexDerivedEntry dd;
    if (!impl->recordsWin.Get((uint64_t)slot, &rr, &gerr) ||
        !impl->derivedWin.Get((uint64_t)slot, &dd, &gerr))
    {
        r.status = BLOCK_INDEX_TIP_IO_ERROR;
        return r;
    }
    r.status = BLOCK_INDEX_TIP_OK;
    r.record = rr;
    r.derived = dd;
    r.active = true;
    r.height = rr.height;
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
    // Repair #3: stream the stores sequentially (O(N) time, O(1) auxiliary
    // RAM beyond the caller's own `out` vector). Active membership is
    // computed by slot arithmetic per record height (the active entry at
    // rel = height - baseTipHeight - 1 must equal this record's id).
    uint64_t activeRel = 0; // ascending walk over the active store
    const uint64_t activeCount = impl->activeWin.Count();
    std::string gerr;
    BlockIndexId curActiveId = 0;
    if (activeCount > 0 && !impl->activeWin.Get(0, &curActiveId, &gerr))
    {
        if (error) *error = gerr;
        return BLOCK_INDEX_TIP_IO_ERROR;
    }
    BlockIndexTipStatus overall = BLOCK_INDEX_TIP_OK;
    if (impl->recordsWin.StreamVisit(0, impl->recordsWin.Count(),
        [&](uint64_t slot, const BlockIndexRecord& rec) {
            BlockIndexTipRead r;
            r.status = BLOCK_INDEX_TIP_OK;
            BlockIndexDerivedEntry dd;
            if (!impl->derivedWin.Get(slot, &dd, &gerr))
            {
                overall = BLOCK_INDEX_TIP_IO_ERROR;
                return false; // abort the stream
            }
            r.record = rec;
            r.derived = dd;
            r.height = rec.height;
            r.active = false;
            const BlockIndexId id = impl->baseLocalToId(slot);
            if (rec.height > impl->meta.baseTipHeight && activeRel < activeCount)
            {
                const uint64_t wantRel =
                    (uint64_t)(rec.height - impl->meta.baseTipHeight - 1);
                // advance the monotone active walk
                while (activeRel < activeCount && activeRel < wantRel)
                {
                    if (!impl->activeWin.Get(activeRel, &curActiveId, &gerr))
                    {
                        overall = BLOCK_INDEX_TIP_IO_ERROR;
                        return false;
                    }
                    ++activeRel;
                }
                if (activeRel == wantRel)
                {
                    if (curActiveId == 0)
                    {
                        if (!impl->activeWin.Get(activeRel, &curActiveId, &gerr))
                        {
                            overall = BLOCK_INDEX_TIP_IO_ERROR;
                            return false;
                        }
                    }
                    if (curActiveId == id)
                        r.active = true;
                }
            }
            if (out) out->push_back(r);
            return true;
        }, &gerr) == false)
    {
        if (error && overall == BLOCK_INDEX_TIP_OK) *error = gerr;
        return overall != BLOCK_INDEX_TIP_OK ? overall : BLOCK_INDEX_TIP_IO_ERROR;
    }
    if (error) error->clear();
    return overall;
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

    // Repair #3: bounded-RAM selector. The hash -> slot map is the durable
    // side index (which maps to RECORD IDs; slot = id - baseRecordCount - 1);
    // record/derived entries are read by value through the bounded windows.
    // RAM: O(branch chain) temporary, never a full-history materialization.

    bool found = false;
    uint64_t bestJ = 0;
    std::string gerr;
    // Single sequential pass over the records store (O(N) reads are expected
    // here: selection walks the whole retained mutable tail by contract).
    if (impl->recordsWin.StreamVisit(0, impl->recordsWin.Count(),
        [&](uint64_t slot, const BlockIndexRecord& rec) {
            if (rec.height <= forkHeight)
                return true;                                    // at/below the fork
            // Effective eligibility: the PENDING intent overrides the
            // committed set for the target hash (invalidate => treat as
            // invalid; reconsider => eligible).
            bool selfInvalid = (rec.hash == pendingHash)
                                   ? pendingInvalidate
                                   : (impl->invalidSet.count(rec.hash) != 0);
            if (selfInvalid)
                return true;
            // Authenticate ancestry up to the fork: every intermediate must be
            // present in the tip window, height-consistent, not operator-invalid.
            int32_t h = rec.height;
            uint256 cur = rec.hashPrev;
            bool ok = true;
            while (h > forkHeight + 1)
            {
                bool ancInvalid = (cur == pendingHash)
                                      ? pendingInvalidate
                                      : (impl->invalidSet.count(cur) != 0);
                if (ancInvalid) { ok = false; break; }
                // resolve the ancestor via the side index
                BlockIndexId ancId = 0;
                if (impl->sideIndex.Lookup(cur, &ancId, NULL) !=
                        BLOCK_INDEX_HASH_LOOKUP_FOUND)
                { ok = false; break; }  // leaves the tip window
                if (ancId <= impl->meta.baseRecordCount)
                { ok = false; break; }
                const uint64_t ancSlot = ancId - impl->meta.baseRecordCount - 1;
                if (ancSlot >= impl->recordsWin.Count())
                { ok = false; break; }
                BlockIndexRecord p;
                if (!impl->recordsWin.Get(ancSlot, &p, &gerr))
                { ok = false; break; }
                if (p.height + 1 != h) { ok = false; break; } // inconsistent linkage
                cur = p.hashPrev;
                h = p.height;
            }
            if (!ok)
                return true;
            if (!found)
            {
                found = true;
                bestJ = slot;
                return true;
            }
            // tie-break compare by (derived.chainTrust, hash) -- by-value reads
            BlockIndexDerivedEntry curD, bestD;
            if (!impl->derivedWin.Get(slot, &curD, &gerr))
            { gerr = "derived read failed during selection"; return false; }
            if (!impl->derivedWin.Get(bestJ, &bestD, &gerr))
            { gerr = "derived read failed during selection"; return false; }
            BlockIndexRecord bestR;
            if (!impl->recordsWin.Get(bestJ, &bestR, &gerr))
            { gerr = "record read failed during selection"; return false; }
            if (curD.chainTrust > bestD.chainTrust ||
                (curD.chainTrust == bestD.chainTrust && rec.hash < bestR.hash))
                bestJ = slot;
            return true;
        }, &gerr) == false)
    {
        if (error) *error = gerr;
        return BLOCK_INDEX_TIP_IO_ERROR;
    }

    if (!found)
        return BLOCK_INDEX_TIP_OK;   // no eligible candidate above fork -> tip = fork

    // Reconstruct the branch (fork+1 .. bestTip) ascending.
    std::vector<uint64_t> chainSlots;
    {
        uint64_t j = bestJ;
        BlockIndexRecord curRec;
        if (!impl->recordsWin.Get(j, &curRec, &gerr))
        {
            if (error) *error = gerr;
            return BLOCK_INDEX_TIP_IO_ERROR;
        }
        chainSlots.push_back(j);
        while (curRec.height > forkHeight + 1)
        {
            // fork-adjacent node stops the walk; its parent is the fork
            BlockIndexId ancId = 0;
            if (impl->sideIndex.Lookup(curRec.hashPrev, &ancId, NULL) !=
                    BLOCK_INDEX_HASH_LOOKUP_FOUND ||
                ancId <= impl->meta.baseRecordCount)
            {
                if (error) *error = "branch walk lost parent below tip window";
                return BLOCK_INDEX_TIP_CORRUPT;
            }
            const uint64_t ancSlot = ancId - impl->meta.baseRecordCount - 1;
            if (ancSlot >= impl->recordsWin.Count())
            {
                if (error) *error = "branch walk lost parent below tip window";
                return BLOCK_INDEX_TIP_CORRUPT;
            }
            if (!impl->recordsWin.Get(ancSlot, &curRec, &gerr))
            {
                if (error) *error = gerr;
                return BLOCK_INDEX_TIP_IO_ERROR;
            }
            chainSlots.push_back(ancSlot);
        }
    }
    for (size_t k = chainSlots.size(); k-- > 0; )
    {
        const uint64_t idx = chainSlots[k];
        BlockIndexTipAppend ap;
        BlockIndexRecord rr;
        BlockIndexDerivedEntry dd;
        if (!impl->recordsWin.Get(idx, &rr, &gerr) ||
            !impl->derivedWin.Get(idx, &dd, &gerr))
        {
            if (error) *error = gerr;
            return BLOCK_INDEX_TIP_IO_ERROR;
        }
        ap.record = rr;
        ap.derived = dd;
        if (outBranch) outBranch->push_back(ap);
        if (outHeights) outHeights->push_back(rr.height);
    }
    if (outBestHash || outBestHeight)
    {
        BlockIndexRecord bestR;
        if (!impl->recordsWin.Get(bestJ, &bestR, &gerr))
        {
            if (error) *error = gerr;
            return BLOCK_INDEX_TIP_IO_ERROR;
        }
        if (outBestHash) *outBestHash = bestR.hash;
        if (outBestHeight) *outBestHeight = bestR.height;
    }
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
bool BlockIndexTipAuthority::IsEmpty() const
{
    // committed records count == 0 <=> no tip records (meta is the authority).
    return impl->open && impl->meta.tipRecordCount == 0;
}

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
    // An operator intent changes neither records/derived/active nor the fence, so
    // the contentDigest is ALREADY correct for the current protocol version.
    // Upgrade v1 -> v2 (same OLD digest flavour); keep a v3 tip as v3. This path
    // must NOT switch the digest flavour, or a v2 tip relabelled/validated as the
    // legacy layout would fail closed.
    if (newMeta.version != BLOCK_INDEX_TIP_META_VERSION_V3)
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
    // Repair #3: release the bounded windows' file handles and the LevelDB
    // side index handle (Close is idempotent, same as before).
    impl->recordsWin.Reset();
    impl->derivedWin.Reset();
    impl->activeWin.Reset();
    impl->sideIndex.Close();
    impl->chainsValid = false;
}