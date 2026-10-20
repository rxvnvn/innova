// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.
//
// Independent differential verify: LEGACY txleveldb SNAPSHOT vs BUILT V2
// generation directories (records.dat / active.dat / derived.dat).
// Read-only; no consensus validation. Counters printed to stdout.
//
// usage: innova-lm-verify <snapshot_leveldb_dir> <generation_dir>
#include "blockindex_generation_builder.h"
#include "blockindex_generation_writer.h"
#include "fixed_blockindex_store.h"
#include "blockindex_derived_state.h"
#include "blockindex_activeindex.h"
#include "blockindex_hashindex.h"
#include "main.h"
#include "ui_interface.h"
#include "checkpoints.h"
#include "wallet.h"
#include "util.h"

#include <leveldb/db.h>
#include <leveldb/filter_policy.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// Globals required to link the shared engine OBJS (mirrors builder_tool.cpp).
CWallet* pwalletMain;
CClientUIInterface uiInterface;
bool fConfChange = false;
bool fEnforceCanonical = true;
bool fUseFastIndex = true;
unsigned int nDerivationMethodIndex = 0;
unsigned int nMinerSleep = 5000;
unsigned int nNodeLifespan = 7;
enum Checkpoints::CPMode CheckpointsMode = Checkpoints::STRICT;
void Shutdown(void* parg) { exit(0); }
void StartShutdown() { exit(0); }

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: %s <snapshot_leveldb_dir> <generation_dir>\n", argv[0]);
        return 2;
    }
    const std::string snapDir = argv[1];
    const std::string genDir = argv[2];

    // ---- source snapshot -> hash -> (height, hashPrev, nFile, nBlockPos) ----
    leveldb::Options options;
    options.create_if_missing = false;
    options.error_if_exists = false;
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    leveldb::DB* db = NULL;
    leveldb::Status status = leveldb::DB::Open(options, snapDir, &db);
    if (!status.ok())
    {
        fprintf(stderr, "VERIFY_FATAL: snapshot open failed: %s\n", status.ToString().c_str());
        return 1;
    }
    std::map<uint256, CDiskBlockIndex> srcByHash;
    uint64_t srcCount = 0;
    {
        leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
        CDataStream ssStartKey(SER_DISK, CLIENT_VERSION);
        ssStartKey << make_pair(std::string("blockindex"), uint256(0));
        it->Seek(ssStartKey.str());
        while (it->Valid())
        {
            CDataStream ssKey(SER_DISK, CLIENT_VERSION);
            ssKey.write(it->key().data(), it->key().size());
            std::string strType;
            ssKey >> strType;
            if (strType != "blockindex")
                break;
            CDataStream ssValue(SER_DISK, CLIENT_VERSION);
            ssValue.write(it->value().data(), it->value().size());
            CDiskBlockIndex di;
            ssValue >> di;
            srcByHash[di.GetBlockHash()] = di;
            ++srcCount;
            it->Next();
        }
        delete it;
    }
    uint256 srcBest;
    {
        CDataStream ssBestKey(SER_DISK, CLIENT_VERSION);
        ssBestKey << std::string("hashBestChain");
        std::string bestVal;
        leveldb::Status bs = db->Get(leveldb::ReadOptions(), ssBestKey.str(), &bestVal);
        if (!bs.ok())
        {
            fprintf(stderr, "VERIFY_FATAL: hashBestChain missing\n");
            return 1;
        }
        CDataStream ss(bestVal.data(), bestVal.data() + bestVal.size(), SER_DISK, CLIENT_VERSION);
        ss >> srcBest;
    }
    delete db;
    int srcTipHeight = (srcByHash.count(srcBest) ? srcByHash[srcBest].nHeight : -1);

    // ---- built generation MANIFEST ----
    FixedBlockIndexStore store;
    FixedBlockIndexOpenOptions openOptions;
    if (!FixedBlockIndexStore::OpenReadOnly(genDir, openOptions, &store, NULL))
    {
        fprintf(stderr, "VERIFY_FATAL: store open failed (requires COMPLETE manifest)\n");
        return 1;
    }
    const FixedBlockIndexManifest& man = store.GetManifest();
    fprintf(stderr, "VERIFY manifest: recordCount=%llu tipId=%llu tipHeight=%d tipHash=%s capability=%u state=%u\n",
            (unsigned long long)man.recordCount, (unsigned long long)man.committedTipId,
            man.committedTipHeight, man.committedTipHash.GetHex().c_str(),
            man.capability, man.state);

    // read all records by id
    std::vector<BlockIndexRecord> recs;
    recs.reserve(man.recordCount);
    {
        BlockIndexRecord r;
        uint64_t n = 0;
        while (store.Read((BlockIndexId)(n + 1), &r, NULL) && n < man.recordCount)
        {
            recs.push_back(r);
            ++n;
        }
    }

    // ---- active.dat: dense height -> id ----
    std::vector<BlockIndexId> activeIds;
    {
        FILE* f = fopen((boost::filesystem::path(genDir) / "active.dat").string().c_str(), "rb");
        if (!f) { fprintf(stderr, "VERIFY_FATAL: active.dat missing\n"); return 1; }
        fseeko(f, 0, SEEK_END);
        const off_t fsz = ftello(f);
        fseeko(f, BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1, SEEK_SET);
        const off_t nEntries = (fsz - BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1) / BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1;
        activeIds.resize(nEntries);
        for (off_t i = 0; i < nEntries; ++i)
        {
            unsigned char enc[8];
            BlockIndexId id = 0;
            if (fread(enc, 1, sizeof(enc), f) != sizeof(enc) ||
                !DecodeBlockIndexActiveEntry((const char*)enc, sizeof(enc), &id, NULL))
            {
                fprintf(stderr, "VERIFY_FATAL: active.dat entry decode failed at %lld\n", (long long)i);
                fclose(f);
                return 1;
            }
            activeIds[i] = id;
        }
        fclose(f);
    }

    // ---- derived.dat entries ----
    BlockIndexDerivedStateStore der;
    bool derivedReadable =
        BlockIndexDerivedStateStore::OpenReadOnly(genDir, man.generation, &der, NULL);

    // ---- counters ----
    uint64_t cntMismatches = 0;
    uint64_t parentMismatches = 0;
    uint64_t posMismatches = 0;
    uint64_t fileMismatches = 0;
    uint64_t activeGaps = 0;
    uint64_t activeBadHeight = 0;
    uint64_t activeBadTip = 0;
    uint64_t tipHashMismatch = 0;
    uint64_t badDerived = 0;
    uint64_t chainTrustMismatch = 0;
    uint64_t spotCheckMismatches = 0;

    // 1. record count vs source
    if (recs.size() != srcCount)
    {
        fprintf(stderr, "VERIFY_FAIL: generation record count %llu != source %llu\n",
                (unsigned long long)recs.size(), (unsigned long long)srcCount);
        ++cntMismatches;
    }

    // 2. per-record: height/hashPrev/nFile/nBlockPos/flags vs source by hash
    std::vector<BlockIndexId> oldToNew; // srcRecordId(from insertion order of map not useful) -- map by hash
    for (size_t i = 0; i < recs.size(); ++i)
    {
        const BlockIndexRecord& r = recs[i];
        std::map<uint256, CDiskBlockIndex>::const_iterator srcIt = srcByHash.find(r.hash);
        if (srcIt == srcByHash.end())
        {
            fprintf(stderr, "VERIFY_FAIL: generation record %llu hash %s not in source\n",
                    (unsigned long long)(i + 1), r.hash.GetHex().c_str());
            ++cntMismatches;
            continue;
        }
        const CDiskBlockIndex& d = srcIt->second;
        if (r.height != d.nHeight ||
            !(r.hashPrev == d.hashPrev))
        {
            ++cntMismatches;
            if (cntMismatches < 6)
            {
                fprintf(stderr, "VERIFY_FAIL: record %llu height/prev mismatch: gen(%d, %s) src(%d, %s)\n",
                        (unsigned long long)(i + 1), r.height, r.hashPrev.GetHex().c_str(),
                        d.nHeight, d.hashPrev.GetHex().c_str());
            }
        }
        if ((r.nFile != d.nFile) || (r.nBlockPos != d.nBlockPos))
            ++fileMismatches;
        if (r.nFlags != d.nFlags)
            ++posMismatches;
    }

    // 3. active.dat: every height maps to the record whose hash == the src
    //    hashBestChain ancestor chain at that height
    if ((int64_t)activeIds.size() != (int64_t)srcTipHeight + 1)
    {
        fprintf(stderr, "VERIFY_FAIL: active.dat heights %llu != expected %d (tip height)\n",
                (unsigned long long)activeIds.size(), srcTipHeight + 1);
        ++activeGaps;
    }
    // replay the chain downward from the tip in the SOURCE
    {
        uint256 cur = srcBest;
        std::vector<std::pair<int32_t, uint256> > chain; // tip first
        chain.reserve(srcTipHeight + 1);
        std::map<uint256, CDiskBlockIndex>::const_iterator cit;
        while ((cit = srcByHash.find(cur)) != srcByHash.end())
        {
            chain.push_back(std::make_pair(cit->second.nHeight, cit->second.GetBlockHash()));
            if (cur == uint256(0))
                break;
            cur = cit->second.hashPrev;
            if (cit->second.nHeight == 0)
                break;
        }
        if ((int64_t)chain.size() != (int64_t)srcTipHeight + 1)
        {
            fprintf(stderr, "VERIFY_FAIL: source active chain walk produced %llu entries, tip height %d\n",
                    (unsigned long long)chain.size(), srcTipHeight);
            ++activeGaps;
        }
        else
        {
            // ascending: tip-first chain reversed
            for (size_t h = 0; h < chain.size(); ++h)
            {
                const uint256 wantHash = chain[chain.size() - 1 - h].second; // chain[0]=tip, reversed -> genesis
                if ((int)h != chain[chain.size() - 1 - h].first)
                {
                    ++activeBadHeight;
                    continue;
                }
                if (h < (size_t)activeIds.size())
                {
                    const BlockIndexId id = activeIds[h];
                    if (id == 0 || (size_t)id > recs.size() || recs[id - 1].hash != wantHash)
                    {
                        ++activeGaps;
                        if (activeGaps < 6)
                        {
                            fprintf(stderr, "VERIFY_FAIL: active height %llu: gen hash %s != src hash %s (id=%llu)\n",
                                    (unsigned long long)h,
                                    (id && (size_t)id <= recs.size()) ? recs[id-1].hash.GetHex().c_str() : "(oob)",
                                    wantHash.GetHex().c_str(), (unsigned long long)id);
                        }
                    }
                }
            }
        }
    }

    // 4. MANIFEST tip hash + height vs source
    {
        // source tip
        std::map<uint256, CDiskBlockIndex>::const_iterator tit = srcByHash.find(srcBest);
        if (tit != srcByHash.end())
        {
            if (man.committedTipHash != srcBest ||
                man.committedTipHeight != tit->second.nHeight)
            {
                fprintf(stderr, "VERIFY_FAIL: manifest tip (%s h=%d) != source tip (%s h=%d)\n",
                        man.committedTipHash.GetHex().c_str(), man.committedTipHeight,
                        srcBest.GetHex().c_str(), tit->second.nHeight);
                ++tipHashMismatch;
            }
        }
    }

    // 5. derived entries: driven by active.dat RecordIds, ascending 0..tip;
    //    chainTrust must be strictly increasing past genesis (uint256).
    if (derivedReadable)
    {
        BlockIndexDerivedEntry e, pe;
        bool havePe = false;
        for (int32_t h = 0; h <= srcTipHeight && (size_t)h < activeIds.size(); ++h)
        {
            const BlockIndexId id = activeIds[h];
            BlockIndexDerivedLookupStatus ls = der.Read(id, &e, NULL);
            if (ls != BLOCK_INDEX_DERIVED_LOOKUP_FOUND)
            {
                ++badDerived;
                continue;
            }
            if (!e.HasBlockSize() || e.nSize == 0)
            {
                ++badDerived;
                continue;
            }
            if (havePe && !(pe.chainTrust < e.chainTrust))
                ++chainTrustMismatch;
            pe = e;
            havePe = true;
        }
    }

    fprintf(stderr, "VERIFY count=%llu srcCount=%llu cntMismatches=%llu parentMismatches=%llu "
            "posMismatches=%llu fileMismatches=%llu activeGaps=%llu activeBadHeight=%llu "
            "tipHashMismatch=%llu badDerived=%llu chainTrustMismatch=%llu spotCheckMismatches=%llu\n",
            (unsigned long long)recs.size(), (unsigned long long)srcCount,
            (unsigned long long)cntMismatches, (unsigned long long)parentMismatches,
            (unsigned long long)posMismatches, (unsigned long long)fileMismatches,
            (unsigned long long)activeGaps, (unsigned long long)activeBadHeight,
            (unsigned long long)tipHashMismatch, (unsigned long long)badDerived,
            (unsigned long long)chainTrustMismatch, (unsigned long long)spotCheckMismatches);

    return (cntMismatches == 0 && parentMismatches == 0 && posMismatches == 0 &&
            fileMismatches == 0 && activeGaps == 0 && activeBadHeight == 0 &&
            tipHashMismatch == 0 && badDerived == 0 && chainTrustMismatch == 0) ? 0 : 1;
}
