#include "blockindex_v2_reader.h"

namespace {
static bool Fail(std::string* error, const std::string& text) { if (error) *error = text; return false; }
static void Clear(std::string* error) { if (error) error->clear(); }
}

BlockIndexV2Reader::BlockIndexV2Reader() : open(false), generation(0), cacheCapacity(0) {}
BlockIndexV2Reader::~BlockIndexV2Reader() { Close(); }

bool BlockIndexV2Reader::Open(const std::string& root, const BlockIndexV2ReaderOptions& options, std::string* error)
{
    Close();
    BlockIndexCurrentRecord current;
    BlockIndexLifecycleStatus status = BlockIndexGenerationManager::ReadCurrent(root, &current, error);
    if (status != BLOCK_INDEX_LIFECYCLE_OK) return false;
    const std::string dir = BlockIndexGenerationManager::GenerationPath(root, current.generation);
    FixedBlockIndexOpenOptions storeOptions;
    FixedBlockIndexStore nextStore;
    BlockIndexActiveIndex nextActive;
    BlockIndexHashIndex nextHash;
    if (!FixedBlockIndexStore::OpenReadOnly(dir, storeOptions, &nextStore, error)) return false;
    const FixedBlockIndexManifest nextManifest = nextStore.GetManifest();
    if (nextManifest.generation != current.generation ||
        !BlockIndexActiveIndex::Open(dir, current.generation, &nextActive, error) ||
        !BlockIndexHashIndex::Open(dir, current.generation, &nextHash, error)) return false;
    if (nextManifest.committedTipHeight >= 0) {
        BlockIndexId tipId = BLOCK_INDEX_ID_INVALID;
        BlockIndexRecord tip;
        if (!nextActive.ReadEntry(nextManifest.committedTipHeight, &tipId, error) || tipId != nextManifest.committedTipId ||
            !nextStore.Read(tipId, &tip, error) || tip.hash != nextManifest.committedTipHash ||
            tip.height != nextManifest.committedTipHeight) return Fail(error, "reader manifest/active tip coherence failure");
    }
    LOCK(cs);
    store = std::move(nextStore); active = std::move(nextActive); hashIndex = std::move(nextHash);
    rootPath = root; generationPath = dir; generation = current.generation; manifest = nextManifest;
    cacheCapacity = options.cacheCapacityBytes; cache.clear(); lru.clear(); stats = BlockIndexV2ReaderCacheStats(); stats.capacityBytes = cacheCapacity; open = true;
    Clear(error); return true;
}
void BlockIndexV2Reader::Close() { LOCK(cs); hashIndex.Close(); cache.clear(); lru.clear(); open=false; generation=0; rootPath.clear(); generationPath.clear(); manifest=FixedBlockIndexManifest(); }
bool BlockIndexV2Reader::IsOpen() const { LOCK(cs); return open; }
uint64_t BlockIndexV2Reader::Generation() const { LOCK(cs); return generation; }
uint64_t BlockIndexV2Reader::RecordCount() const { LOCK(cs); return open ? manifest.recordCount : 0; }
BlockIndexV2ReaderCacheStats BlockIndexV2Reader::CacheStats() const { LOCK(cs); return stats; }

BlockIndexSnapshot BlockIndexV2Reader::SnapshotFromRecord(BlockIndexId id, const BlockIndexRecord& r, bool inActive) const {
    BlockIndexSnapshot s; s.found=true; s.id=id; s.hash=r.hash; s.hashPrev=r.hashPrev; s.hashMerkleRoot=r.hashMerkleRoot; s.height=r.height; s.nFile=r.nFile; s.nBlockPos=r.nBlockPos; s.nFlags=r.nFlags; s.nVersion=r.nVersion; s.nTime=r.nTime; s.nBits=r.nBits; s.nNonce=r.nNonce; s.nMint=r.nMint; s.nMoneySupply=r.nMoneySupply; s.nStakeModifier=r.nStakeModifier; s.prevoutStake=r.prevoutStake; s.nStakeTime=r.nStakeTime; s.hashProof=r.hashProof; s.fProofOfStake=(r.prevoutStake.hash != uint256(0)); s.fInMainChain=inActive; s.hasParent=(r.hashPrev != uint256(0)); return s;
}
void BlockIndexV2Reader::CachePut(const BlockIndexSnapshot& s) const {
    if (cacheCapacity == 0) return;
    const uint64_t estimate = sizeof(CacheEntry) + sizeof(BlockIndexId) + sizeof(BlockIndexSnapshot) + 2*sizeof(void*);
    while (!lru.empty() && stats.bytesEstimated + estimate > cacheCapacity) { BlockIndexId old=lru.back(); cache.erase(old); lru.pop_back(); --stats.entries; ++stats.evictions; stats.bytesEstimated -= estimate; }
    if (estimate > cacheCapacity) return;
    lru.push_front(s.id); CacheEntry e; e.snapshot=s; e.lru=lru.begin(); cache[s.id]=e; ++stats.entries; stats.bytesEstimated += estimate;
}
BlockIndexV2ReadStatus BlockIndexV2Reader::GetRecordById(BlockIndexId id, BlockIndexSnapshot* out, std::string* error) const {
    if (!out) { Fail(error,"null reader output"); return BLOCK_INDEX_V2_READ_IO_ERROR; } LOCK(cs); if (!open) { Fail(error,"reader is not open"); return BLOCK_INDEX_V2_READ_NOT_OPEN; } if (id==0 || id>manifest.recordCount) { *out=BlockIndexSnapshot(); Clear(error); return BLOCK_INDEX_V2_READ_NOT_FOUND; }
    std::map<BlockIndexId,CacheEntry>::iterator it=cache.find(id); if(it!=cache.end()) { lru.splice(lru.begin(),lru,it->second.lru); *out=it->second.snapshot; ++stats.hits; Clear(error); return BLOCK_INDEX_V2_READ_FOUND; }
    ++stats.misses; BlockIndexRecord r; if(!store.Read(id,&r,error)) return BLOCK_INDEX_V2_READ_CORRUPT; BlockIndexId activeId=BLOCK_INDEX_ID_INVALID; bool inActive=false; if(r.height>=0 && r.height<=manifest.committedTipHeight) { if(!active.ReadEntry(r.height,&activeId,error)) return BLOCK_INDEX_V2_READ_CORRUPT; inActive=(activeId==id); } BlockIndexSnapshot s=SnapshotFromRecord(id,r,inActive); CachePut(s); *out=s; Clear(error); return BLOCK_INDEX_V2_READ_FOUND;
}
BlockIndexV2ReadStatus BlockIndexV2Reader::LookupByHash(const uint256& h, BlockIndexSnapshot* out, std::string* error) const { LOCK(cs); if(!open){Fail(error,"reader is not open");return BLOCK_INDEX_V2_READ_NOT_OPEN;} BlockIndexId id=0; BlockIndexHashLookupStatus hs=hashIndex.Lookup(h,&id,error); if(hs==BLOCK_INDEX_HASH_LOOKUP_NOT_FOUND){*out=BlockIndexSnapshot();return BLOCK_INDEX_V2_READ_NOT_FOUND;} if(hs!=BLOCK_INDEX_HASH_LOOKUP_FOUND)return BLOCK_INDEX_V2_READ_CORRUPT; BlockIndexV2ReadStatus rs=GetRecordById(id,out,error); if(rs==BLOCK_INDEX_V2_READ_FOUND && out->hash!=h){Fail(error,"hashindex/record hash mismatch");return BLOCK_INDEX_V2_READ_CORRUPT;} return rs; }
BlockIndexV2ReadStatus BlockIndexV2Reader::GetActiveByHeight(int h, BlockIndexSnapshot* out, std::string* error) const
{
    LOCK(cs);
    if (!open) { Fail(error, "reader is not open"); return BLOCK_INDEX_V2_READ_NOT_OPEN; }
    if (h < 0 || h > manifest.committedTipHeight) { *out=BlockIndexSnapshot(); return BLOCK_INDEX_V2_READ_NOT_FOUND; }
    BlockIndexId id=BLOCK_INDEX_ID_INVALID;
    if (!active.ReadEntry(h,&id,error) || id==0 || id>manifest.recordCount) { if(id==0 || id>manifest.recordCount) Fail(error,"active RecordId invalid/beyond committed count"); return BLOCK_INDEX_V2_READ_CORRUPT; }
    BlockIndexV2ReadStatus rs=GetRecordById(id,out,error);
    if (rs != BLOCK_INDEX_V2_READ_FOUND) return rs;
    if (out->height != h) { Fail(error,"active height/record mismatch"); return BLOCK_INDEX_V2_READ_CORRUPT; }
    // O(1) local chain-link integrity: validate both present adjacent active entries.
    if (h > 0) { BlockIndexId prevId=BLOCK_INDEX_ID_INVALID; BlockIndexSnapshot prev; if (!active.ReadEntry(h-1,&prevId,error) || prevId==0 || prevId>manifest.recordCount || GetRecordById(prevId,&prev,error)!=BLOCK_INDEX_V2_READ_FOUND || prev.height!=h-1 || out->hashPrev!=prev.hash) { Fail(error,"active predecessor linkage mismatch"); return BLOCK_INDEX_V2_READ_CORRUPT; } }
    if (h < manifest.committedTipHeight) { BlockIndexId nextId=BLOCK_INDEX_ID_INVALID; BlockIndexSnapshot next; if (!active.ReadEntry(h+1,&nextId,error) || nextId==0 || nextId>manifest.recordCount || GetRecordById(nextId,&next,error)!=BLOCK_INDEX_V2_READ_FOUND || next.height!=h+1 || next.hashPrev!=out->hash) { Fail(error,"active successor linkage mismatch"); return BLOCK_INDEX_V2_READ_CORRUPT; } }
    out->fInMainChain=true; Clear(error); return BLOCK_INDEX_V2_READ_FOUND;
}

BlockIndexV2ReadStatus BlockIndexV2Reader::GetNextActive(BlockIndexId id, BlockIndexSnapshot* out, std::string* error) const
{
    BlockIndexSnapshot s;
    BlockIndexV2ReadStatus r = GetRecordById(id, &s, error);
    if (r != BLOCK_INDEX_V2_READ_FOUND)
        return r;
    if (!s.fInMainChain || s.height >= manifest.committedTipHeight)
    {
        *out = BlockIndexSnapshot();
        return BLOCK_INDEX_V2_READ_NOT_FOUND;
    }
    return GetActiveByHeight(s.height + 1, out, error);
}
BlockIndexV2ReadStatus BlockIndexV2Reader::GetParent(BlockIndexId id, BlockIndexSnapshot* out, std::string* error) const { BlockIndexSnapshot child; BlockIndexV2ReadStatus r=GetRecordById(id,&child,error); if(r!=BLOCK_INDEX_V2_READ_FOUND||!child.hasParent){if(r==BLOCK_INDEX_V2_READ_FOUND)*out=BlockIndexSnapshot();return r==BLOCK_INDEX_V2_READ_FOUND?BLOCK_INDEX_V2_READ_NOT_FOUND:r;} r=LookupByHash(child.hashPrev,out,error); if(r==BLOCK_INDEX_V2_READ_FOUND && out->hash!=child.hashPrev){Fail(error,"parent hash mismatch");return BLOCK_INDEX_V2_READ_CORRUPT;} return r; }
BlockIndexV2ReadStatus BlockIndexV2Reader::GetAncestor(BlockIndexId id,int target,BlockIndexSnapshot*out,std::string*error)const { BlockIndexSnapshot s; BlockIndexV2ReadStatus r=GetRecordById(id,&s,error); if(r!=BLOCK_INDEX_V2_READ_FOUND||target>s.height){if(r==BLOCK_INDEX_V2_READ_FOUND)*out=BlockIndexSnapshot();return BLOCK_INDEX_V2_READ_NOT_FOUND;} BlockIndexId activeId=BLOCK_INDEX_ID_INVALID; if(active.ReadEntry(s.height,&activeId,error) && activeId==id) return GetActiveByHeight(target,out,error); while(s.height>target){r=GetParent(s.id,&s,error);if(r!=BLOCK_INDEX_V2_READ_FOUND)return r;}*out=s;return r; }
BlockIndexV2ReadStatus BlockIndexV2Reader::GetStakingMetadata(BlockIndexId id, BlockIndexStakingMetadata* out, std::string* error) const
{
    if (!out)
    {
        Fail(error, "null staking metadata output");
        return BLOCK_INDEX_V2_READ_IO_ERROR;
    }
    *out = BlockIndexStakingMetadata();

    BlockIndexSnapshot current;
    BlockIndexV2ReadStatus status = GetRecordById(id, &current, error);
    if (status != BLOCK_INDEX_V2_READ_FOUND)
        return status;

    // This is the exact GetLastStakeModifier parent walk, expressed only in
    // persisted V1 fields. A zero time is valid; availability is separate.
    BlockIndexSnapshot modifierSource = current;
    while (modifierSource.hasParent &&
           !(modifierSource.nFlags & CBlockIndex::BLOCK_STAKE_MODIFIER))
    {
        status = GetParent(modifierSource.id, &modifierSource, error);
        if (status != BLOCK_INDEX_V2_READ_FOUND)
            return status;
    }
    if (!(modifierSource.nFlags & CBlockIndex::BLOCK_STAKE_MODIFIER))
    {
        Fail(error, "stake modifier generation unavailable at root");
        return BLOCK_INDEX_V2_READ_NOT_FOUND;
    }
    out->nStakeModifierTime = modifierSource.nTime;
    out->hasStakeModifierTime = true;

    // Reproduce GetStakeModifierChecksum from genesis/branch root forward.
    // Every input is persisted in V1. The no-parent/non-genesis rule is kept
    // byte-for-byte equivalent to kernel.cpp's legacy implementation.
    std::vector<BlockIndexSnapshot> path;
    current = BlockIndexSnapshot();
    status = GetRecordById(id, &current, error);
    if (status != BLOCK_INDEX_V2_READ_FOUND)
        return status;
    while (true)
    {
        path.push_back(current);
        if (!current.hasParent)
            break;
        status = GetParent(current.id, &current, error);
        if (status != BLOCK_INDEX_V2_READ_FOUND)
            return status;
    }

    unsigned int checksum = 0;
    for (std::vector<BlockIndexSnapshot>::reverse_iterator it = path.rbegin();
         it != path.rend(); ++it)
    {
        if (!it->hasParent && it->hash != GetGenesisBlockHash())
        {
            checksum = 0;
            continue;
        }
        CDataStream ss(SER_GETHASH, 0);
        if (it->hasParent)
            ss << checksum;
        const uint256 proof = (it->nFlags & CBlockIndex::BLOCK_PROOF_OF_STAKE) ? it->hashProof : uint256(0);
        ss << it->nFlags << proof << it->nStakeModifier;
        uint256 hashChecksum = Hash(ss.begin(), ss.end());
        hashChecksum >>= (256 - 32);
        checksum = hashChecksum.Get64();
    }
    out->nStakeModifierChecksum = checksum;
    out->hasStakeModifierChecksum = true;
    Clear(error);
    return BLOCK_INDEX_V2_READ_FOUND;
}

BlockIndexV2ReadStatus BlockIndexV2Reader::GetStakeModifierTime(BlockIndexId id, int64_t* out, std::string* error) const
{
    if (!out)
    {
        Fail(error, "null modifier time output");
        return BLOCK_INDEX_V2_READ_IO_ERROR;
    }
    BlockIndexSnapshot current;
    BlockIndexV2ReadStatus status = GetRecordById(id, &current, error);
    if (status != BLOCK_INDEX_V2_READ_FOUND)
        return status;
    BlockIndexSnapshot modifierSource = current;
    while (modifierSource.hasParent && !(modifierSource.nFlags & CBlockIndex::BLOCK_STAKE_MODIFIER))
    {
        status = GetParent(modifierSource.id, &modifierSource, error);
        if (status != BLOCK_INDEX_V2_READ_FOUND)
            return status;
    }
    if (!(modifierSource.nFlags & CBlockIndex::BLOCK_STAKE_MODIFIER))
    {
        Fail(error, "stake modifier generation unavailable at root");
        return BLOCK_INDEX_V2_READ_NOT_FOUND;
    }
    *out = (int64_t)modifierSource.nTime;
    Clear(error);
    return BLOCK_INDEX_V2_READ_FOUND;
}

BlockIndexV2ReadStatus BlockIndexV2Reader::GetStakeModifierChecksum(BlockIndexId id, unsigned int* out, std::string* error) const
{
    if (!out)
    {
        Fail(error, "null modifier checksum output");
        return BLOCK_INDEX_V2_READ_IO_ERROR;
    }
    std::vector<BlockIndexSnapshot> path;
    BlockIndexSnapshot current;
    BlockIndexV2ReadStatus status = GetRecordById(id, &current, error);
    if (status != BLOCK_INDEX_V2_READ_FOUND)
        return status;
    while (true)
    {
        path.push_back(current);
        if (!current.hasParent)
            break;
        status = GetParent(current.id, &current, error);
        if (status != BLOCK_INDEX_V2_READ_FOUND)
            return status;
    }
    unsigned int checksum = 0;
    for (std::vector<BlockIndexSnapshot>::reverse_iterator it = path.rbegin();
         it != path.rend(); ++it)
    {
        if (!it->hasParent && it->hash != GetGenesisBlockHash())
        {
            checksum = 0;
            continue;
        }
        CDataStream ss(SER_GETHASH, 0);
        if (it->hasParent)
            ss << checksum;
        const uint256 proof = (it->nFlags & CBlockIndex::BLOCK_PROOF_OF_STAKE) ? it->hashProof : uint256(0);
        ss << it->nFlags << proof << it->nStakeModifier;
        uint256 hashChecksum = Hash(ss.begin(), ss.end());
        hashChecksum >>= (256 - 32);
        checksum = hashChecksum.Get64();
    }
    *out = checksum;
    Clear(error);
    return BLOCK_INDEX_V2_READ_FOUND;
}

BlockIndexV2ReadStatus BlockIndexV2Reader::FindFork(BlockIndexId a,BlockIndexId b,BlockIndexSnapshot*out,std::string*error)const { BlockIndexSnapshot x,y; BlockIndexV2ReadStatus r=GetRecordById(a,&x,error);if(r!=1)return r;r=GetRecordById(b,&y,error);if(r!=1)return r;while(x.height>y.height){r=GetParent(x.id,&x,error);if(r!=1)return r;}while(y.height>x.height){r=GetParent(y.id,&y,error);if(r!=1)return r;}while(x.hash!=y.hash){r=GetParent(x.id,&x,error);if(r!=1)return r;r=GetParent(y.id,&y,error);if(r!=1)return r;}*out=x;return BLOCK_INDEX_V2_READ_FOUND; }
BlockIndexSnapshot BlockIndexV2Reader::GetTip() const { BlockIndexSnapshot s; std::string e; GetRecordById(manifest.committedTipId,&s,&e); if(s.found)s.fInMainChain=true; return s; }
bool BlockIndexV2Reader::CurrentSelectionChanged(std::string* error) const { LOCK(cs); if(!open)return Fail(error,"reader is not open"); BlockIndexCurrentRecord c; BlockIndexLifecycleStatus st=BlockIndexGenerationManager::ReadCurrent(rootPath,&c,error); return st!=BLOCK_INDEX_LIFECYCLE_OK||c.generation!=generation; }
