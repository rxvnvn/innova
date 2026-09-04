// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef INNOVA_BLOCKINDEX_HOT_OWNER_H
#define INNOVA_BLOCKINDEX_HOT_OWNER_H

#include "blockindex_navigation.h"
#include "blockindex_accessor.h"
#include "main.h"
#include "sync.h"

#include <cstddef>
#include <map>
#include <vector>
#include <memory>

// ---------------------------------------------------------------------------
// A.10.1d: BlockIndexHotOwner -- ownership/pinning contract that decouples
// MATERIALIZATION from RESIDENCY LIFETIME and both from AUTHORITY.
//
//   AUTHORITY       : what is logically true (hash/trust/active/ancestry) --
//                     by-value via StartupAuthority / V2 reader / frontier.
//   MATERIALIZATION : how to obtain an in-memory CBlockIndex (layer-1 metadata
//                     from V2 source). Does NOT decide how long it stays alive.
//                     The materializer interface (BlockIndexHotMaterializer) is
//                     STATELESS; it holds no resident object cache.
//   RESIDENCY       : how long a materialized CBlockIndex stays in RAM -- owned
//                     ONLY by BlockIndexHotOwner, governed by pin lifetime.
//                     HotOwner maintains a resident cache (std::map<uint256,Entry>).
//
// mapBlockIndex remains the authoritative/fully-resident legacy owner. The
// HotOwner is ADDITIVE machinery (used by tests + future consumers); it does
// NOT delete, evict, or cut over any legacy object.
//
// Identity is the stable logical hash (BlockIndexLogicalId), independent of
// any std::map node address. phashBlock is owner-owned (see Handle::Get).
// ---------------------------------------------------------------------------

/** Typed results (authority vs materialization are deliberately separate). */
enum class BlockIndexHotStatus
{
    OK = 0,
    NOT_RESIDENT,              // logical id present in authority but not materialized
    MATERIALIZATION_PENDING,   // request in flight (owner state machine)
    MATERIALIZATION_UNAVAILABLE, // source cannot provide metadata (corrupt/absent)
    AUTHORITY_MISSING,         // logical id not found in authority at all
    GENERATION_MISMATCH,       // materialization/gen tag does not match owner's CURRENT
    CORRUPT_METADATA,          // derived chainTrust/size/etc unavailable or hash mismatch
    EVICTION_BLOCKED,          // attempted eviction of a pinned/anchor object
    PIN_REQUIRED               // a raw pointer was requested without a lifetime claim
};

/** By-value materialization payload (layer-1 index metadata). No block bytes. */
struct BlockIndexHotMaterialized
{
    bool            found;
    uint64_t        generation;   // the generation that produced this materialization
    BlockIndexSnapshot snapshot;  // full by-value index metadata (hash/trust/height/...)

    bool            hasBlockSize;
    unsigned int    blockSize;

    BlockIndexHotMaterialized() : found(false), generation(0), hasBlockSize(false), blockSize(0) {}
};

/**
 * Materialization token (A.10.1d two-phase commit). Created by
 * RequestMaterialization under the owner lock (IDLE -> PENDING) and returned
 * to the caller. The actual materializer I/O runs OUTSIDE the lock; the token
 * is later validated (hash + sequence + generation) and published under the
 * lock. Uniquely tags one in-flight request so a stale/foreign completion can
 * never publish.
 */
struct BlockIndexHotToken
{
    uint256 hash;
    uint64_t generation;   // owner generation at request time
    uint64_t seq;          // monotonic per-owner request id

    BlockIndexHotToken() : hash(0), generation(0), seq(0) {}
    bool IsValid() const { return seq != 0; }
};

/**
 * Materializer seam (A.10.1d): resolves a logical hash into by-value index
 * metadata. SYNCHRONOUS today. A future async provider maps to the same
 * interface by returning MATERIALIZATION_PENDING and later publishing the
 * result through the owner's state machine -- ownership/authority semantics do
 * not change. Deterministic state machine is the only required transport.
 */
class BlockIndexHotMaterializer
{
public:
    virtual ~BlockIndexHotMaterializer() {}
    /** Resolve logical id -> materialized metadata. Status is the typed result;
     *  *out.found set on OK. Never throws. */
    virtual BlockIndexHotStatus Materialize(const BlockIndexLogicalId& id,
                                            BlockIndexHotMaterialized* out) const = 0;
};

class BlockIndexHotOwner; // fwd for handle

/**
 * Move-only RAII lifetime claim on one resident hot object. While a handle
 * exists, the underlying CBlockIndex* (returned by Get()) is guaranteed valid.
 * Copy is disabled; move transfers the claim. Release on destruction.
 */
class BlockIndexHotHandle
{
public:
    BlockIndexHotHandle() : owner(NULL), hash(0) {}
    ~BlockIndexHotHandle();
    BlockIndexHotHandle(BlockIndexHotHandle&& o) noexcept;
    BlockIndexHotHandle& operator=(BlockIndexHotHandle&& o) noexcept;
    BlockIndexHotHandle(const BlockIndexHotHandle&) = delete;
    BlockIndexHotHandle& operator=(const BlockIndexHotHandle&) = delete;

    bool IsValid() const { return owner != NULL; }
    const uint256& Hash() const { return hash; }
    /** Valid only while IsValid(); ownership-claim guarantees lifetime. */
    CBlockIndex* Get() const;
    /** Explicit release; idempotent. */
    void Reset();

private:
    friend class BlockIndexHotOwner;
    BlockIndexHotOwner* owner;
    uint256 hash;
};

/** Metrics (idempotent atomics for the future real-datadir experiment). */
struct BlockIndexHotMetrics
{
    int64_t residentCount;        // current resident objects
    int64_t pinnedCount;          // current active pin claims
    int64_t evictableCount;       // resident objects currently eviction-eligible
    int64_t materializationRequests;
    int64_t materializationHits;  // already resident
    int64_t materializationMisses;// required a materialization
    int64_t inFlightCount;        // PENDING
    int64_t materializationFailures;
    int64_t evictions;
    int64_t evictionBlocked;
    int64_t peakResidentCount;

    BlockIndexHotMetrics()
        : residentCount(0), pinnedCount(0), evictableCount(0),
          materializationRequests(0), materializationHits(0), materializationMisses(0),
          inFlightCount(0), materializationFailures(0), evictions(0),
          evictionBlocked(0), peakResidentCount(0) {}
    BlockIndexHotMetrics& operator+=(const BlockIndexHotMetrics& o);
    BlockIndexHotMetrics operator+(const BlockIndexHotMetrics& o) const;
};

/**
 * Owner of evictable CBlockIndex objects keyed by stable logical hash.
 *
 * Thread-safe via one internal LEAF lock (cc `cs`). The lock is never acquired
 * while cs_main/cs_wallet/cs_spvutxos are held by the caller IN REVERSE order,
 * and the owner NEVER takes cs_main: it is a leaf, so it cannot create a lock
 * cycle. The internal lock is held only for a *finite* state transition -- the
 * materializer I/O explicitly runs OUTSIDE `cs` (two-phase split-lock, A.10.1d).
 */
class BlockIndexHotOwner
{
public:
    BlockIndexHotOwner();
    ~BlockIndexHotOwner();

    /** Install a materializer (non-owning; must outlive this owner). */
    void SetMaterializer(const BlockIndexHotMaterializer* m);

    // --- identity / lookup (by value) ---
    bool IsResident(const BlockIndexLogicalId& id) const;
    bool IsPinned(const BlockIndexLogicalId& id) const;

    /** Replace the owner's current generation tag (for GENERATION_MISMATCH). */
    void SetCurrentGeneration(uint64_t gen);
    uint64_t CurrentGeneration() const;

    // --- materialization (two-phase: request under lock, materialize outside,
    //     publish under lock) ---
    /** Phase 1 (under lock): IDLE->PENDING for a logical block and mint a token.
     *  READY if already resident (token valid but unused). Returns the token via
     *  the out-param in every case. */
    BlockIndexHotStatus RequestMaterialization(const BlockIndexLogicalId& id,
                                               BlockIndexHotToken* token);
    /** Phase 3 (under lock): validate the token (hash+seq+generation still
     *  current) and atomically publish an already-materialized object. */
    BlockIndexHotStatus PublishMaterialized(const BlockIndexHotToken& token,
                                            const BlockIndexHotMaterialized& m);
    /** Convenience synchronous driver (checkpoint): request -> materialize
     *  OUTSIDE the lock -> publish under lock -> done. */
    BlockIndexHotStatus EnsureResident(const BlockIndexLogicalId& id);
    /** Full synchronous pin: request + materialize-out-of-lock + publish +
     *  acquire a lifetime claim. */
    BlockIndexHotStatus Pin(const BlockIndexLogicalId& id, BlockIndexHotHandle* out);
    /** Release one pin claim. Returns the updated pin count. */
    int ReleasePin(const uint256& hash);
    BlockIndexHotStatus LookupResident(const BlockIndexLogicalId& id,
                                       BlockIndexHotHandle* out);

    // --- eviction eligibility (deterministic; policy deferred) ---
    /** List of currently eviction-eligible resident ids (not pinned, not anchor).
     *  Order is unspecified; never a fixed height window. */
    std::vector<BlockIndexLogicalId> EvictEligible() const;
    /** Attempt to evict one resident object. EVICTION_BLOCKED if pinned/anchor. */
    BlockIndexHotStatus EvictResident(const BlockIndexLogicalId& id);

    // --- counts / metrics ---
    size_t ResidentCount() const;
    size_t PinCount() const;
    BlockIndexHotMetrics Metrics() const;

    /** Mark a logical hash as a required anchor (never evictable). */
    void PinPermanent(const BlockIndexLogicalId& id);

    /** Raw resident object for a given logical hash (valid only while a handle
     *  guarantees lifetime; NULL if absent/not materialized). */
    CBlockIndex* GetResidentRaw(const uint256& hash) const;

private:
    struct Entry
    {
        BlockIndexLogicalId id;
        uint256 ownHash;                 // owner-owned identity (phashBlock target)
        CBlockIndex*         index;      // materialized hot object (owner-owned)
        int                  pins;
        uint64_t             residentGen;
        bool                 materialized;
        bool                 anchor;     // permanent (not evictable)
        bool                 pending;    // materialization in flight (PENDING)
        uint64_t             seq;        // token sequence for in-flight request
        Entry() : index(NULL), pins(0), residentGen(0), materialized(false),
                  anchor(false), pending(false), seq(0) {}
    };

    const BlockIndexHotMaterializer* materializer;
    std::map<uint256, Entry> resident;   // keyed by logical hash
    uint64_t currentGeneration;
    uint64_t nextTokenSeq;
    BlockIndexHotMetrics metrics;
    mutable CCriticalSection cs;         // LEAF lock: never acquires cs_main etc.

    Entry* EntryLocked(uint256 hash);                       // requires cs (recursive)
    BlockIndexHotStatus RequestMaterializationLocked(const BlockIndexLogicalId& id,
                                                     BlockIndexHotToken* token); // requires cs
    BlockIndexHotStatus PublishMaterializedLocked(const BlockIndexHotToken& token,
                                                  const BlockIndexHotMaterialized& m); // requires cs
};

#endif // INNOVA_BLOCKINDEX_HOT_OWNER_H