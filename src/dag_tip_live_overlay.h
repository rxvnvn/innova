// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.
//
// A.13.6-R2c.1b — bounded live DAG tip frontier overlay (correction).
//
// Replaces the previous unbounded resident delta-state implementation. The
// authoritative logical override state lives in a persistent, generation-bound,
// hash-keyed LevelDB store on disk; a RAM cache is only an accelerator and is
// strictly capped. Eviction removes only cache residency, never logical state.
//
// Composition is:
//
//     frontier = immutable seed tips overridden by persistent hash override
//     F(hash) = overlay override (PRESENT/ABSENT) if one exists,
//               else immutable seed membership
//
// AddTip(hash)  -> overlay override PRESENT
// RemoveTip(hash)-> overlay override ABSENT
//
// RESIDENCY CAPACITY != LOGICAL CAPACITY.
// The persistent on-disk representation is authority. Logical delta cardinality
// may grow unboundedly on disk while RAM stays bounded by the enforced cache
// capacity. No consensus/live-history limit is introduced.

#ifndef INNOVA_DAG_TIP_LIVE_OVERLAY_H
#define INNOVA_DAG_TIP_LIVE_OVERLAY_H

#include <stdint.h>
#include <cstring>
#include <string>
#include <vector>

#include "uint256.h"

namespace dag_tip_frontier {

class TipFrontierReader;

// Narrow default-off test seams; no configuration/RPC path.
extern bool g_testFailLiveTipOverlayOverrideWrite;
extern bool g_testFailLiveTipOverlayCheckpointWrite;

enum LiveOverlayPhase
{
    LIVE_OVERLAY_PHASE_UNAVAILABLE = 0,
    LIVE_OVERLAY_PHASE_APPLYING,
    LIVE_OVERLAY_PHASE_CLEAN
};

// Versioned, value-only derived-state checkpoint. dagInputDigest and
// frontierDigest bind the immutable seed artifact; appliedSourceStateId names
// the mutable recovered daglinks relation. This is never source authority.
// Version 2 certifies source-semantic delivery. Version 1 may contain a
// legacy-residency false CLEAN and must take bounded recovery even at equal T.
static const uint32_t LIVE_OVERLAY_CHECKPOINT_VERSION = 2;
struct LiveTipOverlayCheckpoint
{
    uint32_t version;
    uint64_t generationId;
    unsigned char dagInputDigest[32];
    unsigned char frontierDigest[32];
    LiveOverlayPhase phase;
    uint256 appliedSourceStateId;
    LiveTipOverlayCheckpoint() : version(LIVE_OVERLAY_CHECKPOINT_VERSION), generationId(0), phase(LIVE_OVERLAY_PHASE_UNAVAILABLE), appliedSourceStateId(0)
    {
        memset(dagInputDigest, 0, sizeof(dagInputDigest));
        memset(frontierDigest, 0, sizeof(frontierDigest));
    }
};

enum LiveOverlayStatus
{
    LIVE_OVERLAY_OK = 0,
    LIVE_OVERLAY_NOT_FOUND,
    LIVE_OVERLAY_GENERATION_MISMATCH,
    LIVE_OVERLAY_CORRUPT,
    LIVE_OVERLAY_AUTHORITY_FAILURE
};

// Persistent, generation-bound, hash-keyed override store (LevelDB).
//   key   = CDataStream(SER_DISK) << make_pair("dagtipovr", hash)
//   value = 1 byte override state: 1 = PRESENT, 0 = ABSENT
// plus a generation meta record bound to the same generation.
//
// The store is the authoritative logical overlay state. It is NOT an
// append-only log that must be replayed to memory: lookups are direct keyed
// reads and iteration is a bounded LevelDB cursor. Logical cardinality on disk
// may exceed any RAM cache capacity.
class LiveTipOverlayStore
{
public:
    LiveTipOverlayStore();
    ~LiveTipOverlayStore();
    // Open/verify a generation-bound store at dbDir. Fails closed on generation
    // mismatch, missing/corrupt meta, or unreadable store.
    bool Open(const std::string& dbDir, uint64_t expectGeneration, std::string* error);
    void Close();
    bool IsOpen() const;
    uint64_t Generation() const;
    uint64_t EntryCount() const; // on-disk override count (bounded query effort optional)
    // Set the current override state for a hash (PRESENT/ABSENT). Idempotent.
    bool SetState(const uint256& hash, bool present, std::string* error);
    // Exact direct lookup: returns true and sets present when an override exists;
    // returns with *hasOverride=false if the hash has never been touched.
    bool GetState(const uint256& hash, bool* hasOverride, bool* present, std::string* error) const;
    // Streaming override iteration (hash, present). Never materializes the set.
    typedef void (*ForEachOverrideFn)(const uint256&, bool present, void* ctx);
    bool ForEachOverride(ForEachOverrideFn fn, void* ctx, std::string* error) const;
    // Removes every persisted override in bounded disk-backed batches. Used only
    // by a rebuild already durably marked APPLYING; never infer health from it.
    bool ClearOverrides(std::string* error);
    // Checkpoint is distinct from the legacy generation marker. Missing is
    // explicitly unavailable; callers may establish CLEAN only after proof.
    bool ReadCheckpoint(LiveTipOverlayCheckpoint* out, std::string* error) const;
    bool WriteCheckpoint(const LiveTipOverlayCheckpoint& checkpoint, std::string* error);
private:
    struct Impl;
    Impl* impl_;
};

// Bounded-RAM overlay view. Composes immutable seed + persistent overrides with
// an enforced LRU cache. Correctness never depends on a hash being cached.
class LiveTipFrontierOverlay
{
public:
    LiveTipFrontierOverlay();
    ~LiveTipFrontierOverlay();
    // Open seed reader + persistent override store for the same generation.
    // cacheCapacity is a strict RAM bound on resident override entries
    // (0 == effectively nil cache, always logically correct).
    bool Open(const std::string& artifactPath,
              const std::string& overlayDbDir,
              uint64_t generation,
              unsigned char dagInputDigest[32],
              size_t cacheCapacity,
              std::string* error);
    void Close();
    bool IsOpen() const;
    uint64_t Generation() const;
    // Logical live mutations (persistent; exact regardless of cache residency).
    bool AddTip(const uint256& hash, std::string* error);
    bool RemoveTip(const uint256& hash, std::string* error);
    // Exact streaming frontier iteration (seed overridden by persistent overrides).
    typedef bool (*ForEachFn)(const uint256&, void* ctx);
    bool ForEachTip(ForEachFn fn, void* ctx, std::string* error) const;
    bool Contains(const uint256& hash, bool* out, std::string* error) const;
    // Immutable-seed membership (used by the streaming emit trampoline).
    bool HasSeedMembership(const uint256& hash) const;
    // Logical composed tip count (streams iteration).
    bool GetComposedTipCount(uint64_t* out, std::string* error) const;
    // Residency metrics.
    size_t CacheCapacity() const;
    size_t CacheCurrent() const;
    size_t CachePeak() const;
    uint64_t PersistentOverrideCount() const;
    // Missing/corrupt checkpoint is unavailable; this method never treats a
    // generation marker or override contents as checkpoint proof.
    bool ReadCheckpoint(LiveTipOverlayCheckpoint* out, std::string* error) const;
    bool WriteCheckpoint(const LiveTipOverlayCheckpoint& checkpoint, std::string* error);
    // Rebuild-only APIs: reset persistent override state in bounded batches and
    // obtain a separately validated immutable seed cursor for a streaming diff.
    bool ClearPersistentOverrides(std::string* error);
    bool OpenImmutableSeedReader(TipFrontierReader* out, std::string* error) const;
    // Constructs a checkpoint bound to this already-validated immutable seed.
    bool MakeCheckpoint(LiveOverlayPhase phase, const uint256& token,
                        LiveTipOverlayCheckpoint* out, std::string* error) const;
    bool IsImmutableBindingValid(const LiveTipOverlayCheckpoint& checkpoint) const;
private:
    struct Impl;
    Impl* impl_;
};

} // namespace dag_tip_frontier

#endif // INNOVA_DAG_TIP_LIVE_OVERLAY_H