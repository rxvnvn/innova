#include "cold_hot_seam.h"
#include "kernel.h" // GetStakeModifierSelectionInterval, nModifierInterval

namespace {
static bool SeamFail(std::string* error, const std::string& text)
{
    if (error)
        *error = text;
    return false;
}

static void SeamClear(std::string* error)
{
    if (error)
        error->clear();
}
}

ColdHotSeamNavigator::ColdHotSeamNavigator() : testHotResolver(NULL), open(false)
{
}

void ColdHotSeamNavigator::SetTestHotResolver(const ColdHotHotResolver* resolver)
{
    testHotResolver = resolver;
}

bool ColdHotSeamNavigator::Open(const std::string& v2Root,
                                 const BlockIndexV2ReaderOptions& options,
                                 std::string* error)
{
    Close();
    if (!coldReader.Open(v2Root, options, error))
        return false;
    open = true;
    return true;
}

void ColdHotSeamNavigator::Close()
{
    coldReader.Close();
    open = false;
}

bool ColdHotSeamNavigator::IsOpen() const
{
    return open && coldReader.IsOpen();
}

uint64_t ColdHotSeamNavigator::ColdGeneration() const
{
    return IsOpen() ? coldReader.Generation() : 0;
}

BlockIndexSnapshot ColdHotSeamNavigator::GetColdTip() const
{
    return IsOpen() ? coldReader.GetTip() : BlockIndexSnapshot();
}

BlockIndexSnapshot ColdHotSeamNavigator::GetHotTip() const
{
    AssertLockHeld(cs_main);
    return testHotResolver ? testHotResolver->GetTip() : hotAccessor.GetTip();
}

bool ColdHotSeamNavigator::VerifySeam(std::string* error) const
{
    AssertLockHeld(cs_main);
    if (!IsOpen())
        return SeamFail(error, "cold/hot navigator is not open");
    if (coldReader.CurrentSelectionChanged(error))
        return SeamFail(error, "selected V2 generation changed");

    const BlockIndexSnapshot coldTip = coldReader.GetTip();
    if (!coldTip.found || !coldTip.fInMainChain)
        return SeamFail(error, "cold generation has no valid active tip");
    const BlockIndexSnapshot hotAtSeam = testHotResolver ? testHotResolver->GetActiveByHeight(coldTip.height) : hotAccessor.GetActiveByHeight(coldTip.height);
    if (!hotAtSeam.found)
        return SeamFail(error, "live active chain does not reach cold seam");
    if (hotAtSeam.hash != coldTip.hash)
        return SeamFail(error, "cold/hot seam hash mismatch");
    SeamClear(error);
    return true;
}

bool ColdHotSeamNavigator::MakeCold(const BlockIndexSnapshot& snapshot,
                                    ColdHotSeamSnapshot* out,
                                    std::string* error) const
{
    if (!out || !snapshot.found || snapshot.id == BLOCK_INDEX_ID_INVALID)
        return SeamFail(error, "invalid cold snapshot");
    const BlockIndexNavigationRef ref = BlockIndexNavigationRef::Cold(
        BlockIndexLogicalId(snapshot.hash), coldReader.Generation(), snapshot.id);
    if (!ref.IsValid())
        return SeamFail(error, "invalid cold navigation reference");
    out->ref = ref;
    out->snapshot = snapshot;
    SeamClear(error);
    return true;
}

bool ColdHotSeamNavigator::MakeHot(const BlockIndexSnapshot& snapshot,
                                   ColdHotSeamSnapshot* out,
                                   std::string* error) const
{
    if (!out || !snapshot.found)
        return SeamFail(error, "invalid hot snapshot");
    const BlockIndexNavigationRef ref = BlockIndexNavigationRef::Hot(
        BlockIndexLogicalId(snapshot.hash));
    if (!ref.IsValid())
        return SeamFail(error, "invalid hot navigation reference");
    out->ref = ref;
    out->snapshot = snapshot;
    SeamClear(error);
    return true;
}

bool ColdHotSeamNavigator::LookupCold(const BlockIndexLogicalId& logical,
                                      ColdHotSeamSnapshot* out,
                                      std::string* error) const
{
    AssertLockHeld(cs_main);
    if (!logical.IsValid() || !VerifySeam(error))
        return false;
    BlockIndexSnapshot snapshot;
    if (coldReader.LookupByHash(logical.GetHash(), &snapshot, error) != BLOCK_INDEX_V2_READ_FOUND)
        return false;
    if (snapshot.hash != logical.GetHash())
        return SeamFail(error, "cold hash resolver mismatch");
    return MakeCold(snapshot, out, error);
}

bool ColdHotSeamNavigator::LookupHot(const BlockIndexLogicalId& logical,
                                     ColdHotSeamSnapshot* out,
                                     std::string* error) const
{
    AssertLockHeld(cs_main);
    if (!logical.IsValid())
        return SeamFail(error, "invalid logical block identity");
    const BlockIndexSnapshot snapshot = testHotResolver ? testHotResolver->LookupByHash(logical.GetHash()) : hotAccessor.LookupByHash(logical.GetHash());
    if (!snapshot.found)
        return SeamFail(error, "hot logical block identity not found");
    if (snapshot.hash != logical.GetHash())
        return SeamFail(error, "hot hash resolver mismatch");
    return MakeHot(snapshot, out, error);
}

bool ColdHotSeamNavigator::Resolve(const BlockIndexNavigationRef& ref,
                                   ColdHotSeamSnapshot* out,
                                   std::string* error) const
{
    AssertLockHeld(cs_main);
    if (!ref.IsValid())
        return SeamFail(error, "invalid navigation reference");
    if (ref.IsHot())
        return LookupHot(ref.logical, out, error);

    if (!ref.MatchesGeneration(coldReader.Generation()))
        return SeamFail(error, "cold reference generation mismatch");
    if (!VerifySeam(error))
        return false;

    BlockIndexSnapshot snapshot;
    if (coldReader.GetRecordById(ref.recordId, &snapshot, error) != BLOCK_INDEX_V2_READ_FOUND)
        return false;
    if (snapshot.hash != ref.logical.GetHash())
        return SeamFail(error, "cold record/logical identity mismatch");
    return MakeCold(snapshot, out, error);
}

bool ColdHotSeamNavigator::IsAtColdTip(const BlockIndexSnapshot& snapshot) const
{
    const BlockIndexSnapshot tip = coldReader.GetTip();
    return snapshot.found && tip.found && snapshot.hash == tip.hash &&
           snapshot.id == tip.id && snapshot.height == tip.height;
}

bool ColdHotSeamNavigator::GetParent(const BlockIndexNavigationRef& ref,
                                     ColdHotSeamSnapshot* out,
                                     std::string* error) const
{
    ColdHotSeamSnapshot current;
    if (!Resolve(ref, &current, error) || !current.snapshot.hasParent)
        return false;

    if (current.ref.IsCold())
    {
        BlockIndexSnapshot parent;
        if (coldReader.GetParent(current.snapshot.id, &parent, error) != BLOCK_INDEX_V2_READ_FOUND)
            return false;
        return MakeCold(parent, out, error);
    }

    const BlockIndexSnapshot parent = testHotResolver ? testHotResolver->GetParentByHash(current.snapshot.hash) : hotAccessor.GetParent(current.snapshot.id);
    if (!parent.found)
        return SeamFail(error, "hot parent not found");

    // A hot active block whose parent lies in a valid frozen prefix crosses to
    // cold only after hash-based validation. Side branches remain hot.
    const BlockIndexSnapshot coldTip = coldReader.GetTip();
    if (parent.fInMainChain && parent.height <= coldTip.height)
    {
        ColdHotSeamSnapshot coldParent;
        if (!LookupCold(BlockIndexLogicalId(parent.hash), &coldParent, error))
            return false;
        return MakeCold(coldParent.snapshot, out, error);
    }
    return MakeHot(parent, out, error);
}

bool ColdHotSeamNavigator::GetAncestor(const BlockIndexNavigationRef& ref,
                                       int targetHeight,
                                       ColdHotSeamSnapshot* out,
                                       std::string* error) const
{
    ColdHotSeamSnapshot current;
    if (!Resolve(ref, &current, error))
        return false;
    if (targetHeight < 0 || targetHeight > current.snapshot.height)
        return SeamFail(error, "invalid ancestor target height");

    // Cold active blocks resolve ancestor in O(1) via active.dat (the reader's
    // own fast path), never an O(depth) parent walk.
    if (current.ref.IsCold())
    {
        BlockIndexSnapshot ancestor;
        if (coldReader.GetAncestor(current.snapshot.id, targetHeight, &ancestor, error) != BLOCK_INDEX_V2_READ_FOUND)
            return false;
        return MakeCold(ancestor, out, error);
    }

    // Hot block: if the whole ancestor path stays above the seam and the block
    // is active, resolve by height in O(1); otherwise fall back to a bounded
    // parent walk that crosses to cold at the seam.
    const BlockIndexSnapshot coldTip = coldReader.GetTip();
    if (current.snapshot.fInMainChain && targetHeight > coldTip.height)
    {
        const BlockIndexSnapshot ancestor = testHotResolver ? testHotResolver->GetActiveByHeight(targetHeight) : hotAccessor.GetActiveByHeight(targetHeight);
        if (!ancestor.found)
            return SeamFail(error, "hot ancestor not found");
        return MakeHot(ancestor, out, error);
    }
    while (current.snapshot.height > targetHeight)
    {
        if (!GetParent(current.ref, &current, error))
            return false;
    }
    if (out)
        *out = current;
    SeamClear(error);
    return true;
}

bool ColdHotSeamNavigator::GetNextActive(const BlockIndexNavigationRef& ref,
                                         ColdHotSeamSnapshot* out,
                                         std::string* error) const
{
    ColdHotSeamSnapshot current;
    if (!Resolve(ref, &current, error))
        return false;

    if (current.ref.IsCold())
    {
        if (!current.snapshot.fInMainChain)
            return SeamFail(error, "side block has no active successor");
        if (!IsAtColdTip(current.snapshot))
        {
            BlockIndexSnapshot next;
            if (coldReader.GetNextActive(current.snapshot.id, &next, error) != BLOCK_INDEX_V2_READ_FOUND)
                return false;
            return MakeCold(next, out, error);
        }
        if (!VerifySeam(error))
            return false;
        const BlockIndexSnapshot next = testHotResolver ? testHotResolver->GetActiveByHeight(current.snapshot.height + 1) : hotAccessor.GetActiveByHeight(current.snapshot.height + 1);
        if (!next.found)
            return SeamFail(error, "cold seam is current live tip");
        if (next.hashPrev != current.snapshot.hash)
            return SeamFail(error, "hot successor does not extend cold seam");
        return MakeHot(next, out, error);
    }

    const BlockIndexSnapshot next = testHotResolver ? testHotResolver->GetNextActiveByHash(current.snapshot.hash) : hotAccessor.GetNextActive(current.snapshot.id);
    if (!next.found)
        return SeamFail(error, "hot block has no active successor");
    const BlockIndexSnapshot coldTip = coldReader.GetTip();
    if (next.fInMainChain && next.height <= coldTip.height)
    {
        ColdHotSeamSnapshot coldNext;
        if (!LookupCold(BlockIndexLogicalId(next.hash), &coldNext, error))
            return false;
        return MakeCold(coldNext.snapshot, out, error);
    }
    return MakeHot(next, out, error);
}

bool ColdHotSeamNavigator::GetStakingMetadata(const BlockIndexNavigationRef& ref,
                                               BlockIndexStakingMetadata* out,
                                               std::string* error) const
{
    if (!out)
        return SeamFail(error, "null staking metadata output");
    ColdHotSeamSnapshot current;
    if (!Resolve(ref, &current, error))
        return false;
    if (current.ref.IsHot())
    {
        out->hasStakeModifierTime = current.snapshot.hasStakeModifierTime;
        out->hasStakeModifierChecksum = current.snapshot.hasStakeModifierChecksum;
        out->nStakeModifierTime = current.snapshot.nStakeModifierTime;
        out->nStakeModifierChecksum = current.snapshot.nStakeModifierChecksum;
        if (!out->hasStakeModifierTime || !out->hasStakeModifierChecksum)
            return SeamFail(error, "hot staking metadata unavailable");
        SeamClear(error);
        return true;
    }
    if (coldReader.GetStakingMetadata(current.snapshot.id, out, error) != BLOCK_INDEX_V2_READ_FOUND)
        return false;
    if (!out->hasStakeModifierTime || !out->hasStakeModifierChecksum)
        return SeamFail(error, "cold staking metadata unavailable");
    return true;
}

bool ColdHotSeamNavigator::GetStakeModifierTime(const BlockIndexNavigationRef& ref,
                                                int64_t* out,
                                                std::string* error) const
{
    if (!out)
        return SeamFail(error, "null modifier time output");
    ColdHotSeamSnapshot current;
    if (!Resolve(ref, &current, error))
        return false;
    if (current.ref.IsHot())
    {
        if (!current.snapshot.hasStakeModifierTime)
            return SeamFail(error, "hot modifier time unavailable");
        *out = (int64_t)current.snapshot.nStakeModifierTime;
        SeamClear(error);
        return true;
    }
    if (coldReader.GetStakeModifierTime(current.snapshot.id, out, error) != BLOCK_INDEX_V2_READ_FOUND)
        return false;
    return true;
}

bool ColdHotSeamNavigator::GetStakeModifierChecksum(const BlockIndexNavigationRef& ref,
                                                    unsigned int* out,
                                                    std::string* error) const
{
    if (!out)
        return SeamFail(error, "null modifier checksum output");
    ColdHotSeamSnapshot current;
    if (!Resolve(ref, &current, error))
        return false;
    if (current.ref.IsHot())
    {
        if (!current.snapshot.hasStakeModifierChecksum)
            return SeamFail(error, "hot modifier checksum unavailable");
        *out = current.snapshot.nStakeModifierChecksum;
        SeamClear(error);
        return true;
    }
    if (coldReader.GetStakeModifierChecksum(current.snapshot.id, out, error) != BLOCK_INDEX_V2_READ_FOUND)
        return false;
    return true;
}

bool ColdHotSeamNavigator::ResolveLogical(const BlockIndexLogicalId& logical,
                                          ColdHotSeamSnapshot* out,
                                          std::string* error) const
{
    if (!logical.IsValid())
        return SeamFail(error, "invalid logical block identity");
    // Cold first; a cold block is authoritative for the frozen prefix.
    if (LookupCold(logical, out, error))
        return true;
    // A cold-side miss is an empty/"not found" (Snapshot not found), not an
    // authoritative failure; a hash check / unresolvable cold means not cold.
    if (LookupHot(logical, out, error))
        return true;
    // Distinguish "present but unusable" from "absent": both fail; the caller
    // sees a not-found (out left untouched/found=false) or an explicit error.
    return false;
}

bool ColdHotSeamNavigator::GetLastStakeModifier(const BlockIndexLogicalId& start,
                                                uint64_t* nStakeModifier,
                                                int64_t* nModifierTime,
                                                std::string* error) const
{
    if (!nStakeModifier || !nModifierTime)
        return SeamFail(error, "null GetLastStakeModifier output");
    AssertLockHeld(cs_main);
    ColdHotSeamSnapshot cur;
    if (!ResolveLogical(start, &cur, error))
        return SeamFail(error, "GetLastStakeModifier: block not resolvable");

    // Walk parents until a generated-modifier block. Mirrors kernel.cpp
    // GetLastStakeModifier exactly: advance while pprev exists and the current
    // block has not generated a modifier.
    while (cur.snapshot.hasParent &&
           !(cur.snapshot.nFlags & CBlockIndex::BLOCK_STAKE_MODIFIER))
    {
        ColdHotSeamSnapshot parent;
        if (!GetParent(cur.ref, &parent, error))
            return false;
        cur = parent;
    }
    if (!(cur.snapshot.nFlags & CBlockIndex::BLOCK_STAKE_MODIFIER))
        return SeamFail(error, "GetLastStakeModifier: no generation at genesis block");
    *nStakeModifier = cur.snapshot.nStakeModifier;
    *nModifierTime = (int64_t)cur.snapshot.nTime;
    SeamClear(error);
    return true;
}

bool ColdHotSeamNavigator::GetKernelStakeModifier(const BlockIndexLogicalId& source,
                                                  uint64_t* nStakeModifier,
                                                  int* nStakeModifierHeight,
                                                  int64_t* nStakeModifierTime,
                                                  bool fPrintProofOfStake,
                                                  std::string* error) const
{
    if (!nStakeModifier || !nStakeModifierHeight || !nStakeModifierTime)
        return SeamFail(error, "null GetKernelStakeModifier output");
    AssertLockHeld(cs_main);
    *nStakeModifier = 0;
    ColdHotSeamSnapshot from;
    if (!ResolveLogical(source, &from, error))
        return SeamFail(error, "GetKernelStakeModifier() : block not indexed");

    *nStakeModifierHeight = from.snapshot.height;
    *nStakeModifierTime = (int64_t)from.snapshot.nTime;
    const int64_t nStakeModifierSelectionInterval = GetStakeModifierSelectionInterval();

    // Forward active-chain walk from the source until the modifier is selected
    // a selection-interval later. Mirrors kernel.cpp GetKernelStakeModifier
    // (2-arg) exactly; GetNextActive handles cold->cold and cold->hot seam.
    ColdHotSeamSnapshot pindex = from;
    while (*nStakeModifierTime < (int64_t)from.snapshot.nTime + nStakeModifierSelectionInterval)
    {
        ColdHotSeamSnapshot next;
        if (!GetNextActive(pindex.ref, &next, error))
        {
            // Reached the active tip / no successor. Mirrors legacy "reached
            // best block" branch.
            if ((int64_t)pindex.snapshot.nTime >= (int64_t)from.snapshot.nTime + nStakeModifierSelectionInterval)
            {
                *nStakeModifier = pindex.snapshot.nStakeModifier;
                *nStakeModifierHeight = pindex.snapshot.height;
                *nStakeModifierTime = (int64_t)pindex.snapshot.nTime;
                SeamClear(error);
                return true;
            }
            if (fPrintProofOfStake ||
                ((int64_t)pindex.snapshot.nTime + (int64_t)nStakeMinAge - nStakeModifierSelectionInterval > GetAdjustedTime()))
                return SeamFail(error, strprintf("GetKernelStakeModifier() : reached best block from block %s",
                                source.GetHash().ToString().c_str()));
            return false;
        }
        pindex = next;
        if (pindex.snapshot.nFlags & CBlockIndex::BLOCK_STAKE_MODIFIER)
        {
            *nStakeModifierHeight = pindex.snapshot.height;
            *nStakeModifierTime = (int64_t)pindex.snapshot.nTime;
        }
    }

    *nStakeModifier = pindex.snapshot.nStakeModifier;
    SeamClear(error);
    return true;
}

bool ColdHotSeamNavigator::GetKernelStakeModifier(const BlockIndexLogicalId& source,
                                                  const BlockIndexLogicalId& branchTip,
                                                  uint64_t* nStakeModifier,
                                                  int* nStakeModifierHeight,
                                                  int64_t* nStakeModifierTime,
                                                  bool fPrintProofOfStake,
                                                  std::string* error) const
{
    if (!nStakeModifier || !nStakeModifierHeight || !nStakeModifierTime)
        return SeamFail(error, "null GetKernelStakeModifier output");
    AssertLockHeld(cs_main);

    // Resolve source and candidate-branch tip (the hot block being validated).
    ColdHotSeamSnapshot from, tip;
    if (!ResolveLogical(source, &from, error))
        return SeamFail(error, "GetKernelStakeModifier() : block not indexed");
    if (!ResolveLogical(branchTip, &tip, error))
        return SeamFail(error, "GetKernelStakeModifier() : candidate prev not indexed");

    // Backward ancestor path from branch tip down to the source. Mirrors
    // kernel.cpp 3-arg walk (pindexPrev->pprev until source). Both are
    // by-value parent steps (cold reader or hot resolver), so no arbitrary
    // historical CBlockIndex residency is required.
    std::vector<ColdHotSeamSnapshot> path;
    path.push_back(tip);
    ColdHotSeamSnapshot cur = tip;
    bool found = (tip.snapshot.hash == source.GetHash() &&
                  tip.snapshot.height == from.snapshot.height);
    while (!found && cur.snapshot.hasParent)
    {
        ColdHotSeamSnapshot parent;
        if (!GetParent(cur.ref, &parent, error))
            return false;
        cur = parent;
        path.push_back(cur);
        if (cur.snapshot.hash == source.GetHash() && cur.snapshot.height == from.snapshot.height)
            found = true;
    }
    if (!found || path.empty() || path.back().snapshot.hash != source.GetHash())
        return SeamFail(error, "GetKernelStakeModifier() : stake source is not an ancestor of candidate branch");
    std::reverse(path.begin(), path.end());

    *nStakeModifier = from.snapshot.nStakeModifier;
    *nStakeModifierHeight = from.snapshot.height;
    *nStakeModifierTime = (int64_t)from.snapshot.nTime;
    const int64_t nTargetTime = (int64_t)from.snapshot.nTime + GetStakeModifierSelectionInterval();

    for (size_t i = 1; i < path.size(); ++i)
    {
        const ColdHotSeamSnapshot& pindex = path[i];
        if (pindex.snapshot.nFlags & CBlockIndex::BLOCK_STAKE_MODIFIER)
        {
            *nStakeModifierHeight = pindex.snapshot.height;
            *nStakeModifierTime = (int64_t)pindex.snapshot.nTime;
        }
        if (*nStakeModifierTime >= nTargetTime)
        {
            *nStakeModifier = pindex.snapshot.nStakeModifier;
            SeamClear(error);
            return true;
        }
    }
    const ColdHotSeamSnapshot& pindexTip = path.back();
    if (pindexTip.snapshot.hash == tip.snapshot.hash &&
        (int64_t)pindexTip.snapshot.nTime >= nTargetTime)
    {
        *nStakeModifier = pindexTip.snapshot.nStakeModifier;
        *nStakeModifierHeight = pindexTip.snapshot.height;
        *nStakeModifierTime = (int64_t)pindexTip.snapshot.nTime;
        SeamClear(error);
        return true;
    }
    if (fPrintProofOfStake)
        return SeamFail(error, "GetKernelStakeModifier() : candidate branch ends before selection interval");
    return false;
}
