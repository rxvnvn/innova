#include "blockindex_accessor.h"

namespace {
static std::map<uint256, BlockIndexId>& BlockIndexAccessorIdByHash()
{
    static std::map<uint256, BlockIndexId> s_idByHash;
    return s_idByHash;
}

static std::vector<const CBlockIndex*>& BlockIndexAccessorIndexById()
{
    static std::vector<const CBlockIndex*> s_indexById(1, NULL);
    return s_indexById;
}

static BlockIndexId& BlockIndexAccessorNextId()
{
    static BlockIndexId s_nextId = 1;
    return s_nextId;
}
}

LegacyBlockIndexAccessor::LegacyBlockIndexAccessor()
{
}

bool LegacyBlockIndexAccessor::Contains(uint256 hash) const
{
    AssertLockHeld(cs_main);
    return mapBlockIndex.count(hash) != 0;
}

BlockIndexId LegacyBlockIndexAccessor::GetOrCreateIdLocked(const CBlockIndex* pindex) const
{
    AssertLockHeld(cs_main);
    if (pindex == NULL)
        return BLOCK_INDEX_ID_INVALID;

    const uint256 hash = pindex->GetBlockHash();
    std::map<uint256, BlockIndexId>& idByHash = BlockIndexAccessorIdByHash();
    std::vector<const CBlockIndex*>& indexById = BlockIndexAccessorIndexById();
    BlockIndexId& nextId = BlockIndexAccessorNextId();

    std::map<uint256, BlockIndexId>::const_iterator it = idByHash.find(hash);
    if (it != idByHash.end())
        return it->second;

    const BlockIndexId id = nextId++;
    idByHash[hash] = id;
    if (indexById.size() <= id)
        indexById.resize(id + 1, NULL);
    indexById[id] = pindex;
    return id;
}

const CBlockIndex* LegacyBlockIndexAccessor::ResolveIdLocked(BlockIndexId id) const
{
    AssertLockHeld(cs_main);
    std::vector<const CBlockIndex*>& indexById = BlockIndexAccessorIndexById();
    if (id == BLOCK_INDEX_ID_INVALID || id >= indexById.size())
        return NULL;
    return indexById[id];
}

BlockIndexSnapshot LegacyBlockIndexAccessor::SnapshotFromIndex(const CBlockIndex* pindex) const
{
    AssertLockHeld(cs_main);
    BlockIndexSnapshot out;
    if (pindex == NULL)
        return out;

    out.found = true;
    out.id = GetOrCreateIdLocked(pindex);
    out.hasParent = (pindex->pprev != NULL);
    out.parentId = out.hasParent ? GetOrCreateIdLocked(pindex->pprev) : BLOCK_INDEX_ID_INVALID;

    out.hash = pindex->GetBlockHash();
    out.hashPrev = pindex->pprev ? pindex->pprev->GetBlockHash() : uint256(0);
    out.hashNext = pindex->pnext ? pindex->pnext->GetBlockHash() : uint256(0);
    out.hashMerkleRoot = pindex->hashMerkleRoot;

    out.height = pindex->nHeight;
    out.nFile = pindex->nFile;
    out.nBlockPos = pindex->nBlockPos;
    out.nFlags = pindex->nFlags;
    out.nVersion = pindex->nVersion;
    out.nTime = pindex->nTime;
    out.nBits = pindex->nBits;
    out.nNonce = pindex->nNonce;
    out.nMint = pindex->nMint;
    out.nMoneySupply = pindex->nMoneySupply;
    out.nStakeModifier = pindex->nStakeModifier;
    out.nStakeModifierTime = pindex->nStakeModifierTime;
    out.nStakeModifierChecksum = pindex->nStakeModifierChecksum;
    out.hasStakeModifierTime = true;
    out.hasStakeModifierChecksum = true;
    out.prevoutStake = pindex->prevoutStake;
    out.nStakeTime = pindex->nStakeTime;
    out.hashProof = pindex->hashProof;
    out.nChainTrust = pindex->nChainTrust;
    out.fProofOfStake = pindex->IsProofOfStake();
    out.fInMainChain = pindex->IsInMainChain();
    return out;
}

BlockIndexSnapshot LegacyBlockIndexAccessor::LookupByHash(uint256 hash) const
{
    AssertLockHeld(cs_main);
    std::map<uint256, CBlockIndex*>::const_iterator it = mapBlockIndex.find(hash);
    if (it == mapBlockIndex.end())
        return BlockIndexSnapshot();
    return SnapshotFromIndex(it->second);
}

BlockIndexSnapshot LegacyBlockIndexAccessor::ReadSnapshot(BlockIndexId id) const
{
    AssertLockHeld(cs_main);
    return SnapshotFromIndex(ResolveIdLocked(id));
}

BlockIndexSnapshot LegacyBlockIndexAccessor::GetParent(BlockIndexId id) const
{
    AssertLockHeld(cs_main);
    const CBlockIndex* pindex = ResolveIdLocked(id);
    if (pindex == NULL || pindex->pprev == NULL)
        return BlockIndexSnapshot();
    return SnapshotFromIndex(pindex->pprev);
}

BlockIndexSnapshot LegacyBlockIndexAccessor::GetAncestor(BlockIndexId id, int targetHeight) const
{
    AssertLockHeld(cs_main);
    const CBlockIndex* pindex = ResolveIdLocked(id);
    if (pindex == NULL)
        return BlockIndexSnapshot();
    const CBlockIndex* pancestor = pindex->GetAncestor(targetHeight);
    return SnapshotFromIndex(pancestor);
}

// IFCN#9 diagnostics (-kerneltrace): distinguish "not found by height" from
// "found but not in the main chain" for the active-successor step.
static bool AccKernelTrace()
{
    // NOT cached: the first call may happen before the argument map is final.
    return GetBoolArg("-kerneltrace", false);
}
BlockIndexSnapshot LegacyBlockIndexAccessor::GetActiveByHeight(int height) const
{
    AssertLockHeld(cs_main);
    const bool kt = AccKernelTrace();
    if (height < 0 || pindexBest == NULL || height > pindexBest->nHeight)
    {
        if (kt) { printf("ACCTRACE GetActiveByHeight h=%d -> OUT_OF_RANGE best=%d\n", height, pindexBest ? pindexBest->nHeight : -1); fflush(stdout); }
        return BlockIndexSnapshot();
    }
    CBlockIndex* pindex = FindBlockByHeight(height);
    // IFCN#9 BUGFIX: active-chain membership must NOT be decided by
    // CBlockIndex::IsInMainChain(), which is `(pnext || this == pindexBest)`.
    // A raw pnext edge is legitimately absent on a retained/evicted/materialized
    // object, so that predicate reports "not in main chain" for a block that IS
    // on the active chain -- and that flipped a consensus input (the stake-kernel
    // modifier walk stopped at the durable tip instead of crossing the buffered
    // window). Membership is decided by identity along the ACTIVE chain instead:
    // the block must be the best chain's own ancestor at that height (pprev/pskip
    // based, which bounded residency keeps).
    CBlockIndex* pbest = (pindexBest && height <= pindexBest->nHeight)
                             ? pindexBest->GetAncestor(height) : NULL;
    if (pindex == NULL || pbest == NULL || pbest != pindex ||
        pbest->nHeight != height || pindex->nHeight != height)
    {
        if (kt) { printf("ACCTRACE GetActiveByHeight h=%d -> found=%d ancestor=%d match=%d best=%d"
                         " pHash=%s aHash=%s\n", height,
                         pindex ? 1 : 0, pbest ? 1 : 0, (pindex && pbest == pindex) ? 1 : 0,
                         pindexBest->nHeight,
                         pindex ? pindex->GetBlockHash().ToString().substr(0,16).c_str() : "-",
                         pbest ? pbest->GetBlockHash().ToString().substr(0,16).c_str() : "-"); fflush(stdout); }
        return BlockIndexSnapshot();
    }
    if (kt) { printf("ACCTRACE GetActiveByHeight h=%d -> OK hash=%s\n", height, pindex->GetBlockHash().ToString().substr(0,16).c_str()); fflush(stdout); }
    return SnapshotFromIndex(pindex);
}

BlockIndexSnapshot LegacyBlockIndexAccessor::GetNextActive(BlockIndexId id) const
{
    AssertLockHeld(cs_main);
    const CBlockIndex* pindex = ResolveIdLocked(id);
    if (AccKernelTrace())
    {
        printf("ACCTRACE GetNextActive id=%llu -> pindex=%d h=%d pnext=%d\n",
               (unsigned long long)id, pindex ? 1 : 0, pindex ? pindex->nHeight : -1,
               (pindex && pindex->pnext) ? 1 : 0);
        fflush(stdout);
    }
    if (pindex == NULL || pindex->pnext == NULL)
        return BlockIndexSnapshot();
    return SnapshotFromIndex(pindex->pnext);
}

BlockIndexSnapshot LegacyBlockIndexAccessor::GetTip() const
{
    AssertLockHeld(cs_main);
    return SnapshotFromIndex(pindexBest);
}

const CBlockIndex* LegacyBlockIndexAccessor::FindForkLocked(const CBlockIndex* a,
                                                            const CBlockIndex* b)
{
    if (a == NULL || b == NULL)
        return NULL;
    while (a->nHeight > b->nHeight)
        a = a->GetAncestor(b->nHeight);
    while (b->nHeight > a->nHeight)
        b = b->GetAncestor(a->nHeight);
    while (a != b)
    {
        if (a == NULL || b == NULL)
            return NULL;
        a = a->pprev;
        b = b->pprev;
    }
    return a;
}

BlockIndexSnapshot LegacyBlockIndexAccessor::FindFork(BlockIndexId a, BlockIndexId b) const
{
    AssertLockHeld(cs_main);
    const CBlockIndex* pa = ResolveIdLocked(a);
    const CBlockIndex* pb = ResolveIdLocked(b);
    return SnapshotFromIndex(FindForkLocked(pa, pb));
}

void ClearBlockIndexAccessorState()
{
    BlockIndexAccessorIdByHash().clear();
    BlockIndexAccessorIndexById().clear();
    BlockIndexAccessorIndexById().push_back(NULL);
    BlockIndexAccessorNextId() = 1;
}
