// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Stage C — production Block Index Manager.
//
// The manager is the single by-value read contract between the authoritative
// V2 history (immutable generations + mutable tip) and consumers. It exposes
// STABLE HASH IDENTITY (BlockIndexLogicalId) and BY-VALUE snapshots
// (BlockIndexSnapshot). Its public API contains NO CBlockIndex* and stores NO
// per-block identity registry of its own: it is a stateless façade over the
// existing authoritative resolution seams and/or a bound BlockIndexV2Reader.
//
// Backends:
//   * COMPOSITE (production): when the authoritative by-value startup is active
//     (g_fAuthoritativeStartup), hash/height resolution is delegated to the
//     established composite seam (tip tail first, then immutable base):
//     ResolveAuthoritativeBlockSnapshotR / AuthoritativeGetActiveSnapshotByHeight.
//     This matches exactly what the existing authoritative RPC branches use, so
//     parity is guaranteed by construction.
//   * READER (offline/test): when no authoritative mode is active but a reader
//     is bound, all reads go to the bound BlockIndexV2Reader.
//
// Fail-closed: a genuine absence is NOT_FOUND; an authority that cannot answer
// (corrupt / stale generation / io) is AUTHORITY_FAILURE and MUST NOT be treated
// as absence. A cache miss is never interpreted as block absence.

#ifndef INNOVA_BLOCKINDEX_MANAGER_H
#define INNOVA_BLOCKINDEX_MANAGER_H

#include "blockindex_accessor.h"       // BlockIndexSnapshot, BlockIndexId
#include "blockindex_navigation.h"     // BlockIndexLogicalId

#include <stdint.h>
#include <string>
#include <vector>

class BlockIndexV2Reader;

enum BlockIndexManagerStatus
{
    BLOCK_INDEX_MANAGER_OK = 0,
    // Genuine, domain-appropriate absence (a block that may legitimately be in
    // another domain). Safe to treat as "not present".
    BLOCK_INDEX_MANAGER_NOT_FOUND,
    // Present but not on the active chain (side / reorged / stale).
    BLOCK_INDEX_MANAGER_NOT_ACTIVE,
    // The manager has no backing authority (not authoritative, no reader bound).
    BLOCK_INDEX_MANAGER_NOT_OPEN,
    // The requested capability is not provided for this generation (e.g. a
    // derived consensus field on a non-derived-capable generation). Fail closed.
    BLOCK_INDEX_MANAGER_UNSUPPORTED,
    // The authority could not answer (corrupt record / stale generation /
    // divergent seam / io). MUST fail closed; never a legacy fallback.
    BLOCK_INDEX_MANAGER_AUTHORITY_FAILURE,
};

const char* BlockIndexManagerStatusName(BlockIndexManagerStatus status);

// By-value derived consensus fields. Every value is consumable ONLY when its
// has* flag is true; a zero value never means "available", and a missing value
// is NEVER silently substituted with zero.
struct BlockIndexDerivedConsensus
{
    uint256 chainTrust;                  bool hasChainTrust;
    unsigned int stakeModifierChecksum;  bool hasStakeModifierChecksum;
    int64_t stakeModifierTime;           bool hasStakeModifierTime;
    unsigned int blockSize;              bool hasBlockSize;

    BlockIndexDerivedConsensus()
        : chainTrust(0), hasChainTrust(false),
          stakeModifierChecksum(0), hasStakeModifierChecksum(false),
          stakeModifierTime(0), hasStakeModifierTime(false),
          blockSize(0), hasBlockSize(false) {}
};

class BlockIndexManager
{
public:
    BlockIndexManager();
    ~BlockIndexManager();
    BlockIndexManager(const BlockIndexManager&) = delete;
    BlockIndexManager& operator=(const BlockIndexManager&) = delete;

    // Non-owning bind of an OPEN immutable generation reader (must outlive this
    // manager). Used for the offline/test backend and, in production, as the
    // identity/generation/count source while history resolves through the
    // composite seam. NULL unbinds.
    void BindReader(const BlockIndexV2Reader* reader);
    bool IsReaderBound() const { return reader_ != NULL; }
    // The currently bound non-owning generation reader (NULL when unbound). Lets a
    // hermetic fixture save/restore the process singleton's binding safely.
    const BlockIndexV2Reader* BoundReader() const { return reader_; }

    // True when the manager can serve real reads. Sets *error otherwise.
    bool IsAvailable(std::string* error) const;

    uint64_t Generation() const;
    // Total committed records of the selected generation (0 if unknown).
    uint64_t TotalRecordCount() const;

    // ---- by-value historical reads (public API contains no CBlockIndex*) ----
    BlockIndexManagerStatus LookupByHash(const uint256& hash,
                                         BlockIndexSnapshot* out, std::string* error) const;
    BlockIndexManagerStatus GetActiveByHeight(int height,
                                              BlockIndexSnapshot* out, std::string* error) const;
    BlockIndexManagerStatus GetParent(const uint256& hash,
                                      BlockIndexSnapshot* out, std::string* error) const;
    BlockIndexManagerStatus GetParent(const BlockIndexLogicalId& id,
                                      BlockIndexSnapshot* out, std::string* error) const;
    BlockIndexManagerStatus GetAncestor(const uint256& hash, int targetHeight,
                                        BlockIndexSnapshot* out, std::string* error) const;
    BlockIndexManagerStatus GetAncestor(const BlockIndexLogicalId& id, int targetHeight,
                                        BlockIndexSnapshot* out, std::string* error) const;
    BlockIndexManagerStatus GetTip(BlockIndexSnapshot* out, std::string* error) const;
    BlockIndexManagerStatus BestTipHash(uint256* out, std::string* error) const;
    BlockIndexManagerStatus ActiveTipHeight(int* out, std::string* error) const;
    BlockIndexManagerStatus Contains(const uint256& hash, bool* out, std::string* error) const;
    BlockIndexManagerStatus TipIs(const uint256& hash, bool* out, std::string* error) const;

    // By-value equivalent of the legacy GetLastBlockIndex(): the nearest block at
    // or before `fromHash` (following parents) whose proof type equals
    // fProofOfStake, stopping at genesis. Walks parents BY VALUE (bounded by
    // depth; no resident pointer graph, no CBlockIndex). Used to serve the
    // difficulty read group from authoritative history.
    BlockIndexManagerStatus GetLastBlockIndexByProofType(const uint256& fromHash,
                                                         bool fProofOfStake,
                                                         BlockIndexSnapshot* out,
                                                         std::string* error) const;

    // Hash-native getblocks locator (Stage F, L6c). Builds the locator vHave list
    // from a begin HASH by walking parents BY VALUE — the by-value equivalent of
    // the legacy CBlockLocator(const CBlockIndex*): active-chain membership, the
    // 10-then-doubling ancestor step, and a trailing genesis hash. No pointer
    // reconstruction and no mapBlockIndex. uint256(0) reproduces the retired
    // NULL begin ([genesis]). If the begin's ancestry cannot be resolved the
    // legacy walk stopped early and still appended genesis — replicated here.
    // Used to construct getblocks locators without any CBlockIndex*.
    BlockIndexManagerStatus GetLocatorHashes(const uint256& hashBegin,
                                             std::vector<uint256>* out,
                                             std::string* error) const;

    // Strict derived-consensus access. Fails closed (UNSUPPORTED/AUTHORITY_FAILURE)
    // rather than returning a zero-substituted value when the generation cannot
    // authoritatively provide the field.
    BlockIndexManagerStatus GetDerivedConsensus(const uint256& hash,
                                                BlockIndexDerivedConsensus* out,
                                                std::string* error) const;

    // ---- consistent read view ----
    // COHERENCE GUARD, NOT AN ATOMIC SNAPSHOT. A View binds reads to a captured
    // (generation, active-tip hash, active-tip height). It does NOT freeze the
    // store or materialize a point-in-time copy; it only lets a caller DETECT
    // that the generation or the active tip moved. Callers that need a coherent
    // multi-read result must CheckCoherent() after their reads and re-pin on
    // AUTHORITY_FAILURE (the manager never mixes an old and a new chain view
    // silently). The underlying durability guarantees still apply: a by-value
    // snapshot stays valid after lock release, and a reader that is closed or a
    // generation that is superseded makes resolution fail closed rather than
    // return a stale reference into evicted memory.
    class View
    {
    public:
        View() : mgr_(NULL), generation_(0), tipHash_(0), tipHeight_(-1) {}
        bool IsValid() const { return mgr_ != NULL; }
        uint64_t Generation() const { return generation_; }
        const uint256& TipHash() const { return tipHash_; }
        int TipHeight() const { return tipHeight_; }
        BlockIndexManagerStatus CheckCoherent(std::string* error) const;

    private:
        friend class BlockIndexManager;
        const BlockIndexManager* mgr_;
        uint64_t generation_;
        uint256 tipHash_;
        int tipHeight_;
    };

    // Capture the current (generation, active-tip). Fails closed when the
    // manager cannot establish a coherent view.
    View MakeView(std::string* error) const;

private:
    // True when resolution should go through the authoritative composite seam.
    bool UseComposite() const;

    const BlockIndexV2Reader* reader_;
};

// Process-lifetime manager façade. Holds NO per-block identity (no registry);
// it is bound (lazily) to the retained authoritative base reader as its
// generation/count source. Safe to call at any time.
BlockIndexManager& GetBlockIndexManager();

#endif // INNOVA_BLOCKINDEX_MANAGER_H
