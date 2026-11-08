// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "blockindex_active_chain_reader.h"
#include "blockindex_authoritative_live.h"   // post-generation tail resolution
#include "blockindex_tip.h"

namespace {

// The live tail authority is PROCESS-GLOBAL. It must only extend a reader whose
// own immutable generation it actually continues: same base generation and same
// committed base-tip hash. Without this guard a stale or unrelated authority
// (another fixture in the test binary, a previous datadir in-process) silently
// changes the height GetActiveHeight()/LookupByHeight() report, and a rescan
// could read blocks that do not belong to the wallet's chain.
bool TailAuthorityExtendsThisReader(const BlockIndexAuthoritativeLive* live,
                                    uint64_t generation,
                                    const uint256& committedTipHash)
{
    if (live == NULL || !live->IsOpen() || generation == 0)
        return false;
    if (live->BaseGeneration() != generation)
        return false;
    const BlockIndexTipAuthority* tip = live->TipAuthority();
    if (tip == NULL || !tip->IsOpen())
        return false;
    if (committedTipHash != uint256(0) && tip->BaseTipHash() != committedTipHash)
        return false;
    return true;
}

} // namespace

BlockIndexActiveChainReader::BlockIndexActiveChainReader()
    : open_(false)
    , generation_(0)
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
    generation_ = generation;
    committedTipHash_ = lookup_.CommittedTipHash();
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
    // ---- post-generation tail ------------------------------------------------
    // The node runs ahead of the frozen generation's committed tip S. Extend the
    // rescan horizon to the mutable authoritative tip when it is open, otherwise
    // a fresh import would silently miss every block between S and the live tip.
    // The extended height is accepted only if it resolves through the same
    // by-value path, so a closed/absent tip authority degrades to S (fail-safe).
    const BlockIndexAuthoritativeLive* live = GetAuthoritativeLiveAuthority();
    if (TailAuthorityExtendsThisReader(live, generation_, committedTipHash_))
    {
        const BlockIndexTipAuthority* tipAuth = live->TipAuthority();
        const BlockIndexTipRead tipRead = tipAuth->GetTip();
        const int32_t liveTip = (tipRead.status == BLOCK_INDEX_TIP_OK) ? tipRead.height : -1;
        if (liveTip > tip)
        {
            BlockIndexActiveBlock tailBlock;
            std::string tailErr;
            if (LookupByHeight(liveTip, &tailBlock, &tailErr))
                return liveTip;
        }
    }
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
    if (st == BLOCK_INDEX_ACTIVE_LOOKUP_FOUND)
    {
        out->hash = rec.hash;
        out->height = rec.height;
        out->nFile = rec.nFile;
        out->nBlockPos = rec.nBlockPos;
        out->nTime = rec.nTime;
        return true;
    }
    if (st != BLOCK_INDEX_ACTIVE_LOOKUP_NOT_FOUND)
        return false; // authority error: fail closed, never fall through
    // ---- post-generation tail ------------------------------------------------
    // The immutable generation only covers heights up to its committed tip S. On
    // mainnet the live node is ~53k blocks ahead of S, so a wallet rescan bounded
    // by S would silently miss recent history (the very blocks a freshly imported
    // key needs). Heights above S are ACTIVE-only members of the mutable tip
    // authority; resolve them there, by value, exactly like the hot resolver does
    // for consensus consumers. Absent authority => not found (previous behaviour).
    const BlockIndexAuthoritativeLive* live = GetAuthoritativeLiveAuthority();
    if (TailAuthorityExtendsThisReader(live, generation_, committedTipHash_))
    {
        const BlockIndexTipAuthority* tip = live->TipAuthority();
        std::string terr;
        const BlockIndexTipRead tr = tip->LookupActiveByHeightLogical(height, &terr);
        if (tr.status == BLOCK_INDEX_TIP_OK)
        {
            out->hash = tr.record.hash;
            out->height = tr.record.height;
            out->nFile = tr.record.nFile;
            out->nBlockPos = tr.record.nBlockPos;
            out->nTime = tr.record.nTime;
            return true;
        }
    }
    return false;
}