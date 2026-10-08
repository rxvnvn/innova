// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#ifndef INNOVA_BLOCKINDEX_TIP_H
#define INNOVA_BLOCKINDEX_TIP_H

#include "blockindex_derived_state.h"
#include "blockindex_activeindex.h"
#include "blockindex_hashindex.h"
#include "blockindex_navigation.h"
#include "blockindex_startup_authority.h"

#include <boost/shared_ptr.hpp>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

// 2b-mutable: dedicated persistent mutable tip authority (blockindex_tip/).
//
// Permanent Block Index V2 write architecture:
//
//        immutable CURRENT-selected V2 generation
//                     +
//        dedicated blockindex_tip/ mutable authority
//                     |
//                  current V2 authority
//
// The mutable tip is NOT a temporary compatibility patch. Its job is to make
// blocks accepted live above the immutable base generation tip S persistent
// V2 authority (with active + side-branch representation, derived/DAG state,
// and crash-consistent append), while keeping the base generation byte-immutable
// so its digest / capability / offline-build contract is untouched.
//
// Storage layout (all under <v2root>/blockindex_tip/):
//   tip.meta       atomic commit point (tmp+fsync+rename): base-generation
//                  identity, tip height, tip hash, fence, content digest.
//   tip-records.dat  appendable BlockIndexRecord tail (RecordId continues past
//                    base recordCount).
//   tip-active.dat   appendable dense height->RecordId tail.
//   tip-derived.dat  V2 56-byte derived entries (chainTrust, stake checksum,
//                    stakeModifierTime, nSize, CRC).
//   tip-index/        LevelDB hash->RecordId for the tip namespace.
//
// Crash model: tip.meta is the SINGLE commit point. Each accepted block appends
// to the three appendable files + LevelDB batch, then advances tip.meta. On
// open, every store is validated against tip.meta and uncommitted tails are
// truncated back to the committed tip (deterministic, O(tail)). Fail-closed on
// base/tip mismatch or corrupt commit state.
//
// Residency: this store itself is by-value; it does NOT keep CBlockIndex
// resident. Resident residency is owned by the HotOwner live tail (P2).

// Mutable tip protocol version history:
//   v1 (104-byte tip.meta): base identity + committed tip + fence + content
//                           digest. Carried NO operator-invalid authority.
//   v2 (140-byte tip.meta): adds the durable operator-invalid log. The invalid
//                           set is an APPEND-ONLY log (tip-invalid.dat) whose
//                           committed length + digest ride in tip.meta, so Open
//                           truncates an uncommitted tail exactly like the other
//                           tip stores (crash-safe, deterministic) -- the exact
//                           same fail-safe methodology as tip-records/derived/
//                           active. tip.meta remains the SINGLE commit point.
// A v1 tip remains readable: on open its operator-invalid set is EMPTY and no
// rewrite occurs merely to read it. The first legitimate new mutable commit
// upgrades the meta to v2. An UNKNOWN future version FAILS CLOSED (never
// silently guessed).
static const uint32_t BLOCK_INDEX_TIP_META_VERSION = 2;
static const uint32_t BLOCK_INDEX_TIP_META_VERSION_V1 = 1;
// v3 (Repair #2): the content digest is the chained append accumulator
//   digest_v3 = SHA256( Rch || Dch || Ach || activeFence )
// with three INDEPENDENT index-order chains over the committed records/derived/
// activeIds (same per-element bytes as v2). It is a pure function of the
// committed state, so it is identical whether maintained incrementally on a
// normal append (O(new)) or rebuilt by Open (O(N)). v3 reuses the SAME 140-byte
// tip.meta layout; only the 32-byte contentDigest MEANING changes. A v2 tip
// opens and validates with the v2 digest; the first new commit upgrades to v3.
// A v3 tip is NOT readable by a pre-v3 binary (fail closed) -- accepted one-way.
static const uint32_t BLOCK_INDEX_TIP_META_VERSION_V3 = 3;

// R3 test-only failpoints around the durability boundaries. Inert unless a test
// explicitly arms them; production code never arms them. Names:
//   FP_DURING_TAIL_UPDATE                  - after a tail temp file is durable,
//                                            before its atomic rename
//   FP_AFTER_TAIL_DURABLE_BEFORE_META      - all tail stores durable, before
//                                            the tip.meta commit
//   FP_BEFORE_META_RENAME                  - tip.meta temp durable, before rename
//   FP_AFTER_META_RENAME_BEFORE_DIRSYNC    - tip.meta renamed, before dir fsync
void BlockIndexTipSetFailpointForTesting(const std::string& name, bool armed);
bool BlockIndexTipFailpointHit(const std::string& name);

struct BlockIndexTipMeta
{
    uint32_t version;
    uint64_t baseGeneration;      // immutable base generation this tip extends
    uint64_t baseRecordCount;     // base records.dat recordCount (RecordId base)
    int32_t  baseTipHeight;       // base generation committed tip height S
    int32_t  tipHeight;           // highest committed tip height (active chain)
    uint256  tipHash;             // tip block hash
    uint64_t tipRecordCount;      // total records in tip-records.dat (committed)
    uint8_t  activeFence;         // monotonic reorg fence (increments per reorg)
    unsigned char contentDigest[32]; // SHA256 over all tip stores' committed region
    // ---- v2 mutable protocol: durable operator-invalid authority ----
    uint32_t invalidLogCount;        // committed entries in tip-invalid.dat
    unsigned char invalidDigest[32]; // SHA256 over the committed invalid log

    BlockIndexTipMeta()
        : version(BLOCK_INDEX_TIP_META_VERSION), baseGeneration(0),
          baseRecordCount(0), baseTipHeight(-1), tipHeight(-1), tipHash(0),
          tipRecordCount(0), activeFence(0), invalidLogCount(0)
    {
        memset(contentDigest, 0, 32);
        memset(invalidDigest, 0, 32);
    }
};

enum BlockIndexTipStatus
{
    BLOCK_INDEX_TIP_OK = 0,
    BLOCK_INDEX_TIP_NOT_FOUND = 1,
    BLOCK_INDEX_TIP_NOT_ACTIVE = 2,
    BLOCK_INDEX_TIP_UNAVAILABLE_DERIVED_STATE = 3,
    BLOCK_INDEX_TIP_CORRUPT = 4,
    BLOCK_INDEX_TIP_IO_ERROR = 5,
    BLOCK_INDEX_TIP_GENERATION_MISMATCH = 6,
    BLOCK_INDEX_TIP_MUST_TRUNCATE = 7,
};

// One block submitted for append. The caller (consensus engine) supplies the
// by-value record + derived state that legacy AddToBlockIndex already computed
// (chainTrust, stake modifier checksum/time, nSize). The tip store persists and
// indexes it; it does NOT re-derive consensus.
struct BlockIndexTipAppend
{
    BlockIndexRecord       record;
    BlockIndexDerivedEntry derived;

    BlockIndexTipAppend() : record(), derived() {}
};

// Read result aligned to BlockIndexStartupResult semantics so the tip authority
// can be composed with the base V2 authority.
struct BlockIndexTipRead
{
    BlockIndexTipStatus status;
    BlockIndexRecord    record;
    BlockIndexDerivedEntry derived;
    bool                active;
    int32_t             height;

    BlockIndexTipRead()
        : status(BLOCK_INDEX_TIP_NOT_FOUND), record(), derived(), active(false),
          height(-1) {}
};

class BlockIndexTipAuthority
{
public:
    BlockIndexTipAuthority();
    ~BlockIndexTipAuthority();

    // Create a fresh, empty tip store anchored to a base generation.
    // The tip store is created (empty) at <root>/blockindex_tip/.
    // baseTipHeight is the base generation's committed tip height S; the tip's
    // dense active array is relative to S (activeIds[0] == height S+1), so it
    // does NOT grow to O(tip height).
    static bool Create(const std::string& root,
                       uint64_t baseGeneration,
                       uint64_t baseRecordCount,
                       int32_t baseTipHeight,
                       BlockIndexTipAuthority* out,
                       std::string* error);

    // Open an existing tip store, validating base-generation binding and every
    // store against tip.meta (fail closed; truncate uncommitted tails).
    // If open_prepared_to_truncate is set, an uncommitted tail is repaired to
    // the committed tip; otherwise MUST_TRUNCATE is returned for the caller to
    // decide.
    static bool Open(const std::string& root,
                     uint64_t expectedBaseGeneration,
                     BlockIndexTipAuthority* out,
                     std::string* error);

    // Atomic append of one block's record+derived entry. Commits the record to
    // tip-records.dat, derived entry to tip-derived.dat, active member to
    // tip-active.dat (if activeHeight provided, must be tipHeight+1), hash->
    // RecordId to tip-index/, then advances tip.meta. If a later step fails the
    // whole append is rolled back (all stores restored to the previous
    // committed tip). Returns OK on success.
    BlockIndexTipStatus Append(const BlockIndexTipAppend& blk,
                               int32_t activeHeight,      // -1 => side branch (not active)
                               std::string* error);

    // Atomic batch append (multiple blocks, single fsync per store). Used by
    // deep reorg catch-up / restart replay. activeHeights[i] -1 means side.
    BlockIndexTipStatus AppendBatch(const std::vector<BlockIndexTipAppend>& blocks,
                                    const std::vector<int32_t>& activeHeights,
                                    std::string* error);

    // Truncate the active chain back to height (reorg). Side records are kept.
    // Increases activeFence. Returns new tip.
    BlockIndexTipStatus TruncateActiveTo(int32_t height, std::string* error);

    // Reorg the ACTIVE chain to the given reconnect branch (fork+1..newTip) after
    // a real Reorganize. Unlike Append/AppendBatch (which are idempotent replay
    // and SKIP a hash already present as a side record), this PROMOTES existing
    // records into active membership: the active chain becomes exactly the branch
    // (dense over [baseTipHeight, baseTipHeight+branch.size()]), records already
    // present as side are reclassified active, and any branch record not yet in
    // the tip is appended. The caller supplies the branch in height-ascending
    // order (forkHeight+1..tip) with matching active heights. Fail closed on any
    // inconsistency/partial application (tip left at forkHeight, recoverable).
    BlockIndexTipStatus ReorgActiveTo(int32_t forkHeight,
                                      const std::vector<BlockIndexTipAppend>& branch,
                                      const std::vector<int32_t>& branchHeights,
                                      std::string* error);

    // ---- reads (by-value, composed as tip authority) ----
    BlockIndexTipRead GetTip() const;
    // Lookup a block by hash within the tip namespace.
    BlockIndexTipRead LookupByHash(const uint256& hash, std::string* error) const;
    // Lookup a block by active height. Only matches active members.
    BlockIndexTipRead LookupActiveByHeight(int32_t height, std::string* error) const;
    // Parent lookup by hash (record.hashPrev).
    BlockIndexTipRead LookupParent(const uint256& childHash, std::string* error) const;
    // Next active successor (active height+1 member whose record.hashPrev == current).
    BlockIndexTipRead LookupNextActive(const uint256& curHash, std::string* error) const;

    // ---- limits & identity ----
    uint64_t BaseGeneration() const;
    uint64_t BaseRecordCount() const;
    int32_t  TipHeight() const;
    uint256  TipHash() const;
    uint64_t TipRecordCount() const;
    uint8_t  ActiveFence() const;
    bool     IsOpen() const;
    bool     IsEmpty() const;          // no tip records committed

    // ---- durable operator-invalid authority (v2 mutable protocol) ----
    // By-value only: never holds CBlockIndex pointers, never requires
    // historical residency. Persisted in tip-invalid.dat, committed by tip.meta
    // (invalidLogCount + invalidDigest) as part of the SAME mutable authority
    // publication as the committed tip.
    uint32_t InvalidLogCount() const;
    bool     IsOperatorInvalid(const uint256& hash) const;
    std::set<uint256> OperatorInvalidSet() const;
    // Append one operator intent to the log and commit it as part of the mutable
    // tip authority (tip.meta is the commit point): invalidate=true records the
    // hash invalid; invalidate=false (reconsider) clears it. Idempotent -- an
    // intent that does not change the current derived state is a no-op (no log
    // entry). Fail closed on IO/validation error; committed authority unchanged.
    BlockIndexTipStatus SetOperatorInvalid(const uint256& hash,
                                           bool invalidate,
                                           std::string* error);

    // FUSED mutable authority transition (V2-R2D): apply ONE operator-invalid
    // intent AND the resulting active-chain reorg (truncate to forkHeight, then
    // promote/append `branch`) as a SINGLE logical authority publication. All
    // stores are written (records/derived/active/invalid), then ONE tip.meta
    // WriteMeta commits invalidLogCount+invalidDigest AND the resulting active
    // fields (tipRecordCount/tipHeight/tipHash/activeFence/contentDigest)
    // TOGETHER. There is therefore no ordinary successful path where the
    // committed invalid set and the committed tip describe incompatible
    // authority states. The invalid intent is idempotent (no log entry when it
    // does not change the derived set). Physical publication back-stops on the
    // single tip.meta commit point; GLOBAL cross-store crash atomicity remains
    // PM1-P0-03/04 (OPEN).
    BlockIndexTipStatus ApplyOperatorInvalidAndReorg(
        const uint256& hash, bool invalidate,
        int32_t forkHeight,
        const std::vector<BlockIndexTipAppend>& branch,
        const std::vector<int32_t>& branchHeights,
        std::string* error);

    // ---- V2-R2D bounded eligible-tip selection (mutable-authority) ----
    // Enumerate committed tip records (bounded to the mutable tip window; never
    // the full history). Each result carries record + persisted derived state.
    BlockIndexTipStatus AllRecords(std::vector<BlockIndexTipRead>* out,
                                   std::string* error) const;

    // Select the best ELIGIBLE tip strictly above forkHeight using ONLY persisted
    // V2 tip state (no mapBlockIndex residency, no full-history materialization):
    //   - a candidate is ineligible if it IS the invalid target, is itself
    //     operator-invalid, or any ancestor within the tip window up to forkHeight
    //     is invalid / missing / height-inconsistent;
    //   - eligibility uses the committed operator-invalid set;
    //   - selection maximizes the canonical accumulated chainTrust (R1 semantics:
    //     the persisted derived chainTrust value, NOT an independent formula),
    //     deterministic tie-break by hash.
    // Returns the reconnect branch (forkHeight+1 .. bestTip, ascending) ready for
    // ApplyOperatorInvalidAndReorg. When no eligible candidate exists, the branch
    // is EMPTY and outBestHeight == forkHeight (tip = fork). OK on success.
    BlockIndexTipStatus SelectBestEligibleBranch(
        int32_t forkHeight,
        const uint256& pendingHash,
        bool pendingInvalidate,
        std::vector<BlockIndexTipAppend>* outBranch,
        std::vector<int32_t>* outHeights,
        uint256* outBestHash,
        int32_t* outBestHeight,
        std::string* error) const;

    void Close();

private:
    struct Impl;
    Impl* impl;
};

#endif // INNOVA_BLOCKINDEX_TIP_H