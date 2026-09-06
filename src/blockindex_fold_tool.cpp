// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "blockindex_fold_tool.h"
#include "blockindex_hashindex.h"

#include <boost/filesystem.hpp>

#include <cstdio>

namespace fs = boost::filesystem;

// P6 fold implementation. See header for design.
//
// The fold builds a NEW generation in a staging dir (build-{F+1}.tmp), then
// lifecycle Publish/Validate/Select makes it CURRENT. The writer stores must be
// destroyed (releasing their FILE* + LevelDB LOCKs) BEFORE PublishGeneration
// re-opens the staging dir read-only — otherwise it collides on the hashindex
// LOCK (the A.10.1q double-open problem). So the store construction lives in a
// helper that returns before publish.

namespace {

struct FoldStagingResult
{
    bool ok;
    uint64_t baseCount;
    uint64_t newRecordCount;   // baseCount + foldRecordCount
    int32_t  newActiveTip;     // = baseTip + foldRecordCount
    BlockIndexId committedTipId;
    uint256 committedTipHash;
    std::string error;
    FoldStagingResult() : ok(false), baseCount(0), newRecordCount(0),
                          newActiveTip(-1), committedTipId(0), committedTipHash(0) {}
};

static bool CopyRecords(FixedBlockIndexStore& src,
                        BlockIndexHashIndex& hashIndex,
                        FixedBlockIndexStore& dst,
                        uint64_t count,
                        std::string* error)
{
    for (uint64_t id = 1; id <= count; ++id)
    {
        BlockIndexRecord rec;
        if (!src.Read(id, &rec, error))
            return false;
        BlockIndexId outId;
        if (!dst.Append(rec, &outId, error))
            return false;
        // hash -> the SAME RecordId (Append assigns id == physical), consistent
        // with base identity.
        if (!hashIndex.Put(rec.hash, id, error))
            return false;
    }
    return true;
}

// Build the staging generation directory and return the fold tip info.
// All writer stores are locals destroyed on return, releasing handles/LOCKs.
static FoldStagingResult BuildFoldStaging(const std::string& v2Root,
                                          uint64_t newGen,
                                          uint64_t baseGen,
                                          const std::string& baseDir,
                                          FixedBlockIndexStore& baseStore,
                                          BlockIndexActiveIndex& baseActive,
                                          BlockIndexDerivedStateStore& baseDerived,
                                          BlockIndexTipAuthority& tip,
                                          int32_t foldHeight,
                                          std::string* error)
{
    FoldStagingResult res;
    const uint64_t baseCount = baseStore.CommittedRecordCount();
    FixedBlockIndexManifest baseM = baseStore.GetManifest();
    const int32_t baseTip = baseM.committedTipHeight;
    res.baseCount = baseCount;

    fs::path staging = fs::path(v2Root) / (strprintf("build-%06llu.tmp", (unsigned long long)newGen));
    boost::system::error_code ec;
    if (fs::exists(staging))
        fs::remove_all(staging, ec);
    if (!fs::create_directories(staging, ec) && ec)
    {
        res.error = "fold: create staging dir failed";
        return res;
    }

    FixedBlockIndexStore newStore;
    if (!FixedBlockIndexStore::Create(staging.string(), newGen, &newStore, error))
    {
        res.error = "fold: create new records store failed: " + (error ? *error : "");
        return res;
    }
    BlockIndexHashIndex newHashIndex;
    if (!BlockIndexHashIndex::Create(staging.string(), newGen, &newHashIndex, error))
    {
        res.error = "fold: create new hashindex failed: " + (error ? *error : "");
        return res;
    }
    BlockIndexActiveIndex newActive;
    if (!BlockIndexActiveIndex::Create(staging.string(), newGen, &newActive, error))
    {
        res.error = "fold: create new active failed: " + (error ? *error : "");
        return res;
    }
    BlockIndexDerivedStateStore newDerived;
    unsigned char binding[32];
    memset(binding, 0, 32);
    if (!BlockIndexDerivedStateStore::Create(staging.string(), newGen, binding, &newDerived, error))
    {
        res.error = "fold: create new derived failed: " + (error ? *error : "");
        return res;
    }

    // Copy base records + hashindex (ids 1..baseCount preserved).
    if (!CopyRecords(baseStore, newHashIndex, newStore, baseCount, error))
    {
        res.error = "fold: copy base records failed: " + (error ? *error : "");
        return res;
    }

    // Copy base active members (heights 0..baseTip).
    std::vector<BlockIndexId> baseActiveIds;
    for (int32_t h = 0; h <= baseTip; ++h)
    {
        BlockIndexId aid;
        if (!baseActive.ReadEntry(h, &aid, error))
        {
            res.error = "fold: read base active failed";
            return res;
        }
        baseActiveIds.push_back(aid);
    }
    if (!newActive.AppendBatch(baseActiveIds, error))
    {
        res.error = "fold: write base active failed: " + (error ? *error : "");
        return res;
    }

    // Copy base derived entries (RecordId 1..baseCount).
    std::vector<BlockIndexDerivedEntry> baseDerivedEntries;
    for (BlockIndexId id = 1; id <= baseCount; ++id)
    {
        BlockIndexDerivedEntry de;
        BlockIndexDerivedLookupStatus ds = baseDerived.Read(id, &de, error);
        if (ds != BLOCK_INDEX_DERIVED_LOOKUP_FOUND)
        {
            res.error = "fold: read base derived failed";
            return res;
        }
        baseDerivedEntries.push_back(de);
    }
    if (!newDerived.AppendBatch(baseDerivedEntries, error))
    {
        res.error = "fold: write base derived failed: " + (error ? *error : "");
        return res;
    }

    // Append tip fold-prefix (active blocks at global height baseTip+1..foldHeight).
    uint64_t folded = 0;
    std::vector<BlockIndexDerivedEntry> foldDerived;
    std::vector<BlockIndexId> foldActive;
    for (int32_t h = baseTip + 1; h <= foldHeight; ++h)
    {
        BlockIndexTipRead tr = tip.LookupActiveByHeight(h, error);
        if (tr.status != BLOCK_INDEX_TIP_OK)
            break; // tip chain shorter than fold; stop folding
        BlockIndexId outId;
        if (!newStore.Append(tr.record, &outId, error))
        {
            res.error = "fold: append fold record failed";
            return res;
        }
        // RecordId for the fold record = baseCount + folded + 1 (dst.Append's outId)
        uint64_t newId = baseCount + folded + 1;
        if (!newHashIndex.Put(tr.record.hash, newId, error))
        {
            res.error = "fold: hashindex fold record failed";
            return res;
        }
        foldDerived.push_back(tr.derived);
        foldActive.push_back(newId);
        folded++;
    }
    if (!newDerived.AppendBatch(foldDerived, error))
    {
        res.error = "fold: append fold derived failed: " + (error ? *error : "");
        return res;
    }
    if (!newActive.AppendBatch(foldActive, error))
    {
        res.error = "fold: append fold active failed: " + (error ? *error : "");
        return res;
    }
    int32_t newActiveTip = baseTip + (int32_t)folded;
    if (!newActive.TruncateTo(newActiveTip, error))
    {
        res.error = "fold: truncate new active failed";
        return res;
    }
    // Resolve fold tip so we can set the derived content binding (must match
    // what ValidateGeneration recomputes: SHA256(tipHash || recordCount || gen)).
    BlockIndexRecord tipRec;
    BlockIndexId cid;
    if (folded > 0)
    {
        BlockIndexTipRead foldTip = tip.LookupActiveByHeight(foldHeight, error);
        if (foldTip.status != BLOCK_INDEX_TIP_OK)
        {
            res.error = "fold: tip authority has no active block at fold height " +
                        strprintf("%d", (int)foldHeight);
            return res;
        }
        tipRec = foldTip.record;
        cid = baseCount + folded;
    }
    else
    {
        BlockIndexId tipActiveId;
        if (!baseActive.ReadEntry(baseTip, &tipActiveId, error))
        {
            res.error = "fold: read base tip id failed";
            return res;
        }
        cid = tipActiveId;
        if (!baseStore.Read(tipActiveId, &tipRec, error))
        {
            res.error = "fold: read base tip record failed";
            return res;
        }
    }
    {
        unsigned char cb[32];
        if (!ComputeDerivedContentBinding(tipRec.hash, baseCount + folded, newGen, cb))
        {
            res.error = "fold: compute derived content binding failed";
            return res;
        }
        newDerived.SetContentBinding(cb);
    }
    if (!newDerived.Finalize(error))
    {
        res.error = "fold: finalize derived failed: " + (error ? *error : "");
        return res;
    }

    // (tipRec and cid already resolved in the content-binding block above.)

    FixedBlockIndexManifest man = newStore.GetManifest();
    man.state = BLOCK_INDEX_MANIFEST_COMPLETE;
    man.recordCount = baseCount + folded;
    man.committedTipHeight = newActiveTip;
    man.committedTipId = cid;
    man.committedTipHash = tipRec.hash;
    if (!newStore.WriteManifest(man, error))
    {
        res.error = "fold: write new MANIFEST failed: " + (error ? *error : "");
        return res;
    }

    // Destroy writer handles (release FILE* + LevelDB LOCK) BEFORE publish.
    newHashIndex.Close();
    res.ok = true;
    res.newRecordCount = baseCount + folded;
    res.newActiveTip = newActiveTip;
    res.committedTipId = cid;
    res.committedTipHash = tipRec.hash;
    res.error.clear();
    return res;
}

} // namespace

namespace {
inline BlockIndexFoldResult FoldFailure(BlockIndexFoldResult r, const std::string& msg,
                                        std::string* error)
{
    r.ok = false;
    r.error = msg;
    if (error)
        *error = msg;
    return r;
}
} // namespace

BlockIndexFoldResult BlockIndexFoldTool::Fold(const std::string& v2Root,
                                              uint64_t newGen,
                                              int32_t foldHeight,
                                              std::string* error)
{
    BlockIndexFoldResult r;
    r.newGeneration = newGen;
    r.foldHeight = foldHeight;

    const uint64_t baseGen = (newGen > 0) ? (newGen - 1) : 0;
    fs::path baseDir = fs::path(v2Root) / (strprintf("gen-%06llu", (unsigned long long)baseGen));
    fs::path tipRoot = fs::path(v2Root);

    // A1. Open base generation (read-only).
    FixedBlockIndexStore baseStore;
    FixedBlockIndexOpenOptions opts;
    opts.requireCompleteManifest = true;
    if (!FixedBlockIndexStore::OpenReadOnly(baseDir.string(), opts, &baseStore, error))
        return FoldFailure(r, "fold: open base generation failed", error);
    const FixedBlockIndexManifest& baseM = baseStore.GetManifest();
    const int32_t baseTip = baseM.committedTipHeight;
    r.baseTipHeight = baseTip;

    // Open base active + derived (for byte-identical copy).
    BlockIndexActiveIndex baseActive;
    if (!BlockIndexActiveIndex::Open(baseDir.string(), baseGen, &baseActive, error))
        return FoldFailure(r, "fold: open base active failed", error);
    BlockIndexDerivedStateStore baseDerived;
    if (!BlockIndexDerivedStateStore::OpenReadOnly(baseDir.string(), baseGen, &baseDerived, error))
        return FoldFailure(r, "fold: open base derived failed", error);

    // A2. Open the mutable tip.
    BlockIndexTipAuthority tip;
    if (!BlockIndexTipAuthority::Open(tipRoot.string(), baseGen, &tip, error))
        return FoldFailure(r, "fold: open tip failed", error);

    // B. Determine fold.
    if (foldHeight <= baseTip)
    {
        // nothing above base tip to fold
        r.ok = true;
        r.foldRecordCount = 0;
        r.foldHeight = baseTip;
        r.newGeneration = baseGen;
        if (error) error->clear();
        return r;
    }

    // C. Build the staging generation (writer stores destroyed inside).
    FoldStagingResult st = BuildFoldStaging(v2Root, newGen, baseGen, baseDir.string(),
                                            baseStore, baseActive, baseDerived, tip,
                                            foldHeight, error);
    if (!st.ok)
        return FoldFailure(r, st.error.empty() ? "fold: staging build failed" : st.error, error);
    r.baseRecordCount = st.baseCount;
    r.foldRecordCount = st.newRecordCount - st.baseCount;
    r.foldHeight = st.newActiveTip;

    // D. Publish + Validate + Select (atomic CURRENT flip).
    BlockIndexLifecycleStatus p = BlockIndexGenerationManager::PublishGeneration(v2Root, newGen, error);
    if (p != BLOCK_INDEX_LIFECYCLE_OK)
        return FoldFailure(r, "fold: publish generation failed: " + (error ? *error : ""), error);
    BlockIndexLifecycleStatus v = BlockIndexGenerationManager::ValidateGeneration(v2Root, newGen, error);
    if (v != BLOCK_INDEX_LIFECYCLE_OK)
        return FoldFailure(r, "fold: validate generation failed: " + (error ? *error : ""), error);
    BlockIndexLifecycleStatus s = BlockIndexGenerationManager::SelectGeneration(v2Root, newGen, error);
    if (s != BLOCK_INDEX_LIFECYCLE_OK)
        return FoldFailure(r, "fold: select generation failed: " + (error ? *error : ""), error);

    // E. Truncate the mutable tip to the fold height (re-anchor).
    BlockIndexTipStatus tt = tip.TruncateActiveTo(st.newActiveTip, error);
    if (tt != BLOCK_INDEX_TIP_OK)
        return FoldFailure(r, "fold: tip truncate after select failed (will reconcile on restart): "
                       + (error ? *error : ""), error);

    r.ok = true;
    r.foldHeight = st.newActiveTip;
    r.newGeneration = newGen;
    if (error) error->clear();
    return r;
}