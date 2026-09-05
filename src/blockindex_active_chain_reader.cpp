// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "blockindex_active_chain_reader.h"

BlockIndexActiveChainReader::BlockIndexActiveChainReader()
    : open_(false)
{
}

bool BlockIndexActiveChainReader::Open(const std::string& generationDir,
                                       uint64_t generation,
                                       std::string* error)
{
    Close();
    if (!FixedBlockIndexShadowActiveLookup::Open(generationDir, &lookup_, error))
        return false;
    open_ = true;
    (void)generation; // lookup binds records.dat+active.dat+MANIFEST generation coherence
    return true;
}

void BlockIndexActiveChainReader::Close()
{
    open_ = false;
    lookup_ = FixedBlockIndexShadowActiveLookup();
}

bool BlockIndexActiveChainReader::IsOpen() const
{
    return open_;
}

int64_t BlockIndexActiveChainReader::GetActiveHeight() const
{
    if (!open_)
        return -1;
    // iterate physical height downward until a committed entry is found; the
    // shadow lookup enforces committed-region boundary.
    for (int64_t h = 0; h < 2000000; ++h)
    {
        BlockIndexRecord rec;
        BlockIndexId id;
        std::string err;
        BlockIndexActiveLookupStatus st = lookup_.LookupByHeight((int32_t)h, &rec, &id, &err);
        if (st == BLOCK_INDEX_ACTIVE_LOOKUP_NOT_FOUND)
            return h - 1;
        if (st != BLOCK_INDEX_ACTIVE_LOOKUP_FOUND)
            return -1; // error
    }
    return -1;
}

bool BlockIndexActiveChainReader::LookupByHeight(int32_t height,
                                                 BlockIndexActiveBlock* out,
                                                 std::string* error) const
{
    if (!open_ || !out)
    {
        if (error) error->clear();
        return false;
    }
    BlockIndexRecord rec;
    BlockIndexId id;
    BlockIndexActiveLookupStatus st = lookup_.LookupByHeight(height, &rec, &id, error);
    if (st != BLOCK_INDEX_ACTIVE_LOOKUP_FOUND)
        return false;
    out->hash = rec.hash;
    out->height = rec.height;
    out->nFile = rec.nFile;
    out->nBlockPos = rec.nBlockPos;
    out->nTime = rec.nTime;
    return true;
}