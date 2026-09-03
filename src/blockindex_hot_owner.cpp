// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blockindex_hot_owner.h"

#include "util.h"

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------
BlockIndexHotMetrics& BlockIndexHotMetrics::operator+=(const BlockIndexHotMetrics& o)
{
    residentCount += o.residentCount;
    pinnedCount += o.pinnedCount;
    evictableCount += o.evictableCount;
    materializationRequests += o.materializationRequests;
    materializationHits += o.materializationHits;
    materializationMisses += o.materializationMisses;
    inFlightCount += o.inFlightCount;
    materializationFailures += o.materializationFailures;
    evictions += o.evictions;
    evictionBlocked += o.evictionBlocked;
    if (o.peakResidentCount > peakResidentCount) peakResidentCount = o.peakResidentCount;
    return *this;
}
BlockIndexHotMetrics BlockIndexHotMetrics::operator+(const BlockIndexHotMetrics& o) const
{
    BlockIndexHotMetrics r = *this;
    r += o;
    return r;
}

// ---------------------------------------------------------------------------
// CBlockIndex materialization from by-value metadata (layer-1, no block bytes).
//
// Populates a CBlockIndex's fields from a BlockIndexHotMaterialized snapshot.
// phashBlock is set to an OWNER-OWNED hash (the Entry::ownHash member), so
// identity never depends on a std::map node address. pprev/pnext/pskip are left
// NULL because this is a sparse-hot object: ancestry/active navigation must use
// the by-value seam (BlockIndexV2Reader/StartupAuthority), not a resident chain.
// ---------------------------------------------------------------------------
static CBlockIndex* HotMaterializeToIndex(const BlockIndexHotMaterialized& m,
                                          uint256* ownHash)
{
    if (!m.found)
        return NULL;
    const BlockIndexSnapshot& s = m.snapshot;
    CBlockIndex* pindex = new CBlockIndex();
    *ownHash = s.hash;
    pindex->phashBlock = ownHash;  // owner-owned identity (not a map node address)
    pindex->pprev = NULL;          // sparse-hot: use by-value seam for ancestry
    pindex->pnext = NULL;
    pindex->pskip = NULL;
    pindex->nHeight   = s.height;
    pindex->nFile     = s.nFile;
    pindex->nBlockPos = s.nBlockPos;
    pindex->nFlags    = s.nFlags;
    pindex->nChainTrust = s.nChainTrust;
    pindex->hashProof = s.hashProof;
    pindex->hashMerkleRoot = s.hashMerkleRoot;
    pindex->prevoutStake = s.prevoutStake;
    pindex->nStakeTime = s.nStakeTime;
    pindex->nVersion   = s.nVersion;
    pindex->nTime      = s.nTime;
    pindex->nBits      = s.nBits;
    pindex->nNonce     = s.nNonce;
    pindex->nMint      = s.nMint;
    pindex->nMoneySupply = s.nMoneySupply;
    pindex->nStakeModifier = s.nStakeModifier;
    if (s.hasStakeModifierTime)
        pindex->nStakeModifierTime = s.nStakeModifierTime;
    if (s.hasStakeModifierChecksum)
        pindex->nStakeModifierChecksum = s.nStakeModifierChecksum;
    pindex->nSize = m.hasBlockSize ? m.blockSize : 0;
    if (s.fProofOfStake)
        pindex->nFlags |= CBlockIndex::BLOCK_PROOF_OF_STAKE;
    return pindex;
}

// ---------------------------------------------------------------------------
// BlockIndexHotOwner
// ---------------------------------------------------------------------------
BlockIndexHotOwner::BlockIndexHotOwner()
    : materializer(NULL), currentGeneration(0), nextTokenSeq(1)
{
}

BlockIndexHotOwner::~BlockIndexHotOwner()
{
    for (auto& kv : resident)
    {
        if (kv.second.index)
            delete kv.second.index;
    }
    resident.clear();
}

void BlockIndexHotOwner::SetMaterializer(const BlockIndexHotMaterializer* m)
{
    materializer = m;
}

bool BlockIndexHotOwner::IsResident(const BlockIndexLogicalId& id) const
{
    std::map<uint256, Entry>::const_iterator it = resident.find(id.GetHash());
    return it != resident.end() && it->second.materialized;
}

bool BlockIndexHotOwner::IsPinned(const BlockIndexLogicalId& id) const
{
    std::map<uint256, Entry>::const_iterator it = resident.find(id.GetHash());
    return it != resident.end() && it->second.pins > 0;
}

BlockIndexHotOwner::Entry* BlockIndexHotOwner::EntryLocked(uint256 hash)
{
    std::map<uint256, Entry>::iterator it = resident.find(hash);
    if (it != resident.end())
        return &it->second;
    return NULL;
}

BlockIndexHotStatus BlockIndexHotOwner::RequestMaterialization(const BlockIndexLogicalId& id,
                                                                BlockIndexHotToken* token)
{
    // PHASE 1 (under owner lock): IDLE->PENDING + mint token. No I/O here.
    Entry* e = EntryLocked(id.GetHash());
    if (!e)
    {
        std::map<uint256, Entry>::iterator it =
            resident.insert(std::make_pair(id.GetHash(), Entry())).first;
        e = &it->second;
        e->id = id;
        e->ownHash = id.GetHash();
    }
    if (!e->pending && !e->materialized)
    {
        e->pending = true;
        e->seq = nextTokenSeq++;
        ++metrics.inFlightCount;
    }
    if (token)
    {
        token->hash = id.GetHash();
        token->generation = currentGeneration;
        token->seq = e->seq;
    }
    if (e->materialized)
    {
        ++metrics.materializationHits;
        return BlockIndexHotStatus::OK; // READY (already resident; token unused)
    }
    return BlockIndexHotStatus::MATERIALIZATION_PENDING;
}

BlockIndexHotStatus BlockIndexHotOwner::PublishMaterialized(const BlockIndexHotToken& token,
                                                            const BlockIndexHotMaterialized& m)
{
    // PHASE 3 (under owner lock): validate token + generation, then atomically
    // publish. A stale/foreign completion can never publish (token seq/hash
    // must match the still-pending entry; generation must match CURRENT).
    if (!token.IsValid() || !m.found)
        return BlockIndexHotStatus::MATERIALIZATION_UNAVAILABLE;
    Entry* e = EntryLocked(token.hash);
    if (!e || !e->pending || e->seq != token.seq)
        return BlockIndexHotStatus::GENERATION_MISMATCH; // stale/foreign request

    if (m.generation != 0 && currentGeneration != 0 && m.generation != currentGeneration)
        return BlockIndexHotStatus::GENERATION_MISMATCH; // generation moved while in flight

    CBlockIndex* idx = HotMaterializeToIndex(m, &e->ownHash);
    if (!idx)
        return BlockIndexHotStatus::CORRUPT_METADATA;
    e->index = idx;
    e->residentGen = m.generation;
    e->materialized = true;
    e->pending = false;
    e->seq = 0;
    ++metrics.materializationMisses;
    ++metrics.materializationRequests;
    ++metrics.residentCount;
    if (metrics.inFlightCount > 0) --metrics.inFlightCount;
    if (metrics.residentCount > metrics.peakResidentCount)
        metrics.peakResidentCount = metrics.residentCount;
    return BlockIndexHotStatus::OK;
}

BlockIndexHotStatus BlockIndexHotOwner::EnsureResident(const BlockIndexLogicalId& id)
{
    // Driver: request (lock), materialize OUTSIDE the lock, publish (lock).
    BlockIndexHotToken token;
    BlockIndexHotStatus st = RequestMaterialization(id, &token);
    if (st == BlockIndexHotStatus::OK)
        return BlockIndexHotStatus::OK; // already resident
    if (st != BlockIndexHotStatus::MATERIALIZATION_PENDING)
        return st;

    // I/O (materializer) runs outside the owner lock.
    BlockIndexHotMaterialized m;
    if (!materializer || materializer->Materialize(id, &m) != BlockIndexHotStatus::OK || !m.found)
        return BlockIndexHotStatus::MATERIALIZATION_UNAVAILABLE;
    // Phase 3: re-acquire lock, validate token, atomic publish.
    BlockIndexHotStatus pub = PublishMaterialized(token, m);
    if (pub != BlockIndexHotStatus::OK)
        ++metrics.materializationFailures;
    return pub;
}

BlockIndexHotStatus BlockIndexHotOwner::Pin(const BlockIndexLogicalId& id,
                                            BlockIndexHotHandle* out)
{
    if (!out)
        return BlockIndexHotStatus::PIN_REQUIRED;
    BlockIndexHotStatus st = EnsureResident(id);
    if (st != BlockIndexHotStatus::OK)
    {
        ++metrics.materializationFailures;
        return st;
    }
    // Under lock: acquire a lifetime claim on the resident object.
    Entry* e = EntryLocked(id.GetHash());
    if (!e || !e->materialized)
        return BlockIndexHotStatus::NOT_RESIDENT;
    ++(e->pins);
    ++metrics.pinnedCount;
    out->owner = this;
    out->hash = id.GetHash();
    return BlockIndexHotStatus::OK;
}

BlockIndexHotStatus BlockIndexHotOwner::LookupResident(const BlockIndexLogicalId& id,
                                                       BlockIndexHotHandle* out)
{
    if (!out)
        return BlockIndexHotStatus::PIN_REQUIRED;
    Entry* e = EntryLocked(id.GetHash());
    if (!e || !e->materialized)
        return BlockIndexHotStatus::NOT_RESIDENT;
    ++(e->pins);
    ++metrics.pinnedCount;
    out->owner = this;
    out->hash = id.GetHash();
    return BlockIndexHotStatus::OK;
}

int BlockIndexHotOwner::ReleasePin(const uint256& hash)
{
    Entry* e = EntryLocked(hash);
    if (!e)
        return 0;
    if (e->pins > 0)
    {
        --(e->pins);
        if (metrics.pinnedCount > 0) --metrics.pinnedCount;
    }
    return e->pins;
}

void BlockIndexHotOwner::PinPermanent(const BlockIndexLogicalId& id)
{
    Entry* e = EntryLocked(id.GetHash());
    if (!e)
    {
        std::map<uint256, Entry>::iterator it =
            resident.insert(std::make_pair(id.GetHash(), Entry())).first;
        e = &it->second;
        e->id = id;
        e->ownHash = id.GetHash();
    }
    e->anchor = true;
}

std::vector<BlockIndexLogicalId> BlockIndexHotOwner::EvictEligible() const
{
    std::vector<BlockIndexLogicalId> out;
    for (const auto& kv : resident)
    {
        const Entry& e = kv.second;
        if (!e.materialized) continue;
        if (e.pins > 0) continue;      // pinned: not eligible
        if (e.anchor) continue;        // permanent anchor: not eligible
        out.push_back(BlockIndexLogicalId(e.id.GetHash()));
    }
    return out;
}

BlockIndexHotStatus BlockIndexHotOwner::EvictResident(const BlockIndexLogicalId& id)
{
    std::map<uint256, Entry>::iterator it = resident.find(id.GetHash());
    if (it == resident.end())
        return BlockIndexHotStatus::NOT_RESIDENT;
    Entry& e = it->second;
    if (e.pins > 0 || e.anchor)
    {
        ++metrics.evictionBlocked;
        return BlockIndexHotStatus::EVICTION_BLOCKED;
    }
    if (!e.materialized)
        return BlockIndexHotStatus::NOT_RESIDENT;
    if (e.index)
        delete e.index;
    e.index = NULL;
    e.materialized = false;
    e.pending = false;
    if (metrics.residentCount > 0) --metrics.residentCount;
    ++metrics.evictions;
    return BlockIndexHotStatus::OK;
}

size_t BlockIndexHotOwner::ResidentCount() const
{
    size_t n = 0;
    for (const auto& kv : resident)
        if (kv.second.materialized) ++n;
    return n;
}

size_t BlockIndexHotOwner::PinCount() const
{
    size_t n = 0;
    for (const auto& kv : resident)
        n += (size_t)kv.second.pins;
    return n;
}

CBlockIndex* BlockIndexHotOwner::GetResidentRaw(const uint256& hash) const
{
    std::map<uint256, Entry>::const_iterator it = resident.find(hash);
    if (it == resident.end() || !it->second.materialized)
        return NULL;
    return it->second.index;
}

BlockIndexHotMetrics BlockIndexHotOwner::Metrics() const
{
    BlockIndexHotMetrics m = metrics;
    m.residentCount = (int64_t)ResidentCount();
    m.pinnedCount = (int64_t)PinCount();
    // evictable count = resident && !pinned && !anchor
    int64_t ev = 0;
    for (const auto& kv : resident)
        if (kv.second.materialized && kv.second.pins == 0 && !kv.second.anchor) ++ev;
    m.evictableCount = ev;
    // in-flight = resident-or-pending entries that are pending (not materialized)
    int64_t infl = 0;
    for (const auto& kv : resident)
        if (kv.second.pending && !kv.second.materialized) ++infl;
    m.inFlightCount = infl;
    return m;
}

// ---------------------------------------------------------------------------
// BlockIndexHotHandle
// ---------------------------------------------------------------------------
CBlockIndex* BlockIndexHotHandle::Get() const
{
    if (!owner) return NULL;
    return owner->GetResidentRaw(hash);
}

void BlockIndexHotHandle::Reset()
{
    if (owner)
    {
        owner->ReleasePin(hash);
        owner = NULL;
        hash = 0;
    }
}

BlockIndexHotHandle::~BlockIndexHotHandle()
{
    Reset();
}

BlockIndexHotHandle::BlockIndexHotHandle(BlockIndexHotHandle&& o) noexcept
    : owner(o.owner), hash(o.hash)
{
    o.owner = NULL;
    o.hash = 0;
}

BlockIndexHotHandle& BlockIndexHotHandle::operator=(BlockIndexHotHandle&& o) noexcept
{
    if (this != &o)
    {
        Reset();
        owner = o.owner;
        hash = o.hash;
        o.owner = NULL;
        o.hash = 0;
    }
    return *this;
}