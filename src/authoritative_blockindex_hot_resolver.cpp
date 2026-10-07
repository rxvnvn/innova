// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "authoritative_blockindex_hot_resolver.h"
#include "blockindex_authoritative_live.h"   // GetAuthoritativeLiveAuthority (post-generation tail)

#include <string>

namespace {

// The mutable authoritative live tail (base generation + blockindex_tip), or
// NULL when no live authority is open (non-authoritative mode / pre-open).
// Resolving through it is a pure by-value snapshot read: no mapBlockIndex /
// CBlockIndex* / pprev / pnext residency is allocated.
const BlockIndexAuthoritativeLive* LiveTailOrNull()
{
    BlockIndexAuthoritativeLive* live = GetAuthoritativeLiveAuthority();
    return (live && live->IsOpen()) ? live : NULL;
}

} // namespace

AuthoritativeBlockIndexHotResolver::AuthoritativeBlockIndexHotResolver(
    const BlockIndexV2Reader* reader)
    : reader_(reader)
{
}

uint64_t AuthoritativeBlockIndexHotResolver::Generation() const
{
    return (reader_ && reader_->IsOpen()) ? reader_->Generation() : 0;
}

BlockIndexSnapshot AuthoritativeBlockIndexHotResolver::LookupByHash(const uint256& hash) const
{
    BlockIndexSnapshot out;
    if (reader_ && reader_->IsOpen())
    {
        std::string err;
        BlockIndexV2ReadStatus st = reader_->LookupByHash(hash, &out, &err);
        if (st == BLOCK_INDEX_V2_READ_FOUND)
            return out;
    }
    // Post-generation fallback (Astra freeze Blocker R-1 model, applied to every
    // seam consumer). The immutable generation that backs this resolver holds
    // only what was frozen at startup; on a fresh node that is genesis alone,
    // while every newly accepted block lives in the CURRENT mutable retained
    // tail. A genuine generation miss must therefore consult the live authority
    // (tip-then-base composite) before reporting absence. No legacy resident
    // fallback; a corrupt/IO authority error stays a lookup miss (found=false).
    if (const BlockIndexAuthoritativeLive* live = LiveTailOrNull())
    {
        BlockIndexSnapshot snap;
        std::string lerr;
        if (live->ResolveBlockSnapshot(hash, &snap, &lerr) == BlockIndexHotStatus::OK)
            return snap;
    }
    return BlockIndexSnapshot(); // found=false
}

BlockIndexSnapshot AuthoritativeBlockIndexHotResolver::GetActiveByHeight(int height) const
{
    BlockIndexSnapshot out;
    if (reader_ && reader_->IsOpen())
    {
        std::string err;
        BlockIndexV2ReadStatus st = reader_->GetActiveByHeight(height, &out, &err);
        if (st == BLOCK_INDEX_V2_READ_FOUND)
            return out;
    }
    // Post-generation fallback: heights above the frozen generation's committed
    // tip are ACTIVE-only members of the mutable tip. LookupActiveByHeight
    // matches active members only, so a side record sharing the height is never
    // selected.
    if (const BlockIndexAuthoritativeLive* live = LiveTailOrNull())
    {
        const BlockIndexTipAuthority* tip = live->TipAuthority();
        if (tip && tip->IsOpen())
        {
            std::string tipErr;
            const BlockIndexTipRead tr = tip->LookupActiveByHeight(height, &tipErr);
            if (tr.status == BLOCK_INDEX_TIP_OK)
            {
                BlockIndexSnapshot snap;
                std::string snapErr;
                if (live->ResolveBlockSnapshot(tr.record.hash, &snap, &snapErr) == BlockIndexHotStatus::OK)
                    return snap;
            }
        }
    }
    return BlockIndexSnapshot(); // found=false
}

BlockIndexSnapshot AuthoritativeBlockIndexHotResolver::GetParentByHash(const uint256& hash) const
{
    // Resolve child by value (generation, then live tail); hashPrev is the
    // logical parent. Both child and parent use the tail-aware LookupByHash so a
    // live-tail child whose parent is also live-tail resolves correctly.
    const BlockIndexSnapshot child = LookupByHash(hash);
    if (!child.found)
        return BlockIndexSnapshot();
    if (!child.hasParent || child.hashPrev == uint256(0))
        return BlockIndexSnapshot(); // genesis / no parent -> found=false
    return LookupByHash(child.hashPrev);
}

BlockIndexSnapshot AuthoritativeBlockIndexHotResolver::GetNextActiveByHash(const uint256& hash) const
{
    const BlockIndexSnapshot cur = LookupByHash(hash);
    if (!cur.found)
        return BlockIndexSnapshot();
    if (!cur.fInMainChain)
        return BlockIndexSnapshot(); // not active -> no next-active
    // next-active = active chain at height+1 (derived by height, not stored pnext)
    return GetActiveByHeight(cur.height + 1);
}

BlockIndexSnapshot AuthoritativeBlockIndexHotResolver::GetTip() const
{
    // The hot tip is the CURRENT active chain tip. Prefer the mutable live tip
    // (true best) when the live authority is open; otherwise the immutable
    // generation tip.
    if (const BlockIndexAuthoritativeLive* live = LiveTailOrNull())
    {
        const BlockIndexTipAuthority* tip = live->TipAuthority();
        if (tip && tip->IsOpen())
        {
            const int32_t h = tip->TipHeight();
            const uint256 hsh = tip->TipHash();
            if (h >= 0 && hsh != uint256(0))
            {
                BlockIndexSnapshot snap;
                std::string lerr;
                if (live->ResolveBlockSnapshot(hsh, &snap, &lerr) == BlockIndexHotStatus::OK)
                    return snap;
            }
        }
    }
    if (!reader_ || !reader_->IsOpen())
        return BlockIndexSnapshot();
    return reader_->GetTip();
}
