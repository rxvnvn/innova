// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "authoritative_blockindex_hot_resolver.h"

#include <string>

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
    if (!reader_ || !reader_->IsOpen())
        return out; // found=false
    std::string err;
    BlockIndexV2ReadStatus st = reader_->LookupByHash(hash, &out, &err);
    if (st != BLOCK_INDEX_V2_READ_FOUND)
        return BlockIndexSnapshot(); // found=false (NOT_FOUND/CORRUPT -> absent)
    return out;
}

BlockIndexSnapshot AuthoritativeBlockIndexHotResolver::GetActiveByHeight(int height) const
{
    BlockIndexSnapshot out;
    if (!reader_ || !reader_->IsOpen())
        return out;
    std::string err;
    BlockIndexV2ReadStatus st = reader_->GetActiveByHeight(height, &out, &err);
    if (st != BLOCK_INDEX_V2_READ_FOUND)
        return BlockIndexSnapshot();
    return out;
}

BlockIndexSnapshot AuthoritativeBlockIndexHotResolver::GetParentByHash(const uint256& hash) const
{
    BlockIndexSnapshot out;
    if (!reader_ || !reader_->IsOpen())
        return out;
    // Resolve child by value; hashPrev is the logical parent.
    std::string err;
    BlockIndexSnapshot child;
    BlockIndexV2ReadStatus st = reader_->LookupByHash(hash, &child, &err);
    if (st != BLOCK_INDEX_V2_READ_FOUND)
        return out;
    if (!child.hasParent || child.hashPrev == uint256(0))
        return out; // genesis / no parent -> found=false
    st = reader_->LookupByHash(child.hashPrev, &out, &err);
    if (st != BLOCK_INDEX_V2_READ_FOUND)
        return BlockIndexSnapshot();
    return out;
}

BlockIndexSnapshot AuthoritativeBlockIndexHotResolver::GetNextActiveByHash(const uint256& hash) const
{
    BlockIndexSnapshot out;
    if (!reader_ || !reader_->IsOpen())
        return out;
    std::string err;
    BlockIndexSnapshot cur;
    BlockIndexV2ReadStatus st = reader_->LookupByHash(hash, &cur, &err);
    if (st != BLOCK_INDEX_V2_READ_FOUND)
        return out;
    if (!cur.fInMainChain)
        return out; // not active -> no next-active
    // next-active = active chain at height+1 (derived by height, not stored pnext)
    st = reader_->GetActiveByHeight(cur.height + 1, &out, &err);
    if (st != BLOCK_INDEX_V2_READ_FOUND)
        return BlockIndexSnapshot(); // tip / no successor -> found=false
    return out;
}

BlockIndexSnapshot AuthoritativeBlockIndexHotResolver::GetTip() const
{
    if (!reader_ || !reader_->IsOpen())
        return BlockIndexSnapshot();
    return reader_->GetTip();
}