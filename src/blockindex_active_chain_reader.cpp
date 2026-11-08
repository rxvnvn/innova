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
    // O(1): the committed active-chain tip is a MANIFEST fact. The previous
    // implementation probed heights 0..1,999,999 and treated "no NOT_FOUND in
    // that window" as an error, which made it return -1 on every chain longer
    // than 2M blocks (mainnet is ~8.2M) and silently disabled the by-value
    // wallet rescan. Never re-introduce a height-proportional probe here.
    const int32_t tip = lookup_.CommittedTipHeight();
    if (tip < 0)
        return -1; // no committed tip: fail closed, callers must not scan
    // Fail-closed coherence check: the committed tip must resolve to a record
    // at that height, and (when the manifest carries one) to the committed hash.
    BlockIndexRecord rec;
    BlockIndexId id = BLOCK_INDEX_ID_INVALID;
    std::string err;
    if (lookup_.LookupByHeight(tip, &rec, &id, &err) != BLOCK_INDEX_ACTIVE_LOOKUP_FOUND)
        return -1;
    if (rec.height != tip)
        return -1;
    const uint256 committedHash = lookup_.CommittedTipHash();
    if (committedHash != uint256(0) && rec.hash != committedHash)
        return -1;
    return tip;
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