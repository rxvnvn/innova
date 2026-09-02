#include "blockindex_generation_builder.h"

#include "txdb-leveldb.h"
#include "main.h"
#include "kernel.h"
#include "dag.h"

#include <leveldb/db.h>
#include <leveldb/filter_policy.h>

#include <map>
#include <set>
#include <stdio.h>
#include <string>
#include <vector>

namespace {

static bool SetError(std::string* error, const std::string& message)
{
    if (error)
        *error = message;
    return false;
}

static void ClearError(std::string* error)
{
    if (error)
        error->clear();
}

} // namespace

bool ReadLegacyBlockIndexSource(const std::string& snapshotLevelDbDir,
                                BlockIndexGenerationSource* out,
                                std::string* error)
{
    if (!out)
        return SetError(error, "null generation source output");

    // Open the STATIC SNAPSHOT copy. We never open the live datadir DB. Bundled
    // LevelDB has no filesystem-level read-only open, so this operates on the
    // dedicated snapshot copy (mutations, if any, are confined to the copy).
    leveldb::Options options;
    options.create_if_missing = false;
    options.error_if_exists = false;
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    leveldb::DB* db = NULL;
    leveldb::Status status = leveldb::DB::Open(options, snapshotLevelDbDir, &db);
    if (!status.ok())
        return SetError(error, std::string("snapshot LevelDB open failure: ") + status.ToString());

    BlockIndexGenerationSource src;

    // scan blockindex keys (same pattern as CTxDB::LoadBlockIndex)
    leveldb::Iterator* iterator = db->NewIterator(leveldb::ReadOptions());
    CDataStream ssStartKey(SER_DISK, CLIENT_VERSION);
    ssStartKey << make_pair(std::string("blockindex"), uint256(0));
    iterator->Seek(ssStartKey.str());

    while (iterator->Valid())
    {
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey.write(iterator->key().data(), iterator->key().size());
        CDataStream ssValue(SER_DISK, CLIENT_VERSION);
        ssValue.write(iterator->value().data(), iterator->value().size());

        std::string strType;
        ssKey >> strType;
        if (strType != "blockindex")
            break;

        CDiskBlockIndex diskindex;
        ssValue >> diskindex;

        uint256 blockHash = diskindex.GetBlockHash();

        BlockIndexGenerationSourceRecord rec;
        rec.hash = blockHash;
        rec.record.hash = blockHash;
        rec.record.hashPrev = diskindex.hashPrev;
        rec.record.hashMerkleRoot = diskindex.hashMerkleRoot;
        rec.record.hashProof = diskindex.hashProof;
        rec.record.prevoutStake = diskindex.prevoutStake;
        rec.record.height = diskindex.nHeight;
        rec.record.nFile = diskindex.nFile;
        rec.record.nBlockPos = diskindex.nBlockPos;
        rec.record.nFlags = diskindex.nFlags;
        rec.record.nVersion = diskindex.nVersion;
        rec.record.nTime = diskindex.nTime;
        rec.record.nBits = diskindex.nBits;
        rec.record.nNonce = diskindex.nNonce;
        rec.record.nMint = diskindex.nMint;
        rec.record.nMoneySupply = diskindex.nMoneySupply;
        rec.record.nStakeModifier = diskindex.nStakeModifier;
        rec.record.nStakeTime = diskindex.nStakeTime;

        src.records.push_back(rec);
        iterator->Next();
    }
    delete iterator;

    // read hashBestChain (key is CDataStream-serialized std::string, so encode
    // it the same way CTxDB::Read does; a raw "hashBestChain" would miss the
    // string-length prefix)
    CDataStream ssBestKey(SER_DISK, CLIENT_VERSION);
    ssBestKey << std::string("hashBestChain");
    std::string bestVal;
    status = db->Get(leveldb::ReadOptions(), ssBestKey.str(), &bestVal);
    if (status.ok())
    {
        CDataStream ss(bestVal.data(), bestVal.data() + bestVal.size(), SER_DISK, CLIENT_VERSION);
        ss >> src.hashBestChain;
        src.foundBestChain = true;
    }
    delete db;

    *out = src;
    ClearError(error);
    return true;
}

bool ReadDAGLinksFromSnapshot(const std::string& snapshotLevelDbDir,
                              std::map<uint256, std::vector<uint256> >* dagLinks,
                              std::map<uint256, uint256>* dagScores,
                              std::string* error)
{
    if (!dagLinks || !dagScores)
        return SetError(error, "null DAG links/scores output");

    leveldb::Options options;
    options.create_if_missing = false;
    options.error_if_exists = false;
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    leveldb::DB* db = NULL;
    leveldb::Status status = leveldb::DB::Open(options, snapshotLevelDbDir, &db);
    if (!status.ok())
        return SetError(error, std::string("snapshot LevelDB open failure for DAG: ") + status.ToString());

    // Iterate "daglinks" prefix (same pattern as CTxDB::IterateDAGLinks)
    leveldb::Iterator* iterator = db->NewIterator(leveldb::ReadOptions());
    CDataStream ssPrefix(SER_DISK, CLIENT_VERSION);
    ssPrefix << std::string("daglinks");
    std::string strPrefix = ssPrefix.str();
    iterator->Seek(strPrefix);

    while (iterator->Valid())
    {
        leveldb::Slice keySlice = iterator->key();
        if (keySlice.ToString().compare(0, strPrefix.size(), strPrefix) != 0)
            break;

        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey.write(keySlice.data(), keySlice.size());
        std::string strType;
        ssKey >> strType;
        if (strType != "daglinks")
            break;

        uint256 blockHash;
        ssKey >> blockHash;

        CBlockDAGData dagData;
        CDataStream ssValue(SER_DISK, CLIENT_VERSION);
        ssValue.write(iterator->value().data(), iterator->value().size());
        ssValue >> dagData;

        // Store parent hashes for DAG trust computation
        (*dagLinks)[blockHash] = dagData.vDAGParents;
        // Store DAG score for canonical post-DAG trust
        if (dagData.nDAGScore != 0)
            (*dagScores)[blockHash] = dagData.nDAGScore;

        iterator->Next();
    }
    delete iterator;
    delete db;

    ClearError(error);
    return true;
}

BlockIndexGenerationBuilder::BlockIndexGenerationBuilder()
    : built(false)
{
}

void BlockIndexGenerationBuilder::Close()
{
    if (derivedStore.IsOpen())
        derivedStore = BlockIndexDerivedStateStore();
    if (activeIndex.IsOpen())
        activeIndex = BlockIndexActiveIndex();
    if (hashIndex.IsOpen())
        hashIndex.Close();
    built = false;
}

bool BlockIndexGenerationBuilder::Build(const BlockIndexGenerationSource& source,
                                        const std::string& generationDir,
                                        uint64_t generation,
                                        BlockIndexGenerationStats* stats,
                                        std::string* error)
{
    if (generation == 0)
        return SetError(error, "invalid generation 0");
    if (!built)
    {
        // fresh build: create store, hashindex, activeindex
        if (!FixedBlockIndexStore::Create(generationDir, generation, &store, error))
            return false;
        if (!BlockIndexHashIndex::Create(generationDir, generation, &hashIndex, error))
            return false;
        if (!BlockIndexActiveIndex::Create(generationDir, generation, &activeIndex, error))
            return false;
    }

    // ---- deterministic RecordId assignment: order by block hash ----
    // Build a sorted vector so RecordId assignment is reproducible and
    // independent of the source iteration order / map pointer identity.
    std::vector<BlockIndexGenerationSourceRecord> ordered = source.records;
    std::sort(ordered.begin(), ordered.end(),
              [](const BlockIndexGenerationSourceRecord& a,
                 const BlockIndexGenerationSourceRecord& b) {
                  return a.hash < b.hash;
              });

    // map hash -> assigned id
    std::map<uint256, BlockIndexId> idMap;
    idMap.clear();
    // duplicate detection: a source containing two records for the same block
    // hash is ambiguous (two blocks cannot share a hash) and must fail closed,
    // otherwise the hashindex would silently map the second id over the first.
    {
        std::map<uint256, int> seen;
        for (size_t i = 0; i < ordered.size(); ++i)
        {
            if (++seen[ordered[i].hash] > 1)
                return SetError(error, "duplicate source block hash");
        }
    }

    // index: hash -> (record, id) for O(log n) active-chain reconstruction.
    std::map<uint256, std::pair<const BlockIndexRecord*, BlockIndexId> > byHash;
    for (size_t i = 0; i < ordered.size(); ++i)
        byHash[ordered[i].hash] = std::make_pair(&ordered[i].record, BLOCK_INDEX_ID_INVALID);

    // Append all records in deterministic id order in a SINGLE batch (one
    // open + one fsync). Assign RecordId sequentially; keep, for each record,
    // the exact id that was appended (so the hashindex step cannot be masked by
    // duplicate-hash collapse in idMap).
    std::vector<BlockIndexRecord> batchRecords;
    batchRecords.reserve(ordered.size());
    for (size_t i = 0; i < ordered.size(); ++i)
        batchRecords.push_back(ordered[i].record);

    std::vector<BlockIndexId> batchIds;
    if (!store.AppendBatch(batchRecords, &batchIds, error))
        return false;

    std::vector<std::pair<uint256, BlockIndexId> > appended;
    appended.reserve(ordered.size());
    for (size_t i = 0; i < ordered.size(); ++i)
    {
        idMap[ordered[i].hash] = batchIds[i];
        byHash[ordered[i].hash].second = batchIds[i];
        appended.push_back(std::make_pair(ordered[i].hash, batchIds[i]));
    }
    const uint64_t totalRecords = ordered.size();

    // ---- hashindex: every record hash -> RecordId ----
    for (size_t i = 0; i < appended.size(); ++i)
    {
        if (!hashIndex.Put(appended[i].first, appended[i].second, error))
            return false;
    }

    // ---- active.dat: active chain by height ----
    // Reconstruct the active chain from hashBestChain by following hashPrev
    // down to genesis, then reverse to ascending height.
    if (!source.foundBestChain)
        return SetError(error, "generation source lacks hashBestChain");

    std::vector<BlockIndexId> activeChainByHeight; // dense, index = height
    {
        std::vector<std::pair<int32_t, BlockIndexId> > byHeight;
        uint256 cur = source.hashBestChain;
        while (true)
        {
            std::map<uint256, std::pair<const BlockIndexRecord*, BlockIndexId> >::iterator it =
                byHash.find(cur);
            if (it == byHash.end())
                return SetError(error, "hashBestChain record missing from source");
            const BlockIndexRecord* rec = it->second.first;
            const BlockIndexId id = it->second.second;
            byHeight.push_back(std::make_pair(rec->height, id));
            if (rec->hashPrev == 0)
                break; // genesis
            cur = rec->hashPrev;
        }

        // The active chain must be dense and contiguous in height 0..tip.
        // (byHeight was collected tip-first, so we only rely on the sorted
        // continuity loop below to prove density 0..n-1.)
        if (byHeight.empty())
            return SetError(error, "empty active chain in source");

        std::sort(byHeight.begin(), byHeight.end());
        for (size_t h = 0; h < byHeight.size(); ++h)
        {
            if (byHeight[h].first != (int32_t)h)
                return SetError(error, "active chain height discontinuity");
            activeChainByHeight.push_back(byHeight[h].second);
        }
    }

    // dense active.dat: height -> RecordId (single batch, one fsync)
    {
        std::vector<BlockIndexId> activeIds(activeChainByHeight.begin(), activeChainByHeight.end());
        if (!activeIndex.AppendBatch(activeIds, error))
            return false;
    }

    // ---- MANIFEST lifecycle: validate then COMPLETE ----
    if (activeChainByHeight.empty())
        return SetError(error, "empty active chain");

    const int32_t tipHeight = (int32_t)activeChainByHeight.size() - 1;
    const BlockIndexId tipId = activeChainByHeight[tipHeight];

    // find tip hash
    uint256 tipHash = 0;
    {
        std::map<uint256, BlockIndexId>::iterator it = idMap.begin();
        for (; it != idMap.end(); ++it)
        {
            if (it->second == tipId)
            {
                tipHash = it->first;
                break;
            }
        }
        if (tipHash == 0)
            return SetError(error, "tip RecordId not found in id map");
    }

    // ---- derived.dat: compute derived state by height order, emit by RecordId ----
    // Build height-sorted index for trust/checksum computation.
    // The builder computes exact derived values using the same semantics as
    // the live runtime: linear trust replay + DAG score for post-DAG PoW,
    // logical-parent checksum, branch-local modifier-time memo.
    std::map<uint256, const BlockIndexRecord*> recordByHash;
    for (size_t i = 0; i < ordered.size(); ++i)
        recordByHash[ordered[i].hash] = &ordered[i].record;

    // Height-sorted vector for sequential computation
    std::vector<std::pair<int32_t, uint256> > heightSorted;
    for (size_t i = 0; i < ordered.size(); ++i)
        heightSorted.push_back(std::make_pair(ordered[i].record.height, ordered[i].hash));
    std::sort(heightSorted.begin(), heightSorted.end());

    // Compute derived values by height order
    struct DerivedComputed {
        uint256 chainTrust;
        uint32_t stakeModifierChecksum;
        int64_t stakeModifierTime;
        bool hasModifierTime;
        uint32_t nSize;
        bool hasBlockSize;
        DerivedComputed() : chainTrust(0), stakeModifierChecksum(0),
            stakeModifierTime(0), hasModifierTime(false), nSize(0), hasBlockSize(false) {}
    };
    std::map<uint256, DerivedComputed> derivedByHash;

    // Set of active chain hashes for active membership check
    std::set<uint256> activeHashSet;
    for (size_t h = 0; h < activeChainByHeight.size(); ++h)
    {
        // Find hash for this RecordId
        for (std::map<uint256, BlockIndexId>::iterator it = idMap.begin(); it != idMap.end(); ++it)
        {
            if (it->second == activeChainByHeight[h])
            {
                activeHashSet.insert(it->first);
                break;
            }
        }
    }

    for (size_t i = 0; i < heightSorted.size(); ++i)
    {
        const uint256& hash = heightSorted[i].second;
        const BlockIndexRecord* rec = recordByHash[hash];
        DerivedComputed dc;

        // nChainTrust: linear replay (parent.trust + GetBlockTrust)
        // For post-DAG PoW blocks, the DAG score overwrites this.
        uint256 parentTrust = 0;
        if (rec->hashPrev != uint256(0))
        {
            std::map<uint256, DerivedComputed>::iterator pit = derivedByHash.find(rec->hashPrev);
            if (pit != derivedByHash.end())
                parentTrust = pit->second.chainTrust;
        }
        // Compute GetBlockTrust equivalent from record fields
        CBigNum bnTarget;
        bnTarget.SetCompact(rec->nBits);
        uint256 blockTrust = 0;
        if (bnTarget > 0)
        {
            // Pre-DAG or PoW: use target-based trust
            if (rec->height < GetForkHeightDAG() || !(rec->prevoutStake.hash != uint256(0)))
                blockTrust = ((CBigNum(1)<<256) / (bnTarget+1)).getuint256();
            // Post-DAG PoS: trust = 0
        }
        dc.chainTrust = parentTrust + blockTrust;

        // A.10.1b-fix2 C3: Apply canonical post-DAG trust.
        // For post-DAG PoW blocks with persisted DAG score, overwrite linear
        // trust with nDAGScore. This matches RestoreDAGTrustIntoChainTrust
        // semantics exactly.
        if (rec->height >= GetForkHeightDAG() && !(rec->prevoutStake.hash != uint256(0)))
        {
            std::map<uint256, uint256>::const_iterator dit = source.dagScores.find(hash);
            if (dit != source.dagScores.end() && dit->second != 0)
                dc.chainTrust = dit->second;
        }

        // nStakeModifierChecksum: by logical parent topology
        // Reproduce GetStakeModifierChecksum semantics
        {
            unsigned int parentChecksum = 0;
            if (rec->hashPrev != uint256(0))
            {
                std::map<uint256, DerivedComputed>::iterator pit = derivedByHash.find(rec->hashPrev);
                if (pit != derivedByHash.end())
                    parentChecksum = pit->second.stakeModifierChecksum;
            }
            // Compute checksum: Hash(parentChecksum || nFlags || hashProof || nStakeModifier)
            CDataStream ss(SER_GETHASH, 0);
            if (rec->hashPrev != uint256(0))
                ss << parentChecksum;
            uint256 proof = (rec->nFlags & CBlockIndex::BLOCK_PROOF_OF_STAKE) ? rec->hashProof : uint256(0);
            ss << rec->nFlags << proof << rec->nStakeModifier;
            uint256 hashChecksum = Hash(ss.begin(), ss.end());
            hashChecksum >>= (256 - 32);
            dc.stakeModifierChecksum = hashChecksum.Get64();
        }

        // nStakeModifierTime: branch-local memo
        // fGeneratedStakeModifier ? GetBlockTime() : parent's memo
        bool fGeneratedStakeModifier = (rec->nFlags & CBlockIndex::BLOCK_STAKE_MODIFIER) != 0;
        if (fGeneratedStakeModifier)
        {
            dc.stakeModifierTime = (int64_t)rec->nTime;
            dc.hasModifierTime = true;
        }
        else if (rec->hashPrev != uint256(0))
        {
            std::map<uint256, DerivedComputed>::iterator pit = derivedByHash.find(rec->hashPrev);
            if (pit != derivedByHash.end() && pit->second.hasModifierTime)
            {
                dc.stakeModifierTime = pit->second.stakeModifierTime;
                dc.hasModifierTime = true;
            }
            // else: unavailable (no parent or parent has no memo)
        }

        // A.10.1b-fix2 C1: Compute exact nSize from block files.
        // Read the block from blk*.dat using nFile/nBlockPos, deserialize,
        // and compute GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION).
        // If block files are not accessible, mark unavailable.
        if (!source.blockDataDir.empty() && rec->nFile > 0)
        {
            std::string blockFn = strprintf("blk%04u.dat", rec->nFile);
            boost::filesystem::path blockPath = boost::filesystem::path(source.blockDataDir) / blockFn;
            FILE* blockFile = fopen(blockPath.string().c_str(), "rb");
            if (blockFile)
            {
                if (fseeko(blockFile, (off_t)rec->nBlockPos, SEEK_SET) == 0)
                {
                    CBlock block;
                    CAutoFile filein(blockFile, SER_DISK, CLIENT_VERSION);
                    try {
                        filein >> block;
                        dc.nSize = ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION);
                        dc.hasBlockSize = (dc.nSize > 0);
                    }
                    catch (std::exception& e)
                    {
                        // Block deserialization failed: mark unavailable
                        dc.nSize = 0;
                        dc.hasBlockSize = false;
                    }
                }
                else
                {
                    fclose(blockFile);
                    dc.nSize = 0;
                    dc.hasBlockSize = false;
                }
            }
            else
            {
                // Block file not found: mark unavailable
                dc.nSize = 0;
                dc.hasBlockSize = false;
            }
        }
        else
        {
            // No block data directory or invalid nFile: mark unavailable
            dc.nSize = 0;
            dc.hasBlockSize = false;
        }

        derivedByHash[hash] = dc;
    }

    // Create derived.dat and emit entries in RecordId order
    // A.10.1b-fix2 C2: Use placeholder binding initially; will be replaced
    // with generation root after all component digests are computed.
    {
        unsigned char placeholderBinding[32];
        memset(placeholderBinding, 0, 32);

        if (!BlockIndexDerivedStateStore::Create(generationDir, generation, placeholderBinding, &derivedStore, error))
            return false;

        // Emit in RecordId order (same as `appended` order)
        // Also compute derived entries digest as we go
        SHA256_CTX derivedEntriesCtx;
        SHA256_Init(&derivedEntriesCtx);

        for (size_t i = 0; i < appended.size(); ++i)
        {
            const uint256& hash = appended[i].first;
            std::map<uint256, DerivedComputed>::iterator dit = derivedByHash.find(hash);
            if (dit == derivedByHash.end())
                return SetError(error, "derived computation missing for hash");

            BlockIndexDerivedEntry entry;
            entry.chainTrust = dit->second.chainTrust;
            entry.stakeModifierChecksum = dit->second.stakeModifierChecksum;
            entry.stakeModifierTime = dit->second.stakeModifierTime;
            entry.SetHasStakeModifierTime(dit->second.hasModifierTime);
            entry.nSize = dit->second.nSize;
            entry.SetHasBlockSize(dit->second.hasBlockSize);

            // Encode entry for digest computation
            std::vector<unsigned char> encoded;
            if (!EncodeBlockIndexDerivedEntry(entry, &encoded, error))
                return false;
            SHA256_Update(&derivedEntriesCtx, &encoded[0], encoded.size());

            if (!derivedStore.Append(entry, error))
                return false;
        }

        // Compute derived entries digest
        unsigned char derivedEntriesDigest[32];
        SHA256_Final(derivedEntriesDigest, &derivedEntriesCtx);

        // A.10.1b-fix2 C2: Compute component content digests.
        // Records digest: SHA256 of all record entry bytes
        unsigned char recordsDigest[32];
        {
            SHA256_CTX ctx;
            SHA256_Init(&ctx);
            // Read records.dat entries (skip header)
            std::string recordsPath = generationDir + "/" + BLOCK_INDEX_RECORDS_FILE_NAME;
            FILE* rf = fopen(recordsPath.c_str(), "rb");
            if (!rf)
                return SetError(error, "cannot open records.dat for digest");
            fseeko(rf, (off_t)BLOCK_INDEX_RECORDS_HEADER_SIZE_V1, SEEK_SET);
            std::vector<unsigned char> recBuf(BLOCK_INDEX_RECORD_SIZE_V1);
            for (uint64_t i = 0; i < totalRecords; ++i)
            {
                if (fread(&recBuf[0], 1, BLOCK_INDEX_RECORD_SIZE_V1, rf) != BLOCK_INDEX_RECORD_SIZE_V1)
                {
                    fclose(rf);
                    return SetError(error, "records.dat truncated for digest");
                }
                SHA256_Update(&ctx, &recBuf[0], BLOCK_INDEX_RECORD_SIZE_V1);
            }
            fclose(rf);
            SHA256_Final(recordsDigest, &ctx);
        }

        // Active digest: SHA256 of all active entry bytes
        unsigned char activeDigest[32];
        {
            SHA256_CTX ctx;
            SHA256_Init(&ctx);
            std::string activePath = generationDir + "/" + BLOCK_INDEX_ACTIVE_FILE_NAME;
            FILE* af = fopen(activePath.c_str(), "rb");
            if (!af)
                return SetError(error, "cannot open active.dat for digest");
            fseeko(af, (off_t)BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1, SEEK_SET);
            std::vector<unsigned char> actBuf(BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1);
            for (size_t h = 0; h < activeChainByHeight.size(); ++h)
            {
                if (fread(&actBuf[0], 1, BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1, af) != BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1)
                {
                    fclose(af);
                    return SetError(error, "active.dat truncated for digest");
                }
                SHA256_Update(&ctx, &actBuf[0], BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1);
            }
            fclose(af);
            SHA256_Final(activeDigest, &ctx);
        }

        // Hashindex digest: SHA256 of all (hash, RecordId) pairs in RecordId order
        unsigned char hashIndexDigest[32];
        {
            SHA256_CTX ctx;
            SHA256_Init(&ctx);
            for (size_t i = 0; i < appended.size(); ++i)
            {
                // Hash: 32 raw bytes
                SHA256_Update(&ctx, appended[i].first.begin(), 32);
                // RecordId: 4 bytes LE
                unsigned char idBuf[4];
                BlockIndexId id = appended[i].second;
                idBuf[0] = (unsigned char)(id & 0xff);
                idBuf[1] = (unsigned char)((id >> 8) & 0xff);
                idBuf[2] = (unsigned char)((id >> 16) & 0xff);
                idBuf[3] = (unsigned char)((id >> 24) & 0xff);
                SHA256_Update(&ctx, idBuf, 4);
            }
            SHA256_Final(hashIndexDigest, &ctx);
        }

        // DAG input digest: SHA256 of all DAG link entries in hash order
        unsigned char dagInputDigest[32];
        {
            SHA256_CTX ctx;
            SHA256_Init(&ctx);
            for (std::map<uint256, uint256>::const_iterator it = source.dagScores.begin();
                 it != source.dagScores.end(); ++it)
            {
                SHA256_Update(&ctx, it->first.begin(), 32);
                SHA256_Update(&ctx, it->second.begin(), 32);
            }
            SHA256_Final(dagInputDigest, &ctx);
        }

        // Compute generation root
        unsigned char generationRoot[32];
        ComputeGenerationRoot(generation, tipHash, totalRecords,
                              recordsDigest, activeDigest, hashIndexDigest,
                              derivedEntriesDigest, dagInputDigest, generationRoot);

        // Update derived header with generation root
        derivedStore.SetContentBinding(generationRoot);

        if (!derivedStore.Finalize(error))
            return false;
    }

    FixedBlockIndexManifest manifest = store.GetManifest();
    manifest.state = BLOCK_INDEX_MANIFEST_BUILDING;
    manifest.recordCount = totalRecords;
    manifest.committedTipId = tipId;
    manifest.committedTipHeight = tipHeight;
    manifest.committedTipHash = tipHash;
    // A.10.1b-fix2: Set explicit capability for authoritative generations
    manifest.capability = BLOCK_INDEX_GENERATION_CAPABILITY_AUTHORITATIVE;
    if (!store.WriteManifest(manifest, error))
        return false;

    // Verify decoration before marking COMPLETE
    FixedBlockIndexManifest verify = store.GetManifest();
    verify.state = BLOCK_INDEX_MANIFEST_COMPLETE;
    verify.capability = BLOCK_INDEX_GENERATION_CAPABILITY_AUTHORITATIVE;
    if (!store.WriteManifest(verify, error))
        return false;

    if (stats)
    {
        stats->totalRecords = totalRecords;
        stats->activeRecords = activeChainByHeight.size();
        stats->sideChainRecords = totalRecords - activeChainByHeight.size();
        stats->activeTipHeight = tipHeight;
        stats->activeTipHash = tipHash;
        stats->hasDerived = true;
    }

    built = true;
    ClearError(error);
    return true;
}

namespace {

// Compare every V1 field of two records. Returns true when identical.
static bool RecordsMatchV1(const BlockIndexRecord& a, const BlockIndexRecord& b)
{
    if (a.hash != b.hash) return false;
    if (a.hashPrev != b.hashPrev) return false;
    if (a.hashMerkleRoot != b.hashMerkleRoot) return false;
    if (a.hashProof != b.hashProof) return false;
    if (!(a.prevoutStake == b.prevoutStake)) return false;
    if (a.height != b.height) return false;
    if (a.nFile != b.nFile) return false;
    if (a.nBlockPos != b.nBlockPos) return false;
    if (a.nFlags != b.nFlags) return false;
    if (a.nVersion != b.nVersion) return false;
    if (a.nTime != b.nTime) return false;
    if (a.nBits != b.nBits) return false;
    if (a.nNonce != b.nNonce) return false;
    if (a.nMint != b.nMint) return false;
    if (a.nMoneySupply != b.nMoneySupply) return false;
    if (a.nStakeModifier != b.nStakeModifier) return false;
    if (a.nStakeTime != b.nStakeTime) return false;
    return true;
}

// deterministic PRNG (xorshift64), fixed seed for repeatable samples
struct XorShift64
{
    uint64_t s;
    explicit XorShift64(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ULL) {}
    uint64_t next()
    {
        uint64_t x = s;
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        s = x;
        return x;
    }
    uint64_t range(uint64_t n)
    {
        return n ? (next() % n) : 0;
    }
};

} // namespace

bool VerifyGenerationAgainstSource(const BlockIndexGenerationSource& source,
                                   const std::string& generationDir,
                                   uint64_t generation,
                                   BlockIndexDifferentialResult* out,
                                   std::string* error)
{
    if (!out)
        return SetError(error, "null differential output");

    // Reopen the MANIFEST + hashindex from disk (independent of builder state).
    FixedBlockIndexOpenOptions storeOptions;
    storeOptions.requireCompleteManifest = true;
    FixedBlockIndexStore store;
    if (!FixedBlockIndexStore::OpenReadOnly(generationDir, storeOptions, &store, error))
        return false;
    BlockIndexHashIndex hashIndex;
    if (!BlockIndexHashIndex::Open(generationDir, generation, &hashIndex, error))
        return false;
    if (hashIndex.Generation() != store.GetManifest().generation)
        return SetError(error, "generation mismatch on reopen");
    const FixedBlockIndexManifest& manifest = store.GetManifest();

    // Load the WHOLE records.dat committed region and active.dat into memory in
    // a single buffered pass each (fresh read from disk, then decoded through
    // the same V1 codec). This avoids ~N file opens per record while still
    // proving the persisted generation is self-consistent from disk.
    std::vector<BlockIndexRecord> recordsById;
    {
        std::string diskDir = generationDir;
        // decode records.dat body (committed count from manifest)
        const uint64_t count = manifest.recordCount;
        std::vector<unsigned char> fileBytes;
        {
            FILE* f = fopen((boost::filesystem::path(diskDir) / BLOCK_INDEX_RECORDS_FILE_NAME).string().c_str(), "rb");
            if (!f)
                return SetError(error, "cannot open records.dat for differential");
            fseeko(f, (off_t)BLOCK_INDEX_RECORDS_HEADER_SIZE_V1, SEEK_SET);
            // read committed region in chunks of 64 records to bound memory churn
            std::vector<BlockIndexRecord> all;
            all.reserve(count);
            std::vector<unsigned char> recBuf(BLOCK_INDEX_RECORD_SIZE_V1);
            for (uint64_t i = 0; i < count; ++i)
            {
                size_t n = fread(&recBuf[0], 1, BLOCK_INDEX_RECORD_SIZE_V1, f);
                if (n != BLOCK_INDEX_RECORD_SIZE_V1)
                {
                    fclose(f);
                    return SetError(error, "records.dat truncated in differential");
                }
                BlockIndexRecord rec;
                if (!DecodeBlockIndexRecordV1(&recBuf[0], recBuf.size(), &rec, error))
                {
                    fclose(f);
                    return false;
                }
                all.push_back(rec);
            }
            fclose(f);
            recordsById.swap(all);
        }
    }
    // active.dat -> vector of RecordId indexed by height (skip the 40-byte
    // versioned header; entries are decoded via the exported entry codec).
    std::vector<BlockIndexId> activeById;
    {
        FILE* f = fopen((boost::filesystem::path(generationDir) / BLOCK_INDEX_ACTIVE_FILE_NAME).string().c_str(), "rb");
        if (!f)
            return SetError(error, "cannot open active.dat for differential");
        std::vector<unsigned char> buf(BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1);
        fseeko(f, (off_t)BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1, SEEK_SET);
        std::vector<BlockIndexId> all;
        for (int32_t h = 0; h <= manifest.committedTipHeight; ++h)
        {
            size_t n = fread(&buf[0], 1, BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1, f);
            if (n != BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1)
            {
                fclose(f);
                return SetError(error, "active.dat truncated in differential");
            }
            BlockIndexId id = BLOCK_INDEX_ID_INVALID;
            if (!DecodeBlockIndexActiveEntry((const char*)&buf[0], buf.size(), &id, error))
            {
                fclose(f);
                return false;
            }
            all.push_back(id);
        }
        fclose(f);
        activeById.swap(all);
    }

    BlockIndexDifferentialResult r;

    // ---- index the source for O(1)/O(log n) lookups (avoids O(n^2) scans) ----
    std::map<uint256, const BlockIndexGenerationSourceRecord*> sourceByHash;
    for (size_t i = 0; i < source.records.size(); ++i)
        sourceByHash[source.records[i].hash] = &source.records[i];

    // ---- active chain truth from source ----
    std::vector<const BlockIndexGenerationSourceRecord*> activeTruth; // index by height
    std::set<uint256> activeHashSet;
    if (source.foundBestChain)
    {
        std::vector<const BlockIndexGenerationSourceRecord*> byHeight;
        uint256 cur = source.hashBestChain;
        while (true)
        {
            std::map<uint256, const BlockIndexGenerationSourceRecord*>::iterator it =
                sourceByHash.find(cur);
            const BlockIndexGenerationSourceRecord* rec = (it != sourceByHash.end()) ? it->second : NULL;
            if (!rec)
            {
                r.tipCoherent = false;
                r.tipDetail = "source best-chain record missing";
                *out = r;
                ClearError(error);
                return true;
            }
            byHeight.push_back(rec);
            if (rec->record.hashPrev == 0)
                break;
            cur = rec->record.hashPrev;
        }
        std::sort(byHeight.begin(), byHeight.end(),
                  [](const BlockIndexGenerationSourceRecord* a,
                     const BlockIndexGenerationSourceRecord* b) {
                      return a->record.height < b->record.height;
                  });
        activeTruth = byHeight;
        for (size_t h = 0; h < activeTruth.size(); ++h)
            activeHashSet.insert(activeTruth[h]->hash);
    }

    // ---- exhaustive hash differential ----
    for (size_t i = 0; i < source.records.size(); ++i)
    {
        const uint256& hash = source.records[i].hash;
        const BlockIndexRecord& truth = source.records[i].record;
        ++r.hashQueries;

        BlockIndexId id = BLOCK_INDEX_ID_INVALID;
        BlockIndexHashLookupStatus st = hashIndex.Lookup(hash, &id, error);
        if (st == BLOCK_INDEX_HASH_LOOKUP_ERROR)
        {
            ++r.hashCorruptions;
            ClearError(error);
            continue;
        }
        if (st == BLOCK_INDEX_HASH_LOOKUP_NOT_FOUND)
        {
            ++r.hashNotFound;
            continue;
        }
        if (id == BLOCK_INDEX_ID_INVALID || id > recordsById.size())
        {
            ++r.hashCorruptions;
            continue;
        }
        const BlockIndexRecord& rec = recordsById[id - 1];
        if (!RecordsMatchV1(rec, truth))
            ++r.hashMismatches;
    }

    // ---- exhaustive active-height differential ----
    for (size_t h = 0; h < activeTruth.size(); ++h)
    {
        const int32_t height = (int32_t)h;
        ++r.heightQueries;
        if ((size_t)height >= activeById.size())
        {
            ++r.heightCorruptions;
            continue;
        }
        BlockIndexId id = activeById[height];
        if (id == BLOCK_INDEX_ID_INVALID || id > recordsById.size())
        {
            ++r.heightCorruptions;
            continue;
        }
        const BlockIndexRecord& rec = recordsById[id - 1];
        if (!RecordsMatchV1(rec, activeTruth[h]->record))
            ++r.heightMismatches;

        // hashindex(active hash).id == active.dat[height].id
        BlockIndexId hid = BLOCK_INDEX_ID_INVALID;
        BlockIndexHashLookupStatus hst = hashIndex.Lookup(activeTruth[h]->hash, &hid, error);
        if (hst == BLOCK_INDEX_HASH_LOOKUP_ERROR)
        {
            ++r.heightCorruptions;
            ClearError(error);
        }
        else if (hst == BLOCK_INDEX_HASH_LOOKUP_FOUND && hid != id)
            ++r.heightMismatches;
    }

    // ---- active parent continuity ----
    for (size_t h = 1; h < activeTruth.size(); ++h)
    {
        ++r.parentChecks;
        if (activeTruth[h]->record.hashPrev != activeTruth[h - 1]->record.hash)
            ++r.parentMismatches;
    }

    // ---- side-chain differential ----
    for (size_t i = 0; i < source.records.size(); ++i)
    {
        const uint256& hash = source.records[i].hash;
        if (activeHashSet.count(hash))
            continue;
        // side record: must resolve by hash
        ++r.sideChainSamples;
        BlockIndexId id = BLOCK_INDEX_ID_INVALID;
        if (hashIndex.Lookup(hash, &id, error) != BLOCK_INDEX_HASH_LOOKUP_FOUND)
        {
            ++r.sideChainMismatches;
            ClearError(error);
            continue;
        }
        if (id == BLOCK_INDEX_ID_INVALID || id > recordsById.size())
        {
            ++r.sideChainMismatches;
            continue;
        }
        if (!RecordsMatchV1(recordsById[id - 1], source.records[i].record))
            ++r.sideChainMismatches;
        // and must NOT be the active entry at its height
        int32_t hgt = source.records[i].record.height;
        if (hgt >= 0 && (int32_t)hgt < (int32_t)activeById.size())
        {
            if (activeById[hgt] == id)
                ++r.sideChainMismatches; // side block wrongly active
        }
    }

    // ---- deterministic random sample ----
    XorShift64 rng(0x5EED5EED5EED5EE5ULL);
    const uint64_t sampleCount = 10000;
    // random hashes
    for (uint64_t k = 0; k < sampleCount && !source.records.empty(); ++k)
    {
        ++r.randomHashChecks;
        const BlockIndexGenerationSourceRecord& rec =
            source.records[rng.range(source.records.size())];
        BlockIndexId id = BLOCK_INDEX_ID_INVALID;
        if (hashIndex.Lookup(rec.hash, &id, error) != BLOCK_INDEX_HASH_LOOKUP_FOUND)
        {
            ++r.randomMismatches;
            ClearError(error);
            continue;
        }
        if (id == BLOCK_INDEX_ID_INVALID || id > recordsById.size() ||
            !RecordsMatchV1(recordsById[id - 1], rec.record))
            ++r.randomMismatches;
    }
    // random active heights
    for (uint64_t k = 0; k < sampleCount && !activeTruth.empty(); ++k)
    {
        ++r.randomHeightChecks;
        const size_t h = rng.range(activeTruth.size());
        BlockIndexId id = BLOCK_INDEX_ID_INVALID;
        if ((size_t)h >= activeById.size())
        {
            ++r.randomMismatches;
            continue;
        }
        id = activeById[h];
        if (id == BLOCK_INDEX_ID_INVALID || id > recordsById.size() ||
            !RecordsMatchV1(recordsById[id - 1], activeTruth[h]->record))
            ++r.randomMismatches;
    }

    // ---- tip coherence ----
    r.tipCoherent = true;
    if (!source.foundBestChain || activeTruth.empty())
    {
        r.tipCoherent = false;
        r.tipDetail = "no best chain in source";
    }
    else
    {
        const BlockIndexGenerationSourceRecord* tip = activeTruth.back();
        const uint256 manifestTipHash = manifest.committedTipHash;
        if ((size_t)tip->record.height >= activeById.size())
        {
            r.tipCoherent = false;
            r.tipDetail = "active tip height out of range";
        }
        else
        {
            const BlockIndexId activeTipId = activeById[tip->record.height];
            const BlockIndexRecord& tipRec = recordsById[activeTipId - 1];
            if (tip->hash != tipRec.hash) r.tipCoherent = false;
            if (manifest.committedTipId != activeTipId) r.tipCoherent = false;
            if (manifest.committedTipHeight != tip->record.height) r.tipCoherent = false;
            if (manifest.committedTipHash != tipRec.hash) r.tipCoherent = false;
            if (manifest.committedTipHash != tip->hash) r.tipCoherent = false;
            if (tipRec.height != tip->record.height) r.tipCoherent = false;
            if (!r.tipCoherent)
                r.tipDetail = "tip identity mismatch between legacy/MANIFEST/active.dat/record";
        }
    }

    *out = r;
    ClearError(error);
    return true;
}