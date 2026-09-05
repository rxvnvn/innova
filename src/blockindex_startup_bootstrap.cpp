// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "blockindex_startup_bootstrap.h"
#include "blockindex_generation_lifecycle.h"

BlockIndexStartupBootstrap::BlockIndexStartupBootstrap()
    : generation_(0), bestTipId_(), genesisId_(), isOpen_(false)
{
}

BlockIndexStartupBootstrap::~BlockIndexStartupBootstrap()
{
    Close();
}

BlockIndexStartupStatus BlockIndexStartupBootstrap::Open(const std::string& root,
                                                         const BlockIndexV2ReaderOptions& options,
                                                         std::string* error)
{
    Close();
    if (error) error->clear();

    // 1. Open the authority first; its Identity() exposes the SELECTED
    //    generation for the CURRENT pointer, and it owns the generation-bound
    //    reader + derived store (exposed via ReaderPtr/DerivedStorePtr).
    {
        const BlockIndexStartupStatus st = authority_.Open(root, error);
        if (st != BLOCK_INDEX_STARTUP_OK)
            return st;
    }

    const BlockIndexStartupAuthorityIdentity id = authority_.Identity();
    if (id.kind != BLOCK_INDEX_STARTUP_AUTHORITY_V2 || !id.generationQualified)
    {
        if (error) *error = "bootstrap: authority not V2/generation-qualified";
        return BLOCK_INDEX_STARTUP_NOT_AUTHORITATIVE_CAPABLE;
    }
    const uint64_t gen = id.generation;

    // 2. Reuse the authority's generation-bound reader and derived store (the
    //    materializer MUST bind these SAME stores; a second independent open of
    //    the same hashindex/derived.dat would collide on the LevelDB LOCK).
    const BlockIndexV2Reader* reader = authority_.ReaderPtr();
    const BlockIndexDerivedStateStore* derived = authority_.DerivedStorePtr();
    if (!reader || !derived || reader->Generation() != gen || derived->Generation() != gen)
    {
        if (error) *error = "bootstrap: authority reader/derived not generation-bound";
        return BLOCK_INDEX_STARTUP_GENERATION_MISMATCH;
    }
    if (derived->EntryCount() != reader->RecordCount())
    {
        if (error) *error = "bootstrap: derived entry count != reader record count";
        return BLOCK_INDEX_STARTUP_GENERATION_MISMATCH;
    }

    // 3. Bind the materializer + owner to the SAME generation.
    mat_.reset(new BlockIndexAuthorityMaterializer(reader, derived, gen));
    owner_.reset(new BlockIndexHotOwner());
    owner_->SetMaterializer(mat_.get());
    owner_->SetCurrentGeneration(gen);

    // 4. Resolve best tip by value, materialize + pin as a PERMANENT anchor.
    const BlockIndexStartupResult tipRes = authority_.GetTip();
    if (!tipRes.HasRecord())
    {
        if (error) *error = "bootstrap: no best tip in authority";
        return BLOCK_INDEX_STARTUP_NOT_FOUND;
    }
    bestTipId_ = tipRes.record.logicalId;

    const BlockIndexHotStatus pinSt = owner_->Pin(bestTipId_, &bestTipHandle_);
    if (pinSt != BlockIndexHotStatus::OK || !bestTipHandle_.IsValid())
    {
        if (error) *error = "bootstrap: best-tip pin/materialize failed (status " +
            std::to_string((int)pinSt) + ")";
        return BLOCK_INDEX_STARTUP_CORRUPT;
    }

    // 5. Resolve + anchor the genesis block of the SAME generation (A.10.1j).
    //    Supplies pindexGenesisBlock for future authoritative startup.
    const BlockIndexStartupResult genesisRes = authority_.GetActiveByHeight(0);
    if (!genesisRes.HasRecord())
    {
        if (error) *error = "bootstrap: no genesis in authority";
        return BLOCK_INDEX_STARTUP_NOT_FOUND;
    }
    genesisId_ = genesisRes.record.logicalId;

    const BlockIndexHotStatus gPinSt = owner_->Pin(genesisId_, &genesisHandle_);
    if (gPinSt != BlockIndexHotStatus::OK || !genesisHandle_.IsValid())
    {
        if (error) *error = "bootstrap: genesis pin/materialize failed (status " +
            std::to_string((int)gPinSt) + ")";
        return BLOCK_INDEX_STARTUP_CORRUPT;
    }
    // Anchor genesis (and best tip) as PINNED-PERMANENT: never evictable for the
    // bootstrap lifetime.
    owner_->PinPermanent(bestTipId_);
    owner_->PinPermanent(genesisId_);

    generation_ = gen;
    isOpen_ = true;
    (void)options;
    return BLOCK_INDEX_STARTUP_OK;
}

void BlockIndexStartupBootstrap::Close()
{
    if (!isOpen_)
        return;
    bestTipHandle_ = BlockIndexHotHandle();
    genesisHandle_ = BlockIndexHotHandle();
    if (owner_)
        owner_.reset();
    if (mat_)
        mat_.reset();
    authority_.Close();
    generation_ = 0;
    isOpen_ = false;
}

bool BlockIndexStartupBootstrap::IsOpen() const
{
    return isOpen_;
}

uint64_t BlockIndexStartupBootstrap::Generation() const
{
    return generation_;
}

BlockIndexStartupAuthorityIdentity BlockIndexStartupBootstrap::AuthorityIdentity() const
{
    return authority_.Identity();
}

BlockIndexStartupResult BlockIndexStartupBootstrap::GetBestTipRecord() const
{
    BlockIndexStartupResult r;
    if (!isOpen_)
        return r;
    return authority_.LookupByHash(bestTipId_);
}

BlockIndexLogicalId BlockIndexStartupBootstrap::BestTipId() const
{
    return bestTipId_;
}

CBlockIndex* BlockIndexStartupBootstrap::BestTipObject() const
{
    if (!isOpen_ || !bestTipHandle_.IsValid())
        return NULL;
    return bestTipHandle_.Get();
}

BlockIndexLogicalId BlockIndexStartupBootstrap::GenesisId() const
{
    return genesisId_;
}

CBlockIndex* BlockIndexStartupBootstrap::GenesisObject() const
{
    if (!isOpen_ || !genesisHandle_.IsValid())
        return NULL;
    return genesisHandle_.Get();
}