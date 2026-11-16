#ifndef INNOVA_BLOCKINDEX_V2_READER_H
#define INNOVA_BLOCKINDEX_V2_READER_H

#include "blockindex_navigation.h"
#include "blockindex_activeindex.h"
#include "blockindex_hashindex.h"
#include "blockindex_derived_state.h"
#include "blockindex_derived_replay.h"
#include "blockindex_generation_lifecycle.h"

#include <functional>
#include <string>
#include <map>
#include <utility>
#include <vector>
#include <sys/types.h>
#include <sys/stat.h>

// A.8: one immutable, CURRENT-selected generation; pointer-free historical API.
// This reader is semantic/logical read-only. It does not publish/select/build,
// auto-switch CURRENT, use cs_main, or expose CBlockIndex*.
enum BlockIndexV2ReadStatus
{
    BLOCK_INDEX_V2_READ_FOUND = 1,
    BLOCK_INDEX_V2_READ_NOT_FOUND = 2,
    BLOCK_INDEX_V2_READ_CORRUPT = 3,
    BLOCK_INDEX_V2_READ_IO_ERROR = 4,
    BLOCK_INDEX_V2_READ_NOT_OPEN = 5,
};

struct BlockIndexV2ReaderCacheStats
{
    uint64_t capacityBytes;
    uint64_t entries;
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    uint64_t bytesEstimated;
    BlockIndexV2ReaderCacheStats() : capacityBytes(0), entries(0), hits(0), misses(0), evictions(0), bytesEstimated(0) {}
};

// BOOT-001: optional per-record visitor for the reader-open integrity scan.
// The scan already decodes and CRC-checks EVERY committed record through the
// bounded forward-only primitive, so a caller that needs one cheap per-record
// derivation of the SAME immutable generation (authoritative startup's
// setStakeSeen rebuild) can collect it here instead of reading records.dat a
// second time. The visitor cannot shorten the scan: a short/corrupt record is
// still a fail-closed open failure whether or not a visitor is installed.
typedef std::function<void(BlockIndexId, const BlockIndexRecord&)> BlockIndexIntegrityScanVisitor;

// BOOT-004: bounded projection riding the SAME open-time scans.
//
// Authoritative startup needs, for a small immutable FRONTIER of hashes
// (candidate-leaves.dat), the record and the derived entry behind each hash.
// Asking for them one by one (LookupByHash + derived.Read) costs, per leaf, a
// hashindex point lookup, a records.dat read and a derived.dat read - all
// RANDOM: measured 3.06 read syscalls and 6.85 ms per leaf on a cold 8.24M
// generation (93 746 syscalls / 209.9 s for 30 633 leaves). Both values are
// already decoded by scans this open performs for integrity anyway, so they are
// collected THERE instead, for zero additional I/O.
//
// `scanFilterHashes` is a SORTED hash vector (candidate-leaves.dat order); an
// EMPTY filter (default) disables collection and leaves the reader's behaviour
// byte-identical. `collectDerivedForScanMatches` additionally collects the
// derived entry for every matched RecordId on the derived integrity pass (which
// already decodes every entry).
struct BlockIndexScanRecordMatch
{
    BlockIndexId id;
    BlockIndexRecord record;
    BlockIndexScanRecordMatch() : id(BLOCK_INDEX_ID_INVALID) {}
};

typedef BlockIndexDerivedScanMatch BlockIndexScanDerivedMatch;

// BOOT-004: ONE-SHOT ownership hand-over of what an open's scans observed for a
// requested frontier.
//
// Why a hand-over instead of accessors: the projection belongs to the IMMUTABLE
// GENERATION (a sealed byte image), and the only legitimate consumer is the
// candidate-frontier build that runs immediately after the open. Keeping the
// data inside the reader (as readable accessors) leaves it reachable for the
// whole process lifetime, which is how a performance projection can quietly
// become a second input to the MUTABLE TIP lifecycle (open/append/recovery).
// TakeScanProjection() moves the vectors OUT and leaves the reader empty, so
// any later consumer -- the navigator's cold reader, the live authority, the
// tip store -- sees nothing and cannot depend on scan state by accident.
//
// The projection is NOT authority: it is a cache of bytes the open already
// validated. `Complete(leafCount)` states whether it can serve a frontier of
// that size; when it cannot, the builder MUST fall back to per-hash lookups
// (honest, slower) instead of treating absence as emptiness.
struct BlockIndexScanProjection
{
    uint64_t generation;    // generation the scan ran on (identity binding)
    size_t requestedLeaves; // size of options.scanFilterHashes at open time
    bool scanRan;           // the full per-record integrity scan actually ran
    std::vector<BlockIndexScanRecordMatch> records;
    std::vector<BlockIndexScanDerivedMatch> derived;

    BlockIndexScanProjection() : generation(0), requestedLeaves(0), scanRan(false) {}

    // True only when this projection can answer a frontier of exactly
    // `leafCount` entries from the generation `expectedGeneration`.
    bool Complete(size_t leafCount, uint64_t expectedGeneration) const
    {
        return scanRan && generation == expectedGeneration &&
               requestedLeaves == leafCount &&
               records.size() == leafCount && derived.size() == leafCount;
    }
};

struct BlockIndexV2ReaderOptions
{
    uint64_t cacheCapacityBytes;
    BlockIndexIntegrityScanVisitor integrityScanVisitor;
    std::vector<uint256> scanFilterHashes;
    bool collectDerivedForScanMatches;
    BlockIndexV2ReaderOptions() : cacheCapacityBytes(64ULL * 1024ULL * 1024ULL),
                                 collectDerivedForScanMatches(false) {}
};

class BlockIndexV2Reader
{
public:
    BlockIndexV2Reader();
    ~BlockIndexV2Reader();

    // A.10.1q / Stage1: move support. The reader owns several open LevelDB
    // handles (store/active/hashIndex); this project's authoritative startup
    // uses one process-open reader shared across bootstrap + navigator to avoid
    // a second hashindex LOCK. Moving transfers the open handles; the source is
    // left closed (no LevelDB leak / no double close).
    BlockIndexV2Reader(BlockIndexV2Reader&& other) noexcept;
    BlockIndexV2Reader& operator=(BlockIndexV2Reader&& other) noexcept;
    BlockIndexV2Reader(const BlockIndexV2Reader&) = delete;
    BlockIndexV2Reader& operator=(const BlockIndexV2Reader&) = delete;
    bool Open(const std::string& root, const BlockIndexV2ReaderOptions& options, std::string* error);
    void Close();
    bool IsOpen() const;
    uint64_t Generation() const;
    uint64_t RecordCount() const;

    // T2 (Cohort T): bounded SEQUENTIAL traversal of every committed record in
    // RecordId order. Same predicate surface as GetRecordById but with none of
    // its per-record costs (reader lock per call, LRU lookup/insert/eviction,
    // active-membership work, snapshot materialization) - appropriate for a
    // known full-generation scan. O(1) auxiliary memory, no LRU population, no
    // CBlockIndex materialization, fail-closed on short/corrupt records.
    bool ForEachRecordSequential(const std::function<bool(BlockIndexId, const BlockIndexRecord&)>& visit,
                                 std::string* error) const;
    std::string GenerationPath() const;
    // Read-only selected manifest retained by this already-open reader.
    const FixedBlockIndexManifest& Manifest() const;
    BlockIndexSnapshot GetTip() const;
    bool CurrentSelectionChanged(std::string* error) const;
    BlockIndexV2ReaderCacheStats CacheStats() const;
    bool GetGenerationInputDigest(unsigned char out[32], std::string* error) const;
    // True when THIS open performed the full per-record integrity scan (i.e. the
    // verify-once memo did not short-circuit it) AND an integrityScanVisitor was
    // installed. Callers that collect state in that visitor may use the collected
    // state only when this is true; otherwise they must re-derive it.
    bool IntegrityScanRanOnOpen() const;

    // BOOT-004: moves this open's scan projection OUT of the reader (one-shot;
    // the reader keeps nothing afterwards). Callers MUST check
    // projection.Complete(leafCount, reader.Generation()) before using it and
    // fall back to per-hash lookups otherwise.
    BlockIndexScanProjection TakeScanProjection();
    BlockIndexV2ReadStatus GetRecordById(BlockIndexId id, BlockIndexSnapshot* out, std::string* error) const;
    BlockIndexV2ReadStatus LookupByHash(const uint256& hash, BlockIndexSnapshot* out, std::string* error) const;
    BlockIndexV2ReadStatus GetActiveByHeight(int height, BlockIndexSnapshot* out, std::string* error) const;
    BlockIndexV2ReadStatus GetNextActive(BlockIndexId id, BlockIndexSnapshot* out, std::string* error) const;
    BlockIndexV2ReadStatus GetParent(BlockIndexId id, BlockIndexSnapshot* out, std::string* error) const;
    BlockIndexV2ReadStatus GetAncestor(BlockIndexId id, int targetHeight, BlockIndexSnapshot* out, std::string* error) const;
    BlockIndexV2ReadStatus GetStakeModifierTime(BlockIndexId id, int64_t* out, std::string* error) const;
    BlockIndexV2ReadStatus GetStakeModifierChecksum(BlockIndexId id, unsigned int* out, std::string* error) const;
    // Derived serialized block size from derived.dat (Stage D). *outHasSize is
    // false when the generation/record carries no authoritative value: an absent
    // value is reported unavailable, NEVER as zero, and never by rescanning the
    // block file.
    BlockIndexV2ReadStatus GetBlockSize(BlockIndexId id, unsigned int* out, bool* outHasSize, std::string* error) const;
    BlockIndexV2ReadStatus GetStakingMetadata(BlockIndexId id, BlockIndexStakingMetadata* out, std::string* error) const;
    BlockIndexV2ReadStatus FindFork(BlockIndexId a, BlockIndexId b, BlockIndexSnapshot* out, std::string* error) const;

private:
    struct CacheEntry { BlockIndexSnapshot snapshot; std::list<BlockIndexId>::iterator lru; CacheEntry() {} };
    mutable CCriticalSection cs;
    bool open;
    std::string rootPath;
    std::string generationPath;
    uint64_t generation;
    FixedBlockIndexManifest manifest;
    // BOOT-004: results of the open-time scans for options.scanFilterHashes.
    // Cleared by TakeScanProjection(); the reader never serves them twice.
    std::vector<BlockIndexScanRecordMatch> scanRecordMatches;
    std::vector<BlockIndexScanDerivedMatch> scanDerivedMatches;
    size_t scanRequestedLeaves;
    FixedBlockIndexStore store;
    BlockIndexActiveIndex active;
    BlockIndexHashIndex hashIndex;
    // G1-A: optional derived.dat companion store. Authoritative generations
    // carry per-record chainTrust (and stake-modifier derived fields) in
    // derived.dat. When present and opened, SnapshotFromRecord fills nChainTrust
    // (and the derived stake-modifier fields) so the by-value authority is also
    // authoritative for chainTrust — required for the boundary AcceptBlock/
    // AddToBlockIndex to compute correct heritage. Absent (V1/shadow) -> trust
    // stays 0 (prior behavior).
    //
    // R3G (PM1-P0-05): when the companion is PRESENT-BUT-CORRUPT, Open performs
    // the deterministic derived rebuild over the authoritative generation
    // inputs (THE shared primitive, blockindex_derived_replay) and serves the
    // SNAPSHOT-TYPE derived fields (chainTrust / modifierTime /
    // modifierChecksum availability) from the rebuild.
    //
    // MEM-003: the rebuild target is an ON-DISK derived.dat in a scratch
    // directory (derivedRebuildDir), re-opened through the normal read-only
    // path and CRC-verified, instead of a std::map node per record. Resident
    // cost is bounded (one entry + page cache) instead of O(records) nodes, so
    // an emergency rebuild cannot break a hard memory cap. The scratch file
    // lives OUTSIDE the immutable generation (generation immutability holds) and
    // is removed when the reader closes.
    BlockIndexDerivedStateStore derived;
    // R3G: DERIVED-TYPE rebuild target (snapshot fields only; see above).
    BlockIndexDerivedStateStore rebuiltDerived;
    bool hasRebuiltDerived;
    std::string derivedRebuildDir;
    // R3G: the generation's blk*.dat directory, recorded at Open for a possible
    // derived rebuild (nSize materializer). Empty when unavailable.
    std::string blockDataDirForRebuild;
    uint64_t cacheCapacity;
    mutable std::map<BlockIndexId, CacheEntry> cache;
    mutable std::list<BlockIndexId> lru;
    mutable BlockIndexV2ReaderCacheStats stats;
    // BOOT-001: see IntegrityScanRanOnOpen().
    bool integrityScanRanOnOpen;
    // R2-PERF (C1/LOCATOR): bounded cache of the CURRENT-selection marker so
    // CurrentSelectionChanged stops re-reading <root>/blockindex-current (a
    // per-call file open+fread+fclose) on EVERY by-value logical resolve — the
    // IBD/P2P-locator hot path. CURRENT only changes via lifecycle Publish/Select
    // (atomic temp+rename -> new inode/mtime), so a per-call lstat comparison
    // detects any genuine rewrite and rebuilds the cache; a stat match returns
    // the cached verdict without touching the file. Preserves exact semantics
    // (fail-closed, NEW marker detection), adds no residency, O(1) memory.
    struct CurrentSelectionCache {
        bool valid;
        bool changed;
        dev_t dev;
        ino_t ino;
        off_t size;
        long mtimeSec;
        long mtimeNsec;
        CurrentSelectionCache()
            : valid(false), changed(true), dev(0), ino(0), size(0),
              mtimeSec(0), mtimeNsec(0) {}
    };
    mutable CurrentSelectionCache currentSelectionCached_;
    BlockIndexSnapshot SnapshotFromRecord(BlockIndexId id, const BlockIndexRecord& record, bool activeRecord) const;
    void CachePut(const BlockIndexSnapshot& value) const;
};

// Verify-once integrity memo (see blockindex_v2_reader.cpp Open): the number
// of full record scans actually performed, and a reset for tests.
uint64_t BlockIndexV2ReaderIntegrityScansForTesting();
void BlockIndexV2ReaderResetIntegrityMemoForTesting();

#endif
