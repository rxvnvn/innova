// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "blockindex_authoritative_restart.h"

#include <algorithm>

BlockIndexAuthoritativeRestart::BlockIndexAuthoritativeRestart()
    : baseGen_(0), hasBase_(false), effectiveTipHeight_(-1), effectiveTipHash_(0)
{
}

BlockIndexAuthoritativeRestart::~BlockIndexAuthoritativeRestart()
{
    delete tip_.release();
}

bool BlockIndexAuthoritativeRestart::OpenBaseAndTip(
    const std::string& v2Root,
    bool hasBaseGen, uint64_t baseGen,
    BlockIndexStartupBootstrap* bootstrap,
    std::string* error)
{
    hasBase_ = hasBaseGen;
    baseGen_ = baseGen;
    effectiveTipHeight_ = hasBaseGen ? (int32_t)-1 : -1;
    effectiveTipHash_ = uint256(0);

    // The tip authority lives at <v2Root>/blockindex_tip/. If it does not exist,
    // the node is a fresh authoritative boot with no post-S history -> effective
    // tip is the base tip S (handled by the caller via the bootstrap anchor).
    if (!tip_)
        tip_.reset(new BlockIndexTipAuthority());

    boost::filesystem::path tipDir = boost::filesystem::path(v2Root) / "blockindex_tip";
    if (!boost::filesystem::exists(tipDir / "tip.meta"))
    {
        // no mutable tip: node boots purely from the base generation (tip = S).
        // bootstrap anchor is authoritative; effective tip stays the base tip.
        if (bootstrap && bootstrap->IsOpen())
        {
            effectiveTipHeight_ = bootstrap->BestTipObject() ? bootstrap->BestTipObject()->nHeight : -1;
            effectiveTipHash_ = bootstrap->BestTipObject() ? bootstrap->BestTipObject()->GetBlockHash() : uint256(0);
        }
        return true;
    }

    if (!BlockIndexTipAuthority::Open(v2Root, hasBaseGen ? baseGen : 0, tip_.get(), error))
        return false; // fail closed: corrupt / base-generation mismatch

    if (tip_->IsEmpty())
    {
        // empty tip (created but no post-S blocks): effective tip = base tip S
        if (bootstrap && bootstrap->IsOpen())
        {
            effectiveTipHeight_ = bootstrap->BestTipObject() ? bootstrap->BestTipObject()->nHeight : -1;
            effectiveTipHash_ = bootstrap->BestTipObject() ? bootstrap->BestTipObject()->GetBlockHash() : uint256(0);
        }
        return true;
    }

    // non-empty tip: effective tip = post-S tip from the tip authority
    BlockIndexTipRead t = tip_->GetTip();
    if (t.status != BLOCK_INDEX_TIP_OK)
    {
        if (error) *error = "authoritative restart: tip authority has no coherent tip";
        return false;
    }
    effectiveTipHeight_ = t.height;
    effectiveTipHash_ = t.record.hash;
    return true;
}

bool BlockIndexAuthoritativeRestart::HasPostSTip() const
{
    return tip_ && tip_->IsOpen() && !tip_->IsEmpty();
}

int32_t BlockIndexAuthoritativeRestart::EffectiveTipHeight() const
{
    return effectiveTipHeight_;
}

uint256 BlockIndexAuthoritativeRestart::EffectiveTipHash() const
{
    return effectiveTipHash_;
}

uint64_t BlockIndexAuthoritativeRestart::BaseGeneration() const
{
    return baseGen_;
}

size_t BlockIndexAuthoritativeRestart::TipRecordCount() const
{
    return tip_ ? tip_->TipRecordCount() : 0;
}

void BlockIndexAuthoritativeRestart::Close()
{
    if (tip_)
        tip_->Close();
}