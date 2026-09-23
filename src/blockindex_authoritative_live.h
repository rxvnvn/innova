// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef INNOVA_BLOCKINDEX_AUTHORITATIVE_LIVE_H
#define INNOVA_BLOCKINDEX_AUTHORITATIVE_LIVE_H

#include "blockindex_tip.h"
#include "blockindex_live_tail.h"
#include "blockindex_live_acceptance.h"
#include "blockindex_v2_reader.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

// G1 — production live-authority seam for BY_VALUE_AUTHORITATIVE mode.
//
// Closes the Window-1 Finding-2 gap in the REAL production block path:
//   ProcessBlock -> AcceptBlock -> (parent resolution) -> AddToBlockIndex -> ...
// currently resolves every parent via mapBlockIndex.find(hashPrevBlock) and
// rejects ABREJECT_PREV_NOT_FOUND when the parent is authoritative-but-not-
// resident. This module retains the mutable authority the live path needs so
// that above-base blocks S+1..  can cross the authoritative boundary.
//
//   AUTHORITY != MATERIALIZATION != RESIDENCY LIFETIME  (A.10.1d contract).
//   - Authority:  base immutable V2 generation (the reader) + mutable
//                 blockindex_tip (BlockIndexTipAuthority).
//   - Materialization: the composite BlockIndexLiveTailMaterializer resolves a
//                 logical hash by value from tip-then-base, no resident graph.
//   - Residency: bounded BlockIndexLiveTail (Horizon = -blockindexlivetail);
//                 never a consensus/validation/reorg-depth limit.
//
// This is a CACHE/AUTHORITY seam, NOT a consensus change: it never alters how
// a block is validated. It supplies:
//   1. ResolveParent(hash): by-value answer "is the parent known, and where"
//      (base V2 or mutable tip), so the live path does NOT require the parent
//      to be resident in mapBlockIndex.
//   2. MaterializeParent(hash): obtain a bounded resident CBlockIndex for the
//      parent (pinned for the validation lifetime) via the composite tail, so
//      the legacy consensus engine can run unchanged on it.
//   3. Persist accepted block(s) into the mutable tip authority (and keep the
//      live-tail residency bounded), so post-S authority is permanent.
//   4. Orphan recording for a block whose parent resolves by value but is at a
//      non-current height (or unknown), so a later join/reorg is possible.
//
// The retained object lives in g_authoritativeContext (blockindex_authoritative_
// startup) for the daemon lifetime and is bound to the SAME single process-open
// base reader (A.10.1q) + the mutable tip under <v2Root>/blockindex_tip.
//
// blockindexlivetail is a RESIDENCY HORIZON ONLY. A deep valid reorg whose
// ancestry is older than the horizon MUST be served by materializing the needed
// historical blocks from V2 on demand and running them; it is never rejected
// for exceeding N. This module never imposes a reorg-depth cap.

enum BlockIndexAuthoritativeParentStatus
{
    BLOCK_INDEX_AUTHORITATIVE_PARENT_FOUND = 0,
    BLOCK_INDEX_AUTHORITATIVE_PARENT_NOT_FOUND,
    BLOCK_INDEX_AUTHORITATIVE_PARENT_NOT_ACTIVE,
    BLOCK_INDEX_AUTHORITATIVE_PARENT_FAILURE
};

// R2c.2/S6-repair-cycle-2 (B1): authoritative by-value spend-maturity verdict.
// MATURE = the source block is not an ancestor of the spending context within
// the bounded window; IMMATURE = found at depth < maxDepth (reject, exactly
// like the legacy predicate); UNAVAILABLE = the authority cannot answer
// (fail closed - the caller must reject, never assume mature).
enum BlockIndexAuthoritativeMaturityStatus
{
    BLOCK_INDEX_MATURITY_MATURE = 0,
    BLOCK_INDEX_MATURITY_IMMATURE,
    BLOCK_INDEX_MATURITY_UNAVAILABLE
};

struct BlockIndexAuthoritativeParentInfo
{
    uint256 hash;
    int height;
    bool proofOfStake;
    bool active;
    unsigned int nFile;
    unsigned int nBlockPos;
    BlockIndexAuthoritativeParentInfo()
        : hash(0), height(-1), proofOfStake(false), active(false),
          nFile(0), nBlockPos(0) {}
};
class BlockIndexAuthoritativeLive
{
public:
    BlockIndexAuthoritativeLive();
    ~BlockIndexAuthoritativeLive();

    BlockIndexAuthoritativeLive(const BlockIndexAuthoritativeLive&) = delete;
    BlockIndexAuthoritativeLive& operator=(const BlockIndexAuthoritativeLive&) = delete;

    // Open/reuse the mutable tip under <v2Root>/blockindex_tip, bind the
    // composite tail to the given base reader + tip, set the residency horizon,
    // and arm the acceptance seam. baseReader must be open and MUST outlive this
    // object. If the tip store does not exist it is created (empty, anchored to
    // the base generation). Fails closed on base/tip mismatch or corrupt tip.
    bool Open(const std::string& v2Root,
              const BlockIndexV2Reader* baseReader,
              int livetailHorizon,
              std::string* error);

    // ---- parent resolution (by value) ----
    // Resolve a parent hash across base V2 + mutable tip. Returns true and sets
    // parentHeight (>=0; -1 if only KNOWN, not height-resolved cheaply) when the
    // parent is known by-value; false when unknown (genuine orphan) or error.
    bool ResolveParent(const uint256& parentHash, int* parentHeight,
                       std::string* error) const;

    // Resolve cold immutable V2 or bounded mutable-hot block metadata by value.
    // requireActive is intentionally false: DAG merge parents and sibling blocks
    // may be legitimate side records. nFile/nBlockPos are materialization
    // coordinates only; their presence does not establish logical authority.
    BlockIndexAuthoritativeParentStatus ResolveParentInfo(
        const uint256& parentHash,
        BlockIndexAuthoritativeParentInfo* out,
        std::string* error) const;

    // Materialize a bounded resident CBlockIndex for a logical hash via the
    // composite live tail (tip-then-base). Returns a pinned handle; the returned
    // CBlockIndex* is valid while the handle is alive. Sparse-hot topology is
    // served by the tail; caller must hold the handle for the whole operation.
    // BlockIndexHotStatus is returned so a caller can fail closed on authority or
    // materialization failure (never silently fall back to legacy residency).
    BlockIndexHotStatus Materialize(const uint256& hash,
                                    BlockIndexHotHandle* out) const;

    // Materialize a full-topology CBlockIndex parent (pprev/pnext/pskip linked)
    // for `hash`, pinned for the operation lifetime. The chain is materialized by
    // value down to the base boundary (bounded by the live-tail horizon; a deep
    // ancestry beyond the horizon is served by the by-value walk, never a reorg
    // cap). Returns the parent CBlockIndex* (valid while `out` is alive) or NULL +
    // error on authority/materialization failure (fail-closed).
    CBlockIndex* MaterializeParentChain(const uint256& hash,
                                        BlockIndexHotHandle* out,
                                        std::string* error) const;

    // R2c.2/S6-repair: narrowly scoped, CALLER-OWNED variant of the bounded
    // full-topology parent materialization. Same by-value walk policy as
    // MaterializeParentChain (tip-then-base; floor = base tip height - WALK),
    // but the materialized objects are appended to the CALLER's containers and
    // are NOT registered in the operation-global store — so their lifetime is
    // exactly the caller's, never dependent on
    // ReleaseOperationMaterializations()/the next ProcessBlock. `walkLookupFailed`
    // (optional) distinguishes a chain-walk lookup failure (the operation-global
    // store's fail-closed rollback trigger) from floor-mismatch/empty failures.
    // Caller must hold cs_main. Returns the requested parent CBlockIndex*
    // (valid while the caller keeps `objs`/`owns` alive) or NULL + error
    // (fail closed; no legacy fallback).
    CBlockIndex* MaterializeParentChainInto(const uint256& hash,
                                            std::vector<CBlockIndex*>* objs,
                                            std::vector<uint256*>* owns,
                                            std::string* error,
                                            bool* walkLookupFailed = NULL) const;

    // Resolve a logical hash to a FULL by-value BlockIndexSnapshot from the
    // CURRENT mutable retained tail (tip first, then the immutable base
    // reader). This is the single current-tail snapshot seam exposed to the
    // authoritative resolver so post-generation retained blocks (present only
    // in the blockindex_tip mutable authority, absent from the selected
    // immutable generation) can be resolved by value. Returns OK + the snapshot
    // on found; AUTHORITY_MISSING when not present in the composite tail;
    // CORRUPT_METADATA/I/O on a fail-closed authority error.
    BlockIndexHotStatus ResolveBlockSnapshot(const uint256& hash,
                                             BlockIndexSnapshot* out,
                                             std::string* error) const;

    // R2c.2/S6-repair-cycle-2 (B1): authoritative by-value verdict for the
    // legacy ConnectInputs coinbase/coinstake maturity predicate ("is the
    // source block, identified by its disk position nFile/nBlockPos, among
    // the ancestors of startHash at depth < maxDepth?"). Never uses pprev or
    // mapBlockIndex residency; each step resolves by value (tip authority
    // first, then the immutable base generation); bounded by maxDepth (the
    // caller passes nCoinbaseMaturity), never by chain history.
    // MATURE / IMMATURE / UNAVAILABLE (fail closed; the caller must reject,
    // never assume mature). Caller must hold cs_main.
    BlockIndexAuthoritativeMaturityStatus ResolveSpendMaturity(
        const uint256& startHash, unsigned int srcFile, unsigned int srcBlockPos,
        int maxDepth, int* outDepth, std::string* error) const;

    // ---- live acceptance / persistence ----
    // Persist an accepted ACTIVE block (already consensus-validated by the live
    // engine) into the mutable tip + refresh the bounded live tail. activeHeight
    // is the block's global active height (must equal tip height + 1).
    bool AcceptActive(const BlockIndexRecord& rec,
                      const BlockIndexDerivedEntry& derived,
                      int activeHeight, std::string* error);

    // Persist an accepted SIDE (non-active) block (validated but not best chain).
    bool AcceptSide(const BlockIndexRecord& rec,
                    const BlockIndexDerivedEntry& derived,
                    std::string* error);

    // Record an accepted block whose parent is unknown (orphan) into the tip as a
    // side record (persisted for a later join), mirroring legacy mapOrphanBlocks.
    bool RecordOrphan(const BlockIndexRecord& rec,
                      const BlockIndexDerivedEntry& derived,
                      std::string* error);

    // Reorg the ACTIVE chain to forkHeight (disconnect the branch above it), then
    // connect newBranch (in order, with global active heights) as the active chain.
    // Side records are retained; fail-closed on any partial application (the tip is
    // left at the truncate point, recoverable by re-running). The caller has already
    // validated + chosen the new branch as best.
    bool ReorgTo(int32_t forkHeight,
                 const std::vector<BlockIndexRecord>& newBranchRecs,
                 const std::vector<BlockIndexDerivedEntry>& newBranchDerived,
                 const std::vector<int32_t>& newHeights,
                 std::string* error);

    // G1-A: resolve and RETAIN the full-topology parent chain for `parentHash`
    // in a PERSISTENT residency store that survives the single-block-operation
    // lifetime. AddToBlockIndex uses this when the parent is V2-authoritative
    // and non-resident in mapBlockIndex, so pindexNew->pprev points at a
    // resident object that stays valid after ProcessBlock returns (no dangling
    // pointer). The base tip S and the current accepted tip are anchored
    // (non-evictable); deep/branch ancestry is evictable by the live-tail
    // horizon (never a consensus/reorg bound).
    //
    // LIFETIME INVARIANT: any resident CBlockIndex whose pprev points to a
    // materialized authoritative ancestor must never outlive that ancestor's
    // safe residency/topology lifetime. This holds because the materialized
    // chain is owned by `fullResident_` (anchor + horizon policy), and is NOT
    // freed by ReleaseOperationMaterializations().
    CBlockIndex* ResolveAndRetainFullParent(const uint256& parentHash,
                                            std::string* error);

    // Release all operation-scoped parent materializations. Called at the END of a
    // single logical block acceptance (authoritative mode) so the residency of
    // materialized full-topology parents stays bounded to ONE block's worth of
    // ancestors (not O(history)). The PERSISTENT full-topology store
    // (ResolveAndRetainFullParent) is NOT freed here — its residency is governed
    // by anchor/horizon policy so already-accepted blocks' pprev stays valid.
    void ReleaseOperationMaterializations();

    // ---- introspection ----
    const BlockIndexTipAuthority* TipAuthority() const;
    BlockIndexTipAuthority* TipAuthorityMutable();
    const BlockIndexLiveTail& Tail() const;
    int Horizon() const;
    uint64_t BaseGeneration() const;
    bool IsOpen() const;

    void Close();

public:
    // PIMPL state; public so the base-known callback (void*) can reach internals.
    class Impl;

private:
    Impl* impl_;
    // baseKnown callback state owner (Impl is the ud payload).
    friend bool BlockIndexAuthoritativeLiveBaseKnown(const uint256& hash, void* ud);
};

// R2c.2/S6-repair: RAII ownership token for a bounded, authoritative,
// full-topology parent materialization. Narrowly scoped variant of
// MaterializeParentChain for consumers that must not depend on the
// operation-global store (ReleaseOperationMaterializations at the end of the
// next ProcessBlock) and must release the reconstructed objects
// deterministically at their own scope exit.
//
//   Acquire(hash)  -> bounded by-value walk (same policy as
//                     MaterializeParentChain: tip-then-base, floor = base tip
//                     height - WALK); objects owned by this token.
//   Parent()       -> the requested parent CBlockIndex* (valid while the token
//                     is alive and not re-Acquired).
//   Release()/dtor -> frees every reconstructed object. The pointer must NOT
//                     be used after Release and must NOT escape the token's
//                     scope (no async retention).
//
// The winner identity is decided by the selector's value result; this token
// only provides a temporary usable parent REPRESENTATION for the legacy
// consensus/block-build code. Caller must hold cs_main for Acquire.
class ScopedMaterializedChain
{
public:
    ScopedMaterializedChain() : parent_(NULL) {}
    ~ScopedMaterializedChain() { Release(); }

    // Fail-closed: NULL + error on authority/materialization failure. Never
    // falls back to legacy/resident authority.
    CBlockIndex* Acquire(BlockIndexAuthoritativeLive* live,
                         const uint256& hash, std::string* error);

    void Release();

    CBlockIndex* Parent() const { return parent_; }
    bool IsValid() const { return parent_ != NULL; }

private:
    ScopedMaterializedChain(const ScopedMaterializedChain&);
    ScopedMaterializedChain& operator=(const ScopedMaterializedChain&);

    std::vector<CBlockIndex*> owned_;
    std::vector<uint256*>     ownedHashes_;
    CBlockIndex*              parent_;
};

// Production accessor (NULL when NOT in authoritative mode). The object is
// retained process-lifetime by the authoritative startup context and bound to
// the single process-open base reader. Callers must NOT free it.
BlockIndexAuthoritativeLive* GetAuthoritativeLiveAuthority();

#endif // INNOVA_BLOCKINDEX_AUTHORITATIVE_LIVE_H