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

ColdHotSeamResult ColdHotSeamResultFromReadStatus(int readStatus)
{
    switch (readStatus)
    {
    case BLOCK_INDEX_V2_READ_FOUND:
        return COLD_HOT_SEAM_OK;
    case BLOCK_INDEX_V2_READ_NOT_FOUND:
        return COLD_HOT_SEAM_NOT_FOUND;
    default:
        // CORRUPT / IO_ERROR / NOT_OPEN are all authority failures: they never
        // justify a silent fall back to legacy historical residency.
        return COLD_HOT_SEAM_AUTHORITY_FAILURE;
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
    return ResolveR(ref, out, error) == COLD_HOT_SEAM_OK;
}

ColdHotSeamResult ColdHotSeamNavigator::ResolveR(const BlockIndexNavigationRef& ref,
                                                 ColdHotSeamSnapshot* out,
                                                 std::string* error) const
{
    AssertLockHeld(cs_main);
    if (!ref.IsValid())
        return COLD_HOT_SEAM_AUTHORITY_FAILURE;
    if (ref.IsHot())
        return LookupHot(ref.logical, out, error) ? COLD_HOT_SEAM_OK : COLD_HOT_SEAM_AUTHORITY_FAILURE;

    if (!ref.MatchesGeneration(coldReader.Generation()))
        return COLD_HOT_SEAM_AUTHORITY_FAILURE;
    if (!coldReader.IsOpen())
        return COLD_HOT_SEAM_AUTHORITY_FAILURE;
    // A CURRENT replacement invalidates references bound to the pinned
    // generation: fail closed rather than resolve stale refs. (Verified per
    // resolution; the original A.9a.3b navigator did the same at 120k scale.)
    if (coldReader.CurrentSelectionChanged(error))
        return COLD_HOT_SEAM_AUTHORITY_FAILURE;

    // The cold reader is pinned to the generation it was opened on; it does not
    // auto-switch. Resolve by record id (reader cache/LRU friendly) rather than
    // a cold hash-index lookup.
    BlockIndexSnapshot snapshot;
    const BlockIndexV2ReadStatus st = coldReader.GetRecordById(ref.recordId, &snapshot, error);
    if (st != BLOCK_INDEX_V2_READ_FOUND)
        return ColdHotSeamResultFromReadStatus(st);
    if (snapshot.found && snapshot.hash != ref.logical.GetHash())
        return COLD_HOT_SEAM_AUTHORITY_FAILURE;
    return MakeCold(snapshot, out, error) ? COLD_HOT_SEAM_OK : COLD_HOT_SEAM_AUTHORITY_FAILURE;
}

bool ColdHotSeamNavigator::IsAtColdTip(const BlockIndexSnapshot& snapshot) const
{
    const BlockIndexSnapshot tip = coldReader.GetTip();
    return snapshot.found && tip.found && snapshot.hash == tip.hash &&
           snapshot.id == tip.id && snapshot.height == tip.height;
}

ColdHotSeamResult ColdHotSeamNavigator::GetParentR(const BlockIndexNavigationRef& ref,
                                                   ColdHotSeamSnapshot* out,
                                                   std::string* error) const
{
    ColdHotSeamSnapshot current;
    {
        const ColdHotSeamResult r = ResolveR(ref, &current, error);
        if (r != COLD_HOT_SEAM_OK)
            return r;
    }
    if (!current.snapshot.hasParent)
    {
        if (out)
            *out = ColdHotSeamSnapshot();
        SeamClear(error);
        return COLD_HOT_SEAM_NOT_FOUND;
    }

    if (current.ref.IsCold())
    {
        BlockIndexSnapshot parent;
        const BlockIndexV2ReadStatus st = coldReader.GetParent(current.snapshot.id, &parent, error);
        if (st != BLOCK_INDEX_V2_READ_FOUND)
            return ColdHotSeamResultFromReadStatus(st);
        return MakeCold(parent, out, error) ? COLD_HOT_SEAM_OK : COLD_HOT_SEAM_AUTHORITY_FAILURE;
    }

    const BlockIndexSnapshot parent = testHotResolver ? testHotResolver->GetParentByHash(current.snapshot.hash) : hotAccessor.GetParent(current.snapshot.id);
    if (!parent.found)
        return SeamFail(error, "hot parent not found") ? COLD_HOT_SEAM_NOT_FOUND : COLD_HOT_SEAM_NOT_FOUND;

    // A hot active block whose parent lies in a valid frozen prefix crosses to
    // cold only after hash-based validation. Side branches remain hot.
    // DOMAIN SAFETY: the parent MUST be PROVEN COLD before MakeCold. A cold
    // miss here (the parent is on the live main chain at/below the frozen
    // seam but is absent from the generation) is a seam divergence and FAILS
    // CLOSED; the hot fallback snapshot must never be rebound as a cold
    // generation RecordId.
    const BlockIndexSnapshot coldTip = coldReader.GetTip();
    if (parent.fInMainChain && parent.height <= coldTip.height)
    {
        ColdHotSeamSnapshot coldParent;
        const ColdHotSeamResult cr = ResolveColdLogicalR(BlockIndexLogicalId(parent.hash), &coldParent, error);
        if (cr != COLD_HOT_SEAM_OK)
            return COLD_HOT_SEAM_AUTHORITY_FAILURE; // cold miss/divergence -> fail closed
        return MakeCold(coldParent.snapshot, out, error) ? COLD_HOT_SEAM_OK : COLD_HOT_SEAM_AUTHORITY_FAILURE;
    }
    return MakeHot(parent, out, error) ? COLD_HOT_SEAM_OK : COLD_HOT_SEAM_AUTHORITY_FAILURE;
}

ColdHotSeamResult ColdHotSeamNavigator::GetAncestorR(const BlockIndexNavigationRef& ref,
                                                     int targetHeight,
                                                     ColdHotSeamSnapshot* out,
                                                     std::string* error) const
{
    ColdHotSeamSnapshot current;
    {
        const ColdHotSeamResult r = ResolveR(ref, &current, error);
        if (r != COLD_HOT_SEAM_OK)
            return r;
    }
    if (targetHeight < 0 || targetHeight > current.snapshot.height)
        return SeamFail(error, "invalid ancestor target height") ? COLD_HOT_SEAM_NOT_FOUND : COLD_HOT_SEAM_NOT_FOUND;

    // Cold active blocks resolve ancestor in O(1) via active.dat (the reader's
    // own fast path), never an O(depth) parent walk.
    if (current.ref.IsCold())
    {
        BlockIndexSnapshot ancestor;
        const BlockIndexV2ReadStatus st = coldReader.GetAncestor(current.snapshot.id, targetHeight, &ancestor, error);
        if (st != BLOCK_INDEX_V2_READ_FOUND)
            return ColdHotSeamResultFromReadStatus(st);
        return MakeCold(ancestor, out, error) ? COLD_HOT_SEAM_OK : COLD_HOT_SEAM_AUTHORITY_FAILURE;
    }

    const BlockIndexSnapshot coldTip = coldReader.GetTip();
    if (current.snapshot.fInMainChain && targetHeight > coldTip.height)
    {
        const BlockIndexSnapshot ancestor = testHotResolver ? testHotResolver->GetActiveByHeight(targetHeight) : hotAccessor.GetActiveByHeight(targetHeight);
        if (!ancestor.found)
            return SeamFail(error, "hot ancestor not found") ? COLD_HOT_SEAM_NOT_FOUND : COLD_HOT_SEAM_NOT_FOUND;
        return MakeHot(ancestor, out, error) ? COLD_HOT_SEAM_OK : COLD_HOT_SEAM_AUTHORITY_FAILURE;
    }
    while (current.snapshot.height > targetHeight)
    {
        const ColdHotSeamResult r = GetParentR(current.ref, &current, error);
        if (r != COLD_HOT_SEAM_OK)
            return r;
    }
    if (out)
        *out = current;
    SeamClear(error);
    return COLD_HOT_SEAM_OK;
}

ColdHotSeamResult ColdHotSeamNavigator::GetNextActiveR(const BlockIndexNavigationRef& ref,
                                                       ColdHotSeamSnapshot* out,
                                                       std::string* error) const
{
    ColdHotSeamSnapshot current;
    {
        const ColdHotSeamResult r = ResolveR(ref, &current, error);
        if (r != COLD_HOT_SEAM_OK)
            return r;
    }

    if (current.ref.IsCold())
    {
        if (!current.snapshot.fInMainChain)
        {
            // Side cold block: no active successor (legacy pnext==NULL).
            SeamClear(error);
            return COLD_HOT_SEAM_END_OF_ACTIVE_CHAIN;
        }
        if (!IsAtColdTip(current.snapshot))
        {
            BlockIndexSnapshot next;
            const BlockIndexV2ReadStatus st = coldReader.GetNextActive(current.snapshot.id, &next, error);
            if (st == BLOCK_INDEX_V2_READ_NOT_FOUND)
                return COLD_HOT_SEAM_END_OF_ACTIVE_CHAIN; // cold tip / no successor
            if (st != BLOCK_INDEX_V2_READ_FOUND)
                return ColdHotSeamResultFromReadStatus(st); // corrupt -> fail closed
            return MakeCold(next, out, error) ? COLD_HOT_SEAM_OK : COLD_HOT_SEAM_AUTHORITY_FAILURE;
        }
        if (!VerifySeam(error))
            return COLD_HOT_SEAM_AUTHORITY_FAILURE; // seam failure -> fail closed
        const BlockIndexSnapshot next = testHotResolver ? testHotResolver->GetActiveByHeight(current.snapshot.height + 1) : hotAccessor.GetActiveByHeight(current.snapshot.height + 1);
        if (!next.found)
            return COLD_HOT_SEAM_END_OF_ACTIVE_CHAIN; // cold seam is current live tip
        if (next.hashPrev != current.snapshot.hash)
            return SeamFail(error, "hot successor does not extend cold seam") ? COLD_HOT_SEAM_AUTHORITY_FAILURE : COLD_HOT_SEAM_AUTHORITY_FAILURE;
        return MakeHot(next, out, error) ? COLD_HOT_SEAM_OK : COLD_HOT_SEAM_AUTHORITY_FAILURE;
    }

    const BlockIndexSnapshot next = testHotResolver ? testHotResolver->GetNextActiveByHash(current.snapshot.hash) : hotAccessor.GetNextActive(current.snapshot.id);
    if (!next.found)
        return COLD_HOT_SEAM_END_OF_ACTIVE_CHAIN; // genuine successor absence
    const BlockIndexSnapshot coldTip = coldReader.GetTip();
    if (next.fInMainChain && next.height <= coldTip.height)
    {
        // DOMAIN SAFETY: the successor MUST be PROVEN COLD before MakeCold; a
        // cold miss here is a seam divergence and fails closed (never rebind a
        // hot snapshot's process-local id as a cold generation RecordId).
        ColdHotSeamSnapshot coldNext;
        const ColdHotSeamResult cr = ResolveColdLogicalR(BlockIndexLogicalId(next.hash), &coldNext, error);
        if (cr != COLD_HOT_SEAM_OK)
            return COLD_HOT_SEAM_AUTHORITY_FAILURE; // cold miss/divergence -> fail closed
        return MakeCold(coldNext.snapshot, out, error) ? COLD_HOT_SEAM_OK : COLD_HOT_SEAM_AUTHORITY_FAILURE;
    }
    return MakeHot(next, out, error) ? COLD_HOT_SEAM_OK : COLD_HOT_SEAM_AUTHORITY_FAILURE;
}

bool ColdHotSeamNavigator::GetParent(const BlockIndexNavigationRef& ref,
                                     ColdHotSeamSnapshot* out,
                                     std::string* error) const
{
    return GetParentR(ref, out, error) == COLD_HOT_SEAM_OK;
}

bool ColdHotSeamNavigator::GetAncestor(const BlockIndexNavigationRef& ref,
                                       int targetHeight,
                                       ColdHotSeamSnapshot* out,
                                       std::string* error) const
{
    return GetAncestorR(ref, targetHeight, out, error) == COLD_HOT_SEAM_OK;
}

bool ColdHotSeamNavigator::GetNextActive(const BlockIndexNavigationRef& ref,
                                         ColdHotSeamSnapshot* out,
                                         std::string* error) const
{
    return GetNextActiveR(ref, out, error) == COLD_HOT_SEAM_OK;
}

ColdHotSeamResult ColdHotSeamNavigator::GetHybridSvmMaturityAuthorityR(
    const BlockIndexLogicalId& sourceBlock,
    const uint256& txHash,
    const std::vector<uint256>& vMerkleBranch,
    int nIndex,
    int* outDepth,
    std::string* error) const
{
    if (!outDepth)
        return COLD_HOT_SEAM_AUTHORITY_FAILURE;
    AssertLockHeld(cs_main);

    // Legacy GetDepthInMainChainINTERNAL sentinel: hashBlock==0 or nIndex==-1
    // returns depth 0 (the caller rejects: 0 < maturity). Preserve exactly.
    if (!sourceBlock.IsValid() || nIndex < 0)
    {
        *outDepth = 0;
        SeamClear(error);
        return COLD_HOT_SEAM_NOT_FOUND;
    }

    // Resolve the historical source block BY-VALUE (stable logical hash) so a
    // deep-cold source needs no resident CBlockIndex. Typed fail-closed result;
    // an AUTHORITY_FAILURE is never converted into a NOT_FOUND, and a logical
    // presence is never conflated with active-chain membership.
    ColdHotSeamSnapshot snap;
    const ColdHotSeamResult r = ResolveLogicalR(sourceBlock, &snap, error);
    if (r != COLD_HOT_SEAM_OK)
    {
        *outDepth = 0;
        // Genuine NOT_FOUND / END_OF_ACTIVE_CHAIN -> legacy depth 0 (reject).
        // AUTHORITY_FAILURE propagates so the caller fails closed.
        if (r != COLD_HOT_SEAM_AUTHORITY_FAILURE)
            SeamClear(error);
        return (r == COLD_HOT_SEAM_AUTHORITY_FAILURE) ? COLD_HOT_SEAM_AUTHORITY_FAILURE
                                                      : COLD_HOT_SEAM_NOT_FOUND;
    }
    if (!snap.IsValid() || !snap.snapshot.found)
    {
        *outDepth = 0;
        SeamFail(error, "HybridSPV maturity: resolved but invalid source snapshot");
        return COLD_HOT_SEAM_AUTHORITY_FAILURE;
    }

    // ACTIVE-chain authority: a V2 record existing by hash is NOT proof that
    // the block is currently active. Require authoritative active membership
    // (cold = O(1) active.dat; hot = pindex->IsInMainChain()). A side branch /
    // known-but-not-active block must NEVER receive positive active depth.
    if (!snap.snapshot.fInMainChain)
    {
        *outDepth = 0;
        SeamClear(error);
        return COLD_HOT_SEAM_NOT_FOUND; // known, not active -> legacy depth 0
    }

    // MERKLE authority: prove the wallet transaction is IN THIS ACTIVE BLOCK,
    // not merely that "the block is active". Exact CheckMerkleBranch root match
    // (identical to legacy GetDepthInMainChainINTERNAL).
    {
        const uint256 computedRoot = CBlock::CheckMerkleBranch(txHash, vMerkleBranch, nIndex);
        if (computedRoot != snap.snapshot.hashMerkleRoot)
        {
            *outDepth = 0;
            SeamClear(error);
            return COLD_HOT_SEAM_NOT_FOUND; // merkle mismatch -> legacy depth 0
        }
    }

    // Authoritative current active tip (live; matches legacy pindexBest).
    const BlockIndexSnapshot tip = GetHotTip();
    if (!tip.found || !tip.fInMainChain)
    {
        SeamFail(error, "HybridSPV maturity: no authoritative active tip");
        return COLD_HOT_SEAM_AUTHORITY_FAILURE;
    }
    if (snap.snapshot.height > tip.height)
    {
        SeamFail(error, "HybridSPV maturity: active source height above active tip");
        return COLD_HOT_SEAM_AUTHORITY_FAILURE;
    }

    // EXACT legacy depth arithmetic: bestHeight - blockHeight + 1.
    *outDepth = tip.height - snap.snapshot.height + 1;
    SeamClear(error);
    return COLD_HOT_SEAM_OK;
}

bool ColdHotSeamNavigator::GetStakingMetadata(const BlockIndexNavigationRef& ref,
                                               BlockIndexStakingMetadata* out,
                                               std::string* error) const
{
    if (!out)
        return SeamFail(error, "null staking metadata output");
    ColdHotSeamSnapshot current;
    if (ResolveLogicalR(ref.logical, &current, error) != COLD_HOT_SEAM_OK)
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
    if (ResolveLogicalR(ref.logical, &current, error) != COLD_HOT_SEAM_OK)
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
    if (ResolveLogicalR(ref.logical, &current, error) != COLD_HOT_SEAM_OK)
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

// ---------------------------------------------------------------------------
// Typed, fail-closed logical resolution (A.9a.3c).
//
// A cold lookup that FOUND a block is authoritative for the frozen prefix. A
// GENUINE cold NOT_FOUND (the block is simply not present in the cold domain —
// e.g. a hot tail block above the seam, or a non-recorded side block) is the
// ONLY outcome eligible for a hot lookup. A cold AUTHORITY_FAILURE (stale
// generation, changed CURRENT, corrupt record/hash index, divergent seam, or
// failed cold->hot crossing) FAILS CLOSED and never falls back to legacy
// historical mapBlockIndex residency.
// ---------------------------------------------------------------------------
ColdHotSeamResult ColdHotSeamNavigator::ResolveLogicalR(const BlockIndexLogicalId& logical,
                                                        ColdHotSeamSnapshot* out,
                                                        std::string* error) const
{
    if (!logical.IsValid())
        return COLD_HOT_SEAM_AUTHORITY_FAILURE;
    AssertLockHeld(cs_main);

    // Cold-first, but ONLY when the cold layer is actually open and its seam
    // verifies (so an authority failure cannot be masked by hot resolution).
    if (IsOpen())
    {
        if (coldReader.CurrentSelectionChanged(error))
            return COLD_HOT_SEAM_AUTHORITY_FAILURE; // stale CURRENT -> fail closed
        BlockIndexSnapshot cold;
        const BlockIndexV2ReadStatus st = coldReader.LookupByHash(logical.GetHash(), &cold, error);
        if (st == BLOCK_INDEX_V2_READ_FOUND)
        {
            if (cold.hash != logical.GetHash())
                return COLD_HOT_SEAM_AUTHORITY_FAILURE; // hash mismatch -> fail closed
            if (!MakeCold(cold, out, error))
                return COLD_HOT_SEAM_AUTHORITY_FAILURE;
            SeamClear(error);
            return COLD_HOT_SEAM_OK;
        }
        if (st != BLOCK_INDEX_V2_READ_NOT_FOUND)
            return COLD_HOT_SEAM_AUTHORITY_FAILURE; // corrupt/io/not-open -> fail closed
        // Genuine cold NOT_FOUND: eligible for hot lookup.
    }

    const BlockIndexSnapshot hot = testHotResolver ? testHotResolver->LookupByHash(logical.GetHash()) : hotAccessor.LookupByHash(logical.GetHash());
    if (!hot.found)
        return COLD_HOT_SEAM_NOT_FOUND;
    if (hot.hash != logical.GetHash())
        return COLD_HOT_SEAM_AUTHORITY_FAILURE;
    if (!MakeHot(hot, out, error))
        return COLD_HOT_SEAM_AUTHORITY_FAILURE;
    SeamClear(error);
    return COLD_HOT_SEAM_OK;
}

ColdHotSeamResult ColdHotSeamNavigator::ResolveColdLogicalR(const BlockIndexLogicalId& logical,
                                                            ColdHotSeamSnapshot* out,
                                                            std::string* error) const
{
    if (!logical.IsValid())
        return COLD_HOT_SEAM_AUTHORITY_FAILURE;
    AssertLockHeld(cs_main);
    if (!IsOpen())
        return COLD_HOT_SEAM_AUTHORITY_FAILURE;
    if (coldReader.CurrentSelectionChanged(error))
        return COLD_HOT_SEAM_AUTHORITY_FAILURE; // stale CURRENT -> fail closed

    BlockIndexSnapshot cold;
    const BlockIndexV2ReadStatus st = coldReader.LookupByHash(logical.GetHash(), &cold, error);
    if (st == BLOCK_INDEX_V2_READ_FOUND)
    {
        if (cold.hash != logical.GetHash())
            return COLD_HOT_SEAM_AUTHORITY_FAILURE; // hash mismatch -> fail closed
        // MakeCold is reached only from a PROVEN-COLD reader result here.
        if (!MakeCold(cold, out, error))
            return COLD_HOT_SEAM_AUTHORITY_FAILURE;
        SeamClear(error);
        return COLD_HOT_SEAM_OK;
    }
    if (st != BLOCK_INDEX_V2_READ_NOT_FOUND)
        return COLD_HOT_SEAM_AUTHORITY_FAILURE; // corrupt/io/not-open -> fail closed
    // Genuine cold miss. The caller is responsible for not treating a hot
    // fallback as a PROVEN-COLD result at a crossing.
    if (error && error->empty())
        *error = "cold logical resolution: block not present in frozen generation";
    return COLD_HOT_SEAM_NOT_FOUND;
}

ColdHotSeamResult ColdHotSeamNavigator::GetLastStakeModifierR(const BlockIndexLogicalId& start,
                                                              uint64_t* nStakeModifier,
                                                              int64_t* nModifierTime,
                                                              std::string* error) const
{
    if (!nStakeModifier || !nModifierTime)
        return COLD_HOT_SEAM_AUTHORITY_FAILURE;
    AssertLockHeld(cs_main);
    ColdHotSeamSnapshot cur;
    const ColdHotSeamResult r0 = ResolveLogicalR(start, &cur, error);
    if (r0 != COLD_HOT_SEAM_OK)
        return r0;

    // Walk parents until a generated-modifier block. Mirrors kernel.cpp
    // GetLastStakeModifier exactly: advance while pprev exists and the current
    // block has not generated a modifier.
    while (cur.snapshot.hasParent &&
           !(cur.snapshot.nFlags & CBlockIndex::BLOCK_STAKE_MODIFIER))
    {
        ColdHotSeamSnapshot parent;
        const ColdHotSeamResult pr = GetParentR(cur.ref, &parent, error);
        if (pr != COLD_HOT_SEAM_OK)
            return pr;
        cur = parent;
    }
    if (!(cur.snapshot.nFlags & CBlockIndex::BLOCK_STAKE_MODIFIER))
        return COLD_HOT_SEAM_NOT_FOUND;
    *nStakeModifier = cur.snapshot.nStakeModifier;
    *nModifierTime = (int64_t)cur.snapshot.nTime;
    SeamClear(error);
    return COLD_HOT_SEAM_OK;
}

bool ColdHotSeamNavigator::GetLastStakeModifier(const BlockIndexLogicalId& start,
                                                uint64_t* nStakeModifier,
                                                int64_t* nModifierTime,
                                                std::string* error) const
{
    return GetLastStakeModifierR(start, nStakeModifier, nModifierTime, error) == COLD_HOT_SEAM_OK;
}

ColdHotSeamResult ColdHotSeamNavigator::GetKernelStakeModifierR(const BlockIndexLogicalId& source,
                                                                uint64_t* nStakeModifier,
                                                                int* nStakeModifierHeight,
                                                                int64_t* nStakeModifierTime,
                                                                bool fPrintProofOfStake,
                                                                std::string* error,
                                                                int* outFinalWalkHeight) const
{
    if (!nStakeModifier || !nStakeModifierHeight || !nStakeModifierTime)
        return COLD_HOT_SEAM_AUTHORITY_FAILURE;
    if (outFinalWalkHeight)
        *outFinalWalkHeight = -1;
    AssertLockHeld(cs_main);
    *nStakeModifier = 0;
    ColdHotSeamSnapshot from;
    const ColdHotSeamResult r0 = ResolveLogicalR(source, &from, error);
    if (r0 != COLD_HOT_SEAM_OK)
        return r0;

    *nStakeModifierHeight = from.snapshot.height;
    *nStakeModifierTime = (int64_t)from.snapshot.nTime;
    const int64_t nStakeModifierSelectionInterval = GetStakeModifierSelectionInterval();

    // Forward active-chain walk from the source until the modifier is selected
    // a selection-interval later. Mirrors kernel.cpp GetKernelStakeModifier
    // (2-arg) exactly. A GENUINE no-successor (END_OF_ACTIVE_CHAIN) may
    // reproduce legacy reached-tip semantics; any navigation AUTHORITY_FAILURE
    // fails closed and never becomes "reached tip".
    ColdHotSeamSnapshot pindex = from;
    while (*nStakeModifierTime < (int64_t)from.snapshot.nTime + nStakeModifierSelectionInterval)
    {
        ColdHotSeamSnapshot next;
        const ColdHotSeamResult nr = GetNextActiveR(pindex.ref, &next, error);
        if (nr == COLD_HOT_SEAM_AUTHORITY_FAILURE)
            return COLD_HOT_SEAM_AUTHORITY_FAILURE; // never tip success
        if (nr != COLD_HOT_SEAM_OK)
        {
            // END_OF_ACTIVE_CHAIN: reached the active tip / no successor.
            // Mirrors legacy "reached best block" branch (pnext == NULL).
            if ((int64_t)pindex.snapshot.nTime >= (int64_t)from.snapshot.nTime + nStakeModifierSelectionInterval)
            {
                *nStakeModifier = pindex.snapshot.nStakeModifier;
                *nStakeModifierHeight = pindex.snapshot.height;
                *nStakeModifierTime = (int64_t)pindex.snapshot.nTime;
                if (outFinalWalkHeight)
                    *outFinalWalkHeight = pindex.snapshot.height;
                SeamClear(error);
                return COLD_HOT_SEAM_OK;
            }
            if (fPrintProofOfStake ||
                ((int64_t)pindex.snapshot.nTime + (int64_t)nStakeMinAge - nStakeModifierSelectionInterval > GetAdjustedTime()))
                return SeamFail(error, strprintf("GetKernelStakeModifier() : reached best block from block %s",
                                source.GetHash().ToString().c_str())) ? COLD_HOT_SEAM_AUTHORITY_FAILURE : COLD_HOT_SEAM_AUTHORITY_FAILURE;
            return COLD_HOT_SEAM_NOT_FOUND;
        }
        pindex = next;
        if (pindex.snapshot.nFlags & CBlockIndex::BLOCK_STAKE_MODIFIER)
        {
            *nStakeModifierHeight = pindex.snapshot.height;
            *nStakeModifierTime = (int64_t)pindex.snapshot.nTime;
        }
    }

    *nStakeModifier = pindex.snapshot.nStakeModifier;
    if (outFinalWalkHeight)
        *outFinalWalkHeight = pindex.snapshot.height;
    SeamClear(error);
    return COLD_HOT_SEAM_OK;
}

bool ColdHotSeamNavigator::GetKernelStakeModifier(const BlockIndexLogicalId& source,
                                                  uint64_t* nStakeModifier,
                                                  int* nStakeModifierHeight,
                                                  int64_t* nStakeModifierTime,
                                                  bool fPrintProofOfStake,
                                                  std::string* error) const
{
    return GetKernelStakeModifierR(source, nStakeModifier, nStakeModifierHeight, nStakeModifierTime, fPrintProofOfStake, error) == COLD_HOT_SEAM_OK;
}

// ---------------------------------------------------------------------------
// Three-argument GetKernelStakeModifier — bounded, no O(depth) memory or work
// for the active-chain mainnet case.
//
// Legacy semantic problem: given source S and candidate branch tip T, the
// modifier is selected along the forward (S->T) branch-local path, and S must
// be proved an ancestor of T.
//
// The naive implementation walks T->S collecting one snapshot per ancestor and
// reverses it (O(depth) RAM and O(depth) CPU). That history-scaled transient
// vector is the A.10 blocker this phase removes.
//
// Exact derivation, split by branch context:
//
//   1. Resolve S and T by stable logical identity.
//
//   2a. ACTIVE-CHAIN case (S and T both fInMainChain, S.height <= T.height): on
//       a single deterministic active chain, "S is an ancestor of T" is implied
//       by heights (both are on the same chain). Verify in O(1) — no walk, no
//       residency. Then walk FORWARD from S via GetNextActive until the running
//       modifier time reaches the target; this walk is bounded by the selection
//       interval in SECONDS (the number of blocks is ~interval/nTargetSpacing,
//       independent of S's depth). Return the first generated-modifier block
//       whose time >= target, exactly as legacy stops.
//
//   2b. SIDE-BRANCH case (candidate branch not on the active chain): the branch
//       is inherently bounded (validation candidate branches are short/hot); a
//       backward tip->S parent walk proves ancestry and finds the selection
//       point with O(branch-depth) CPU and O(1) state.
//
// This preserves exact branch-local semantics while NEVER materializing an
// O(depth) snapshot vector and NEVER requiring arbitrary historical CBlockIndex
// residency for an active-chain source.
// ---------------------------------------------------------------------------
ColdHotSeamResult ColdHotSeamNavigator::GetKernelStakeModifierR(const BlockIndexLogicalId& source,
                                                                const BlockIndexLogicalId& branchTip,
                                                                uint64_t* nStakeModifier,
                                                                int* nStakeModifierHeight,
                                                                int64_t* nStakeModifierTime,
                                                                bool fPrintProofOfStake,
                                                                std::string* error) const
{
    if (!nStakeModifier || !nStakeModifierHeight || !nStakeModifierTime)
        return COLD_HOT_SEAM_AUTHORITY_FAILURE;
    AssertLockHeld(cs_main);

    ColdHotSeamSnapshot from, tip;
    const ColdHotSeamResult rs = ResolveLogicalR(source, &from, error);
    if (rs != COLD_HOT_SEAM_OK)
        return rs;
    const ColdHotSeamResult rt = ResolveLogicalR(branchTip, &tip, error);
    if (rt != COLD_HOT_SEAM_OK)
        return rt;

    const int64_t nTargetTime = (int64_t)from.snapshot.nTime + GetStakeModifierSelectionInterval();

    // --- ACTIVE-CHAIN fast path -------------------------------------------
    // Both source and tip are on the single active chain with source not above
    // tip: S is an ancestor of T (same chain). No depth-scaled ancestry walk.
    if (from.snapshot.fInMainChain && tip.snapshot.fInMainChain &&
        from.snapshot.height <= tip.snapshot.height)
    {
        int runHeight = from.snapshot.height;
        int64_t runTime = (int64_t)from.snapshot.nTime;
        ColdHotSeamSnapshot cur = from;
        while (runTime < nTargetTime)
        {
            // Advance one active step, but never past the branch tip (legacy
            // path is S..T inclusive).
            if (cur.snapshot.height >= tip.snapshot.height)
                break;
            ColdHotSeamSnapshot next;
            const ColdHotSeamResult nr = GetNextActiveR(cur.ref, &next, error);
            if (nr == COLD_HOT_SEAM_AUTHORITY_FAILURE)
                return COLD_HOT_SEAM_AUTHORITY_FAILURE; // fail closed
            if (nr != COLD_HOT_SEAM_OK)
                break; // end of active chain before target
            cur = next;
            // Mirror legacy: update running modifier time/height only at a
            // generated-modifier block, then test against the target. This
            // examines the tip block exactly as legacy's forward loop does.
            if (cur.snapshot.nFlags & CBlockIndex::BLOCK_STAKE_MODIFIER)
            {
                runHeight = cur.snapshot.height;
                runTime = (int64_t)cur.snapshot.nTime;
            }
            if (runTime >= nTargetTime)
            {
                *nStakeModifier = cur.snapshot.nStakeModifier;
                *nStakeModifierHeight = runHeight;
                *nStakeModifierTime = runTime;
                SeamClear(error);
                return COLD_HOT_SEAM_OK;
            }
            if (cur.snapshot.height >= tip.snapshot.height)
                break;
        }
        // Legacy tip fallback: tip's own time already reached the target.
        if ((int64_t)tip.snapshot.nTime >= nTargetTime)
        {
            *nStakeModifier = tip.snapshot.nStakeModifier;
            *nStakeModifierHeight = tip.snapshot.height;
            *nStakeModifierTime = (int64_t)tip.snapshot.nTime;
            SeamClear(error);
            return COLD_HOT_SEAM_OK;
        }
        if (fPrintProofOfStake)
            return SeamFail(error, "GetKernelStakeModifier() : candidate branch ends before selection interval") ? COLD_HOT_SEAM_NOT_FOUND : COLD_HOT_SEAM_NOT_FOUND;
        return COLD_HOT_SEAM_NOT_FOUND;
    }

    // --- SIDE-BRANCH path --------------------------------------------------
    // Candidate branch context: prove S in the branch by a bounded backward walk
    // and find the selection point with O(1) state (O(branch-depth) CPU only).
    ColdHotSeamSnapshot cur = tip;
    ColdHotSeamSnapshot best;         // lowest-height generated modifier ancestor reaching target
    bool hasBest = false;
    bool foundSource = (cur.snapshot.hash == source.GetHash() &&
                        cur.snapshot.height == from.snapshot.height);
    while (!foundSource && cur.snapshot.hasParent)
    {
        // cur is not the source here (foundSource false) and is on the branch.
        if ((cur.snapshot.nFlags & CBlockIndex::BLOCK_STAKE_MODIFIER) &&
            (int64_t)cur.snapshot.nTime >= nTargetTime)
        {
            best = cur;
            hasBest = true;
        }
        ColdHotSeamSnapshot parent;
        const ColdHotSeamResult pr = GetParentR(cur.ref, &parent, error);
        if (pr != COLD_HOT_SEAM_OK)
            return pr; // authority/parent failure -> fail closed
        cur = parent;
        if (cur.snapshot.hash == source.GetHash() &&
            cur.snapshot.height == from.snapshot.height)
        {
            foundSource = true;
            break;
        }
    }
    if (!foundSource)
        return SeamFail(error, "GetKernelStakeModifier() : stake source is not an ancestor of candidate branch") ? COLD_HOT_SEAM_NOT_FOUND : COLD_HOT_SEAM_NOT_FOUND;

    if (hasBest)
    {
        *nStakeModifier = best.snapshot.nStakeModifier;
        *nStakeModifierHeight = best.snapshot.height;
        *nStakeModifierTime = (int64_t)best.snapshot.nTime;
        SeamClear(error);
        return COLD_HOT_SEAM_OK;
    }

    // Legacy tip fallback: if the candidate branch tip's own time has already
    // reached the target, its inherited modifier is authoritative.
    if ((int64_t)tip.snapshot.nTime >= nTargetTime)
    {
        *nStakeModifier = tip.snapshot.nStakeModifier;
        *nStakeModifierHeight = tip.snapshot.height;
        *nStakeModifierTime = (int64_t)tip.snapshot.nTime;
        SeamClear(error);
        return COLD_HOT_SEAM_OK;
    }
    if (fPrintProofOfStake)
        return SeamFail(error, "GetKernelStakeModifier() : candidate branch ends before selection interval") ? COLD_HOT_SEAM_NOT_FOUND : COLD_HOT_SEAM_NOT_FOUND;
    return COLD_HOT_SEAM_NOT_FOUND;
}

bool ColdHotSeamNavigator::GetKernelStakeModifier(const BlockIndexLogicalId& source,
                                                  const BlockIndexLogicalId& branchTip,
                                                  uint64_t* nStakeModifier,
                                                  int* nStakeModifierHeight,
                                                  int64_t* nStakeModifierTime,
                                                  bool fPrintProofOfStake,
                                                  std::string* error) const
{
    return GetKernelStakeModifierR(source, branchTip, nStakeModifier, nStakeModifierHeight, nStakeModifierTime, fPrintProofOfStake, error) == COLD_HOT_SEAM_OK;
}