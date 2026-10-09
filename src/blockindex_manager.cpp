// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Stage C — production Block Index Manager implementation. See blockindex_manager.h.

#include "blockindex_manager.h"

#include "main.h"                              // hashBestChain, nBestHeight
#include "blockindex_v2_reader.h"              // BlockIndexV2Reader
#include "fixed_blockindex_store.h"            // capability enum
#include "blockindex_authoritative_startup.h"  // g_fAuthoritativeStartup, composite seams

namespace {

BlockIndexManagerStatus MapReadStatus(BlockIndexV2ReadStatus status)
{
    switch (status)
    {
    case BLOCK_INDEX_V2_READ_FOUND:     return BLOCK_INDEX_MANAGER_OK;
    case BLOCK_INDEX_V2_READ_NOT_FOUND: return BLOCK_INDEX_MANAGER_NOT_FOUND;
    case BLOCK_INDEX_V2_READ_CORRUPT:   return BLOCK_INDEX_MANAGER_AUTHORITY_FAILURE;
    case BLOCK_INDEX_V2_READ_IO_ERROR:  return BLOCK_INDEX_MANAGER_AUTHORITY_FAILURE;
    case BLOCK_INDEX_V2_READ_NOT_OPEN:  return BLOCK_INDEX_MANAGER_NOT_OPEN;
    }
    return BLOCK_INDEX_MANAGER_AUTHORITY_FAILURE;
}

BlockIndexManagerStatus MapCompositeResult(AuthoritativeBlockResolutionResult r)
{
    switch (r)
    {
    case AUTHORITATIVE_BLOCK_FOUND:             return BLOCK_INDEX_MANAGER_OK;
    case AUTHORITATIVE_BLOCK_NOT_FOUND:         return BLOCK_INDEX_MANAGER_NOT_FOUND;
    case AUTHORITATIVE_BLOCK_NOT_ACTIVE:        return BLOCK_INDEX_MANAGER_NOT_ACTIVE;
    case AUTHORITATIVE_BLOCK_AUTHORITY_FAILURE: return BLOCK_INDEX_MANAGER_AUTHORITY_FAILURE;
    }
    return BLOCK_INDEX_MANAGER_AUTHORITY_FAILURE;
}

} // namespace

const char* BlockIndexManagerStatusName(BlockIndexManagerStatus status)
{
    switch (status)
    {
    case BLOCK_INDEX_MANAGER_OK:                return "OK";
    case BLOCK_INDEX_MANAGER_NOT_FOUND:         return "NOT_FOUND";
    case BLOCK_INDEX_MANAGER_NOT_ACTIVE:        return "NOT_ACTIVE";
    case BLOCK_INDEX_MANAGER_NOT_OPEN:          return "NOT_OPEN";
    case BLOCK_INDEX_MANAGER_UNSUPPORTED:       return "UNSUPPORTED";
    case BLOCK_INDEX_MANAGER_AUTHORITY_FAILURE: return "AUTHORITY_FAILURE";
    }
    return "UNKNOWN";
}

BlockIndexManager::BlockIndexManager() : reader_(NULL) {}

BlockIndexManager::~BlockIndexManager() {}

void BlockIndexManager::BindReader(const BlockIndexV2Reader* reader)
{
    reader_ = reader;
}

bool BlockIndexManager::UseComposite() const
{
    return g_fAuthoritativeStartup;
}

bool BlockIndexManager::IsAvailable(std::string* error) const
{
    if (UseComposite())
        return true;
    if (reader_ != NULL && reader_->IsOpen())
        return true;
    if (error)
        *error = "block index manager: no authoritative authority and no open reader";
    return false;
}

uint64_t BlockIndexManager::Generation() const
{
    if (reader_ != NULL)
        return reader_->Generation();
    if (UseComposite())
        return AuthoritativeGeneration();
    return 0;
}

uint64_t BlockIndexManager::TotalRecordCount() const
{
    if (reader_ != NULL)
        return reader_->RecordCount();
    return 0;
}

BlockIndexManagerStatus BlockIndexManager::LookupByHash(const uint256& hash,
                                                        BlockIndexSnapshot* out,
                                                        std::string* error) const
{
    if (!out)
        return BLOCK_INDEX_MANAGER_AUTHORITY_FAILURE;
    if (UseComposite())
    {
        out->found = false;
        return MapCompositeResult(ResolveAuthoritativeBlockSnapshotR(hash, out, error));
    }
    if (reader_ == NULL || !reader_->IsOpen())
    {
        if (error) *error = "block index manager: not open";
        return BLOCK_INDEX_MANAGER_NOT_OPEN;
    }
    return MapReadStatus(reader_->LookupByHash(hash, out, error));
}

BlockIndexManagerStatus BlockIndexManager::GetActiveByHeight(int height,
                                                             BlockIndexSnapshot* out,
                                                             std::string* error) const
{
    if (!out)
        return BLOCK_INDEX_MANAGER_AUTHORITY_FAILURE;
    if (UseComposite())
    {
        out->found = false;
        if (AuthoritativeGetActiveSnapshotByHeight(height, out))
            return BLOCK_INDEX_MANAGER_OK;
        if (error) *error = "block index manager: active height not found";
        return BLOCK_INDEX_MANAGER_NOT_FOUND;
    }
    if (reader_ == NULL || !reader_->IsOpen())
    {
        if (error) *error = "block index manager: not open";
        return BLOCK_INDEX_MANAGER_NOT_OPEN;
    }
    return MapReadStatus(reader_->GetActiveByHeight(height, out, error));
}

BlockIndexManagerStatus BlockIndexManager::GetParent(const uint256& hash,
                                                     BlockIndexSnapshot* out,
                                                     std::string* error) const
{
    BlockIndexSnapshot child;
    BlockIndexManagerStatus st = LookupByHash(hash, &child, error);
    if (st != BLOCK_INDEX_MANAGER_OK)
        return st;
    if (child.hashPrev == uint256(0))
    {
        if (error) *error = "block index manager: block has no persisted parent";
        return BLOCK_INDEX_MANAGER_NOT_FOUND;
    }
    return LookupByHash(child.hashPrev, out, error);
}

BlockIndexManagerStatus BlockIndexManager::GetParent(const BlockIndexLogicalId& id,
                                                     BlockIndexSnapshot* out,
                                                     std::string* error) const
{
    return GetParent(id.GetHash(), out, error);
}

BlockIndexManagerStatus BlockIndexManager::GetAncestor(const uint256& hash, int targetHeight,
                                                       BlockIndexSnapshot* out,
                                                       std::string* error) const
{
    // Reader backend: use the O(1) skip-list GetAncestor when the record id is
    // resolvable in the bound generation (same generation as the source hash).
    if (!UseComposite() && reader_ != NULL && reader_->IsOpen())
    {
        BlockIndexSnapshot src;
        BlockIndexManagerStatus st = MapReadStatus(reader_->LookupByHash(hash, &src, error));
        if (st != BLOCK_INDEX_MANAGER_OK)
            return st;
        return MapReadStatus(reader_->GetAncestor(src.id, targetHeight, out, error));
    }
    if (!UseComposite())
    {
        if (error) *error = "block index manager: not open";
        return BLOCK_INDEX_MANAGER_NOT_OPEN;
    }
    // Composite: bounded by-value parent walk (bounded by |height - target|).
    BlockIndexSnapshot cur;
    BlockIndexManagerStatus st = LookupByHash(hash, &cur, error);
    if (st != BLOCK_INDEX_MANAGER_OK)
        return st;
    if (targetHeight < 0)
    {
        if (error) *error = "block index manager: invalid target height";
        return BLOCK_INDEX_MANAGER_NOT_FOUND;
    }
    while (cur.height > targetHeight)
    {
        BlockIndexSnapshot parent;
        st = GetParent(cur.hash, &parent, error);
        if (st != BLOCK_INDEX_MANAGER_OK)
            return st;
        cur = parent;
    }
    if (cur.height != targetHeight)
    {
        if (error) *error = "block index manager: target height not an ancestor";
        return BLOCK_INDEX_MANAGER_NOT_FOUND;
    }
    if (out)
        *out = cur;
    return BLOCK_INDEX_MANAGER_OK;
}

BlockIndexManagerStatus BlockIndexManager::GetAncestor(const BlockIndexLogicalId& id,
                                                       int targetHeight,
                                                       BlockIndexSnapshot* out,
                                                       std::string* error) const
{
    return GetAncestor(id.GetHash(), targetHeight, out, error);
}

BlockIndexManagerStatus BlockIndexManager::GetTip(BlockIndexSnapshot* out,
                                                  std::string* error) const
{
    if (!out)
        return BLOCK_INDEX_MANAGER_AUTHORITY_FAILURE;
    if (UseComposite())
    {
        if (hashBestChain == uint256(0))
        {
            if (error) *error = "block index manager: no active tip yet";
            return BLOCK_INDEX_MANAGER_NOT_FOUND;
        }
        return LookupByHash(hashBestChain, out, error);
    }
    if (reader_ == NULL || !reader_->IsOpen())
    {
        if (error) *error = "block index manager: not open";
        return BLOCK_INDEX_MANAGER_NOT_OPEN;
    }
    *out = reader_->GetTip();
    return out->found ? BLOCK_INDEX_MANAGER_OK : BLOCK_INDEX_MANAGER_NOT_FOUND;
}

BlockIndexManagerStatus BlockIndexManager::BestTipHash(uint256* out, std::string* error) const
{
    if (!out)
        return BLOCK_INDEX_MANAGER_AUTHORITY_FAILURE;
    BlockIndexSnapshot tip;
    BlockIndexManagerStatus st = GetTip(&tip, error);
    if (st != BLOCK_INDEX_MANAGER_OK)
        return st;
    *out = tip.hash;
    return BLOCK_INDEX_MANAGER_OK;
}

BlockIndexManagerStatus BlockIndexManager::ActiveTipHeight(int* out, std::string* error) const
{
    if (!out)
        return BLOCK_INDEX_MANAGER_AUTHORITY_FAILURE;
    BlockIndexSnapshot tip;
    BlockIndexManagerStatus st = GetTip(&tip, error);
    if (st != BLOCK_INDEX_MANAGER_OK)
        return st;
    *out = tip.height;
    return BLOCK_INDEX_MANAGER_OK;
}

BlockIndexManagerStatus BlockIndexManager::Contains(const uint256& hash, bool* out,
                                                    std::string* error) const
{
    if (!out)
        return BLOCK_INDEX_MANAGER_AUTHORITY_FAILURE;
    BlockIndexSnapshot s;
    BlockIndexManagerStatus st = LookupByHash(hash, &s, error);
    if (st == BLOCK_INDEX_MANAGER_OK)
    {
        *out = true;
        return BLOCK_INDEX_MANAGER_OK;
    }
    if (st == BLOCK_INDEX_MANAGER_NOT_FOUND || st == BLOCK_INDEX_MANAGER_NOT_ACTIVE)
    {
        *out = false;
        return BLOCK_INDEX_MANAGER_OK;
    }
    return st; // fail closed: never report absence on an authority failure
}

BlockIndexManagerStatus BlockIndexManager::TipIs(const uint256& hash, bool* out,
                                                 std::string* error) const
{
    if (!out)
        return BLOCK_INDEX_MANAGER_AUTHORITY_FAILURE;
    uint256 tip;
    BlockIndexManagerStatus st = BestTipHash(&tip, error);
    if (st != BLOCK_INDEX_MANAGER_OK)
        return st;
    *out = (tip == hash);
    return BLOCK_INDEX_MANAGER_OK;
}

BlockIndexManagerStatus BlockIndexManager::GetDerivedConsensus(const uint256& hash,
                                                               BlockIndexDerivedConsensus* out,
                                                               std::string* error) const
{
    if (!out)
        return BLOCK_INDEX_MANAGER_AUTHORITY_FAILURE;
    BlockIndexSnapshot s;
    BlockIndexManagerStatus st = LookupByHash(hash, &s, error);
    if (st != BLOCK_INDEX_MANAGER_OK)
        return st;

    // Fail closed on a generation that cannot authoritatively provide derived
    // consensus state (old shadow generation / no derived companion).
    if (!UseComposite())
    {
        if (reader_ == NULL)
            return BLOCK_INDEX_MANAGER_NOT_OPEN;
        if (reader_->Manifest().capability == BLOCK_INDEX_GENERATION_CAPABILITY_OLD_SHADOW)
        {
            if (error) *error = "block index manager: generation is not derived-capable";
            return BLOCK_INDEX_MANAGER_UNSUPPORTED;
        }
        out->chainTrust = s.nChainTrust;
        out->hasChainTrust = true; // derived-capable generation: chainTrust authoritative
    }
    else
    {
        out->chainTrust = s.nChainTrust;
        out->hasChainTrust = true; // authoritative composite always carries chainTrust
    }

    // Never substitute zero: availability flags are carried verbatim.
    out->stakeModifierChecksum = s.nStakeModifierChecksum;
    out->hasStakeModifierChecksum = s.hasStakeModifierChecksum;
    out->stakeModifierTime = s.nStakeModifierTime;
    out->hasStakeModifierTime = s.hasStakeModifierTime;
    // Stage D: derived serialized block size is now served from the same
    // by-value snapshot. Availability is carried verbatim (never zero-substituted).
    out->blockSize = s.nSize;
    out->hasBlockSize = s.hasBlockSize;
    return BLOCK_INDEX_MANAGER_OK;
}

BlockIndexManagerStatus BlockIndexManager::GetLastBlockIndexByProofType(const uint256& fromHash,
                                                                       bool fProofOfStake,
                                                                       BlockIndexSnapshot* out,
                                                                       std::string* error) const
{
    BlockIndexSnapshot s;
    BlockIndexManagerStatus st = LookupByHash(fromHash, &s, error);
    if (st != BLOCK_INDEX_MANAGER_OK)
        return st;
    // Legacy GetLastBlockIndex semantics: stop when the proof type matches OR the
    // walk reaches genesis (no parent). The returned block may then be genesis
    // even when its proof type differs — byte-for-byte the legacy behavior.
    while (s.hasParent && (s.fProofOfStake != fProofOfStake))
    {
        st = GetParent(s.hash, &s, error);
        if (st != BLOCK_INDEX_MANAGER_OK)
            return st;
    }
    *out = s;
    return BLOCK_INDEX_MANAGER_OK;
}

// Stage F (L6c): hash-native getblocks locator. Byte-for-byte equivalent to the
// legacy CBlockLocator(const CBlockIndex*) Set() walk, but parent navigation is
// BY VALUE through the manager (no CBlockIndex*, no mapBlockIndex).
BlockIndexManagerStatus BlockIndexManager::GetLocatorHashes(const uint256& hashBegin,
                                                            std::vector<uint256>* out,
                                                            std::string* error) const
{
    if (out == NULL)
        return BLOCK_INDEX_MANAGER_AUTHORITY_FAILURE;
    out->clear();
    if (hashBegin == uint256(0))
    {
        // Retired NULL begin produced a genesis-only locator.
        out->push_back(GetGenesisBlockHash());
        return BLOCK_INDEX_MANAGER_OK;
    }
    BlockIndexSnapshot start;
    BlockIndexManagerStatus st = LookupByHash(hashBegin, &start, error);
    if (st != BLOCK_INDEX_MANAGER_OK)
    {
        // Fail closed: never emit a locator for a begin we cannot resolve.
        if (error != NULL && error->empty())
            *error = "block index manager: locator begin is not authoritatively resolvable";
        return st;
    }
    int nStep = 1;
    int nHeight = start.height;
    while (nHeight >= 0)
    {
        BlockIndexSnapshot a;
        BlockIndexManagerStatus as;
        if (nHeight == start.height)
        {
            a = start;
            as = BLOCK_INDEX_MANAGER_OK;
        }
        else
        {
            as = GetAncestor(hashBegin, nHeight, &a, NULL);
        }
        if (as != BLOCK_INDEX_MANAGER_OK)
            break; // legacy: a NULL GetAncestor() ends the walk (partial vHave)
        out->push_back(a.hash);
        nHeight -= nStep;
        if (out->size() > 10)
            nStep *= 2;
    }
    out->push_back(GetGenesisBlockHash());
    return BLOCK_INDEX_MANAGER_OK;
}

BlockIndexManager::View BlockIndexManager::MakeView(std::string* error) const
{
    View v;
    BlockIndexSnapshot tip;
    BlockIndexManagerStatus st = GetTip(&tip, error);
    if (st != BLOCK_INDEX_MANAGER_OK)
        return v;
    v.mgr_ = this;
    v.generation_ = Generation();
    v.tipHash_ = tip.hash;
    v.tipHeight_ = tip.height;
    return v;
}

BlockIndexManagerStatus BlockIndexManager::View::CheckCoherent(std::string* error) const
{
    if (mgr_ == NULL)
    {
        if (error) *error = "block index view: invalid";
        return BLOCK_INDEX_MANAGER_NOT_OPEN;
    }
    if (mgr_->Generation() != generation_)
    {
        if (error) *error = "block index view: selected generation changed";
        return BLOCK_INDEX_MANAGER_AUTHORITY_FAILURE;
    }
    BlockIndexSnapshot tip;
    BlockIndexManagerStatus st = mgr_->GetTip(&tip, error);
    if (st != BLOCK_INDEX_MANAGER_OK)
        return st;
    if (tip.hash != tipHash_)
    {
        if (error) *error = "block index view: active tip changed";
        return BLOCK_INDEX_MANAGER_AUTHORITY_FAILURE;
    }
    return BLOCK_INDEX_MANAGER_OK;
}

BlockIndexManager& GetBlockIndexManager()
{
    static BlockIndexManager mgr;
    const BlockIndexV2Reader* rd = GetAuthoritativeBaseReader();
    if (rd != NULL && !mgr.IsReaderBound())
        mgr.BindReader(rd);
    return mgr;
}
