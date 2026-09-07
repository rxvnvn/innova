// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blockindex_live_tail_full.h"

#include "blockindex_v2_reader.h"

#include <boost/filesystem.hpp>

namespace fs = boost::filesystem;

class BlockIndexLiveTailFull::Impl
{
public:
    const BlockIndexV2Reader* baseReader;

    Impl() : baseReader(NULL) {}
    ~Impl()
    {
        for (size_t i = 0; i < owned_.size(); ++i)
        {
            delete owned_[i];
            delete ownedHashes_[i];
        }
        owned_.clear();
        ownedHashes_.clear();
    }

    // Build a single CBlockIndex from a BlockIndexSnapshot (by-value). Scalar
    // fields filled; pprev/pnext/pskip set by the caller to link the chain.
    CBlockIndex* SnapshotToIndex(const BlockIndexSnapshot& s, uint256* ownHash) const
    {
        CBlockIndex* p = new CBlockIndex();
        *ownHash = s.hash;
        p->phashBlock = ownHash;
        p->pprev = NULL;
        p->pnext = NULL;
        p->pskip = NULL;
        p->nHeight     = s.height;
        p->nFile       = s.nFile;
        p->nBlockPos   = s.nBlockPos;
        p->nChainTrust = s.nChainTrust;
        p->hashProof   = s.hashProof;
        p->hashMerkleRoot = s.hashMerkleRoot;
        p->prevoutStake   = s.prevoutStake;
        p->nStakeTime     = s.nStakeTime;
        p->nVersion   = s.nVersion;
        p->nTime      = s.nTime;
        p->nBits      = s.nBits;
        p->nNonce     = s.nNonce;
        p->nMint      = s.nMint;
        p->nMoneySupply = s.nMoneySupply;
        p->nStakeModifier = s.nStakeModifier;
        // Preserve the authority's nFlags (BLOCK_STAKE_MODIFIER / GENERATED for
        // genesis + stake blocks) — the legacy consensus walks rely on them.
        p->nFlags = s.nFlags;
        if (s.hasStakeModifierTime)
            p->nStakeModifierTime = s.nStakeModifierTime;
        if (s.hasStakeModifierChecksum)
            p->nStakeModifierChecksum = s.nStakeModifierChecksum;
        if (s.fProofOfStake)
            p->nFlags |= CBlockIndex::BLOCK_PROOF_OF_STAKE;
        return p;
    }

    // Owned CBlockIndex objects + their owner-owned hash identities. Kept alive
    // for the lifetime of this materializer (RAII). Released on destruction.
    std::vector<CBlockIndex*> owned_;
    std::vector<uint256*>      ownedHashes_;
};

BlockIndexLiveTailFull::BlockIndexLiveTailFull()
    : impl_(new Impl())
{
}

BlockIndexLiveTailFull::~BlockIndexLiveTailFull()
{
    delete impl_;
}

void BlockIndexLiveTailFull::SetSources(const BlockIndexV2Reader* baseReader,
                                        const void* tipUnused)
{
    (void)tipUnused;
    impl_->baseReader = baseReader;
}

CBlockIndex* BlockIndexLiveTailFull::MaterializeSingle(
    const uint256& hash, BlockIndexHotHandle* handle, std::string* error)
{
    if (!impl_->baseReader || !impl_->baseReader->IsOpen())
    {
        if (error) *error = "full-tail: base reader not open";
        return NULL;
    }
    BlockIndexSnapshot s;
    std::string rerr;
    BlockIndexV2ReadStatus st = impl_->baseReader->LookupByHash(hash, &s, &rerr);
    if (st != BLOCK_INDEX_V2_READ_FOUND || !s.found)
    {
        if (error) *error = "full-tail: by-value lookup failed for " + hash.ToString();
        return NULL;
    }
    uint256* own = new uint256(hash);
    CBlockIndex* p = impl_->SnapshotToIndex(s, own);
    impl_->owned_.push_back(p);
    impl_->ownedHashes_.push_back(own);
    if (handle)
        *handle = BlockIndexHotHandle(); // ownership handled by this materializer
    return p;
}

CBlockIndex* BlockIndexLiveTailFull::MaterializeChain(
    const uint256& tipHash, const uint256& floorHash,
    BlockIndexHotHandle* tipHandle, std::string* error)
{
    // Walk pprev by-value from tipHash down to floorHash, materializing each step.
    std::vector<BlockIndexSnapshot> path;
    std::string rerr;
    uint256 cur = tipHash;
    bool doneFloor = false;
    for (int guard = 0; guard < 20 * 1000 * 1000; ++guard)
    {
        if (cur == uint256(0))
            break;
        BlockIndexSnapshot s;
        BlockIndexV2ReadStatus st = impl_->baseReader->LookupByHash(cur, &s, &rerr);
        if (st != BLOCK_INDEX_V2_READ_FOUND || !s.found)
        {
            if (error) *error = "full-tail: chain walk lookup failed at " + cur.ToString();
            return NULL;
        }
        path.push_back(s);
        if (cur == floorHash)
        {
            doneFloor = true;
            break;
        }
        cur = s.hashPrev;
    }
    if (!doneFloor)
    {
        if (error) *error = "full-tail: floor " + floorHash.ToString() + " not reached from tip";
        return NULL;
    }
    if (path.empty())
    {
        if (error) *error = "full-tail: empty chain";
        return NULL;
    }

    // Materialize objects (path[0]=tip .. path.back()=floor).
    std::vector<CBlockIndex*> objs(path.size(), NULL);
    std::vector<uint256*> ownHashes(path.size(), NULL);
    for (int i = (int)path.size() - 1; i >= 0; --i)
    {
        const BlockIndexSnapshot& s = path[i];
        ownHashes[i] = new uint256(s.hash);
        CBlockIndex* p = impl_->SnapshotToIndex(s, ownHashes[i]);
        objs[i] = p;
    }
    // Link topology: pprev points toward the floor (deeper ancestor),
    // pnext points toward the tip (shallower child = next active member).
    for (size_t i = 0; i < objs.size(); ++i)
    {
        objs[i]->pprev = (i + 1 < objs.size()) ? objs[i + 1] : NULL; // -> floor
        objs[i]->pnext = (i > 0) ? objs[i - 1] : NULL;               // -> tip
        objs[i]->pskip = objs[i]->pprev; // conservative (BuildSkip not needed for bounded tail)
    }
    CBlockIndex* tip = objs.front(); // path[0] is the requested tip (highest height)
    // Retain ownership for lifetime (RAII): the materializer owns the objects.
    for (size_t i = 0; i < objs.size(); ++i)
    {
        impl_->owned_.push_back(objs[i]);
        impl_->ownedHashes_.push_back(ownHashes[i]);
    }
    if (tipHandle)
        *tipHandle = BlockIndexHotHandle(); // empty; caller uses returned CBlockIndex
    return tip;
}

size_t BlockIndexLiveTailFull::ResidentCount() const
{
    // Objects owned by this standalone materializer (approximation of residency).
    return impl_->owned_.size();
}

BlockIndexLiveTailFull* GetAuthoritativeLiveTailFull()
{
    return NULL; // retained by the authoritative live context (see live authority)
}