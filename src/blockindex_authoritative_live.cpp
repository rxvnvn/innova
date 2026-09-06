// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blockindex_authoritative_live.h"

#include "blockindex_startup_bootstrap.h"

#include <boost/filesystem.hpp>

namespace fs = boost::filesystem;

class BlockIndexAuthoritativeLive::Impl
{
public:
    // base-known callback: parent is known if the base reader (blocks <= S) or
    // the mutable tip (> S) can resolve it by value.
    // baseKnown for the acceptance seam uses the reader.
    const BlockIndexV2Reader*  baseReader;
    std::unique_ptr<BlockIndexTipAuthority> tip;
    std::unique_ptr<BlockIndexLiveTail>     tail;
    std::unique_ptr<BlockIndexLiveAcceptance> seam;
    int horizon;
    uint64_t baseGeneration;
    bool open;
    std::string root;

    Impl()
        : baseReader(NULL), horizon(2048), baseGeneration(0), open(false)
    {
    }
};

namespace {

bool BlockIndexAuthoritativeLiveBaseKnown(const uint256& hash, void* ud)
{
    BlockIndexAuthoritativeLive::Impl* self =
        static_cast<BlockIndexAuthoritativeLive::Impl*>(ud);
    if (!self || !self->baseReader || !self->baseReader->IsOpen())
        return false;
    BlockIndexSnapshot sn;
    std::string err;
    BlockIndexV2ReadStatus st = self->baseReader->LookupByHash(hash, &sn, &err);
    return st == BLOCK_INDEX_V2_READ_FOUND;
}

static void ClearError(std::string* error)
{
    if (error) error->clear();
}

static bool SetError(std::string* error, const std::string& message)
{
    if (error) *error = message;
    return false;
}

} // namespace

BlockIndexAuthoritativeLive::BlockIndexAuthoritativeLive()
    : impl_(new Impl())
{
}

BlockIndexAuthoritativeLive::~BlockIndexAuthoritativeLive()
{
    Close();
    delete impl_;
}

bool BlockIndexAuthoritativeLive::Open(const std::string& v2Root,
                                        const BlockIndexV2Reader* baseReader,
                                        int livetailHorizon,
                                        std::string* error)
{
    if (impl_->open)
    {
        Close();
        impl_->open = false;
    }
    if (!baseReader || !baseReader->IsOpen())
        return SetError(error, "authoritative-live: base reader not open");
    const uint64_t gen = baseReader->Generation();
    if (gen == 0)
        return SetError(error, "authoritative-live: base reader generation 0");

    impl_->baseReader = baseReader;
    impl_->baseGeneration = gen;
    impl_->horizon = (livetailHorizon > 0) ? livetailHorizon : 2048;
    impl_->root = v2Root;

    const uint64_t baseRecordCount = baseReader->RecordCount();
    // Determine the base generation's committed tip height S from the reader.
    const BlockIndexSnapshot tipSnap = baseReader->GetTip();
    const int32_t baseTipHeight = tipSnap.found ? (int32_t)tipSnap.height : -1;
    if (baseTipHeight < 0)
        return SetError(error, "authoritative-live: base generation has no tip");

    // Open (or create) the mutable tip store under <v2Root>/blockindex_tip.
    impl_->tip.reset(new BlockIndexTipAuthority());
    fs::path tipMeta = fs::path(v2Root) / "blockindex_tip" / "tip.meta";
    if (fs::exists(tipMeta))
    {
        if (!BlockIndexTipAuthority::Open(v2Root, gen, impl_->tip.get(), error))
        {
            impl_->tip.reset();
            return false; // fail closed on base/tip mismatch or corrupt state
        }
    }
    else
    {
        if (!BlockIndexTipAuthority::Create(v2Root, gen, baseRecordCount,
                                            baseTipHeight, impl_->tip.get(), error))
        {
            impl_->tip.reset();
            return false;
        }
    }

    // Tail composite materializer: base reader (<=S) + tip (> S).
    impl_->tail.reset(new BlockIndexLiveTail());
    impl_->tail->SetSources(impl_->baseReader, impl_->tip.get());
    impl_->tail->SetHorizon(impl_->horizon);
    impl_->tail->SetCurrentGeneration(gen);
    // permanent anchors: best tip (from base reader tip snapshot) + genesis.
    {
        BlockIndexLogicalId best(impl_->baseReader->GetTip().hash);
        // only anchor the base tip if found; genesis is height 0 record
        if (tipSnap.found)
            impl_->tail->PinPermanent(best);
        BlockIndexSnapshot g0;
        std::string gerr;
        if (impl_->baseReader->GetActiveByHeight(0, &g0, &gerr) == BLOCK_INDEX_V2_READ_FOUND && g0.found)
            impl_->tail->PinPermanent(BlockIndexLogicalId(g0.hash));
    }

    // Acceptance seam arm: baseKnown = reader lookup, tip + tail (ud = impl_).
    impl_->seam.reset(new BlockIndexLiveAcceptance());
    impl_->seam->SetSources(&BlockIndexAuthoritativeLiveBaseKnown, impl_,
                            impl_->tip.get(), impl_->tail.get());

    impl_->open = true;
    ClearError(error);
    return true;
}

bool BlockIndexAuthoritativeLive::ResolveParent(const uint256& parentHash,
                                                 int* parentHeight,
                                                 std::string* error) const
{
    if (!impl_->open || !impl_->seam)
        return SetError(error, "authoritative-live: not open");
    const BlockIndexLiveAcceptance::ParentStatus ps =
        impl_->seam->ResolveParent(parentHash, parentHeight, error);
    if (ps == BlockIndexLiveAcceptance::PARENT_ERROR)
        return SetError(error, "authoritative-live: parent resolve error");
    if (ps == BlockIndexLiveAcceptance::PARENT_KNOWN)
    {
        if (error) error->clear();
        return true;
    }
    return false; // PARENT_UNKNOWN: genuine orphan
}

BlockIndexHotStatus BlockIndexAuthoritativeLive::Materialize(
    const uint256& hash, BlockIndexHotHandle* out) const
{
    if (!impl_->open || !impl_->tail)
        return BlockIndexHotStatus::AUTHORITY_MISSING;
    if (!out)
        return BlockIndexHotStatus::MATERIALIZATION_UNAVAILABLE;
    return impl_->tail->Pin(BlockIndexLogicalId(hash), out);
}

bool BlockIndexAuthoritativeLive::AcceptActive(const BlockIndexRecord& rec,
                                                const BlockIndexDerivedEntry& derived,
                                                int activeHeight,
                                                std::string* error)
{
    if (!impl_->open || !impl_->seam)
        return SetError(error, "authoritative-live: not open");
    BlockIndexTipAppend a;
    a.record = rec;
    a.derived = derived;
    int nt = impl_->seam->AcceptActive(a, activeHeight, error);
    if (nt < 0)
        return false;
    // effective tip = nt
    ClearError(error);
    return true;
}

bool BlockIndexAuthoritativeLive::AcceptSide(const BlockIndexRecord& rec,
                                              const BlockIndexDerivedEntry& derived,
                                              std::string* error)
{
    if (!impl_->open || !impl_->seam)
        return SetError(error, "authoritative-live: not open");
    BlockIndexTipAppend a;
    a.record = rec;
    a.derived = derived;
    BlockIndexTipStatus st = impl_->seam->AcceptSide(a, error);
    return st == BLOCK_INDEX_TIP_OK;
}

bool BlockIndexAuthoritativeLive::RecordOrphan(const BlockIndexRecord& rec,
                                                const BlockIndexDerivedEntry& derived,
                                                std::string* error)
{
    if (!impl_->open || !impl_->seam)
        return SetError(error, "authoritative-live: not open");
    BlockIndexTipAppend a;
    a.record = rec;
    a.derived = derived;
    BlockIndexTipStatus st = impl_->seam->RecordOrphan(a, error);
    return st == BLOCK_INDEX_TIP_OK;
}

bool BlockIndexAuthoritativeLive::ReorgTo(
    int32_t forkHeight,
    const std::vector<BlockIndexRecord>& newBranchRecs,
    const std::vector<BlockIndexDerivedEntry>& newBranchDerived,
    const std::vector<int32_t>& newHeights,
    std::string* error)
{
    if (!impl_->open || !impl_->seam)
        return SetError(error, "authoritative-live: not open");
    if (newBranchRecs.size() != newBranchDerived.size() ||
        newBranchRecs.size() != newHeights.size())
        return SetError(error, "authoritative-live: reorg branch size mismatch");
    std::vector<BlockIndexTipAppend> branch(newBranchRecs.size());
    for (size_t i = 0; i < branch.size(); ++i)
    {
        branch[i].record = newBranchRecs[i];
        branch[i].derived = newBranchDerived[i];
    }
    BlockIndexTipStatus st = impl_->seam->ReorgTo(forkHeight, branch, newHeights, error);
    if (st != BLOCK_INDEX_TIP_OK)
        return false;
    ClearError(error);
    return true;
}

const BlockIndexTipAuthority* BlockIndexAuthoritativeLive::TipAuthority() const
{
    return impl_->tip.get();
}

BlockIndexTipAuthority* BlockIndexAuthoritativeLive::TipAuthorityMutable()
{
    return impl_->tip.get();
}

const BlockIndexLiveTail& BlockIndexAuthoritativeLive::Tail() const
{
    static BlockIndexLiveTail s_empty;
    return impl_->tail ? *impl_->tail : s_empty;
}

int BlockIndexAuthoritativeLive::Horizon() const
{
    return impl_->horizon;
}

uint64_t BlockIndexAuthoritativeLive::BaseGeneration() const
{
    return impl_->baseGeneration;
}

bool BlockIndexAuthoritativeLive::IsOpen() const
{
    return impl_->open;
}

void BlockIndexAuthoritativeLive::Close()
{
    if (impl_->seam)
    {
        impl_->seam.reset();
    }
    if (impl_->tail)
    {
        impl_->tail.reset();
    }
    if (impl_->tip)
    {
        impl_->tip->Close();
        impl_->tip.reset();
    }
    impl_->baseReader = NULL;
    impl_->open = false;
}