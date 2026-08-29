#ifndef INNOVA_BLOCKINDEX_ACCESSOR_H
#define INNOVA_BLOCKINDEX_ACCESSOR_H

#include "main.h"

#include <map>
#include <vector>

// Phase A contract:
// - backend-neutral, read-only accessor surface
// - public API exposes NO CBlockIndex*
// - caller MUST hold cs_main for every call into the legacy adapter
// - returned snapshots are by-value and remain valid after lock release
// - IDs are stable for the process lifetime, private to the adapter implementation, and not persisted

typedef uint64_t BlockIndexId;
static const BlockIndexId BLOCK_INDEX_ID_INVALID = 0;

struct BlockIndexSnapshot
{
    bool found;
    BlockIndexId id;
    BlockIndexId parentId;
    bool hasParent;

    uint256 hash;
    uint256 hashPrev;
    uint256 hashNext;

    int height;
    unsigned int nFile;
    unsigned int nBlockPos;
    unsigned int nFlags;
    int nVersion;
    unsigned int nTime;
    unsigned int nBits;
    unsigned int nNonce;
    int64_t nMint;
    int64_t nMoneySupply;
    uint64_t nStakeModifier;
    COutPoint prevoutStake;
    unsigned int nStakeTime;
    uint256 hashProof;
    uint256 nChainTrust;
    bool fProofOfStake;
    bool fInMainChain;
    uint64_t nStakeModifierTime;    // 0 = unset; O(1) modifier time cache
    unsigned int nStakeModifierChecksum;

    BlockIndexSnapshot()
        : found(false),
          id(BLOCK_INDEX_ID_INVALID),
          parentId(BLOCK_INDEX_ID_INVALID),
          hasParent(false),
          hash(0),
          hashPrev(0),
          hashNext(0),
          height(0),
          nFile(0),
          nBlockPos(0),
          nFlags(0),
          nVersion(0),
          nTime(0),
          nBits(0),
          nNonce(0),
          nMint(0),
          nMoneySupply(0),
          nStakeModifier(0),
          prevoutStake(),
          nStakeTime(0),
          hashProof(0),
          nChainTrust(0),
          fProofOfStake(false),
          fInMainChain(false),
          nStakeModifierTime(0),
          nStakeModifierChecksum(0)
    {
    }
};

class BlockIndexAccessor
{
public:
    virtual ~BlockIndexAccessor() {}

    virtual bool Contains(uint256 hash) const = 0;
    virtual BlockIndexSnapshot LookupByHash(uint256 hash) const = 0;
    virtual BlockIndexSnapshot ReadSnapshot(BlockIndexId id) const = 0;
    virtual BlockIndexSnapshot GetParent(BlockIndexId id) const = 0;
    virtual BlockIndexSnapshot GetAncestor(BlockIndexId id, int targetHeight) const = 0;
    virtual BlockIndexSnapshot GetActiveByHeight(int height) const = 0;
    virtual BlockIndexSnapshot GetNextActive(BlockIndexId id) const = 0;
    virtual BlockIndexSnapshot GetTip() const = 0;
    virtual BlockIndexSnapshot FindFork(BlockIndexId a, BlockIndexId b) const = 0;
};

class LegacyBlockIndexAccessor : public BlockIndexAccessor
{
public:
    LegacyBlockIndexAccessor();

    virtual bool Contains(uint256 hash) const;
    virtual BlockIndexSnapshot LookupByHash(uint256 hash) const;
    virtual BlockIndexSnapshot ReadSnapshot(BlockIndexId id) const;
    virtual BlockIndexSnapshot GetParent(BlockIndexId id) const;
    virtual BlockIndexSnapshot GetAncestor(BlockIndexId id, int targetHeight) const;
    virtual BlockIndexSnapshot GetActiveByHeight(int height) const;
    virtual BlockIndexSnapshot GetNextActive(BlockIndexId id) const;
    virtual BlockIndexSnapshot GetTip() const;
    virtual BlockIndexSnapshot FindFork(BlockIndexId a, BlockIndexId b) const;

private:
    BlockIndexSnapshot SnapshotFromIndex(const CBlockIndex* pindex) const;
    BlockIndexId GetOrCreateIdLocked(const CBlockIndex* pindex) const;
    const CBlockIndex* ResolveIdLocked(BlockIndexId id) const;
    static const CBlockIndex* FindForkLocked(const CBlockIndex* a, const CBlockIndex* b);
};

#endif
