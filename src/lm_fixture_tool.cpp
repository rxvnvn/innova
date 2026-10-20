// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.
//
// Fixture builder: reads the FIRST <maxRecords> blockindex records from a
// legacy txleveldb snapshot (iterator order = hash order) and writes a NEW
// small leveldb containing exactly those records plus hashBestChain of the
// highest-height record on the real chain (the parent of the first chain
// root present -- resolved by walking hashPrev within the kept set).
//
// usage: innova-lm-fixture <src_snapshot_dir> <dst_dir> <maxRecords>
#include "main.h"
#include "ui_interface.h"
#include "checkpoints.h"
#include "util.h"

#include <leveldb/db.h>
#include <leveldb/filter_policy.h>
#include <cstdio>
#include <cstring>
#include <string>

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
    if (argc != 4)
    {
        fprintf(stderr, "usage: %s <src_snapshot_dir> <dst_dir> <maxRecords>\n", argv[0]);
        return 2;
    }
    const std::string src = argv[1];
    const std::string dst = argv[2];
    const uint64_t maxRecords = (uint64_t)atoll(argv[3]);

    leveldb::Options ro;
    ro.create_if_missing = false;
    ro.error_if_exists = false;
    ro.filter_policy = leveldb::NewBloomFilterPolicy(10);
    leveldb::DB* sdb = NULL;
    leveldb::Status st = leveldb::DB::Open(ro, src, &sdb);
    if (!st.ok()) { fprintf(stderr, "fixture: src open failed: %s\n", st.ToString().c_str()); return 1; }

    // collect first maxRecords blockindex records
    std::vector<std::pair<std::string, std::string> > rows;
    {
        leveldb::Iterator* it = sdb->NewIterator(leveldb::ReadOptions());
        CDataStream ssStartKey(SER_DISK, CLIENT_VERSION);
        ssStartKey << make_pair(std::string("blockindex"), uint256(0));
        it->Seek(ssStartKey.str());
        while (it->Valid() && rows.size() < maxRecords)
        {
            CDataStream ssKey(SER_DISK, CLIENT_VERSION);
            ssKey.write(it->key().data(), it->key().size());
            std::string strType;
            ssKey >> strType;
            if (strType != "blockindex")
                break;
            std::string k(it->key().data(), it->key().size());
            std::string v(it->value().data(), it->value().size());
            rows.push_back(std::make_pair(k, v));
            it->Next();
        }
        delete it;
    }

    // hashBestChain: hashPrev links the kept set; the chain root within the
    // kept set whose parent is NOT in the kept set becomes the SET's tip only
    // if the real chain tip is inside; otherwise re-link to the kept record
    // with the greatest height on the MAIN chain. Simpler fail-closed choice:
    // find the kept record with max height that still WALKS to genesis purely
    // inside the kept set. With hash-ordered truncation that is the genesis
    // side, so instead pick: the kept rec whose chain reaches BOTH genesis
    // (height 0 with null parent) AND the greatest height. Walk each kept
    // record's ancestry, bounded by the kept set, and track the longest.
    // Contiguous fixture: keep heights 0..(maxRecords-1) of the REAL main
    // chain. Walk down from the real hashBestChain through parent links using
    // point Get()s on the source DB (bounded by maxRecords), then emit those
    // exact keys/values into the destination (plus hashBestChain = the kept
    // TOP record hash -- the migration M3 walk must reach genesis inside the
    // kept set, which heights 0..maxRecords-1 always do).
    uint256 realBest;
    {
        CDataStream ssBestKey(SER_DISK, CLIENT_VERSION);
        ssBestKey << std::string("hashBestChain");
        std::string bestVal;
        leveldb::Status bs = sdb->Get(leveldb::ReadOptions(), ssBestKey.str(), &bestVal);
        if (!bs.ok()) { fprintf(stderr, "fixture: src hashBestChain missing\n"); return 1; }
        CDataStream ss(bestVal.data(), bestVal.data() + bestVal.size(), SER_DISK, CLIENT_VERSION);
        ss >> realBest;
    }
    // Collect the main-chain ancestry FROM THE TIP DOWN, but retain ONLY the
    // GENESIS-SIDE contiguouse window: heights 0..maxRecords-1. Bounded RAM:
    // a deque of the last maxRecords (hash,row) pairs.
    std::map<uint256, CDiskBlockIndex> keptByHash;
    std::map<std::string, std::string> keptRows;
    {
        std::vector<std::pair<std::string, std::string> > window; // tip..down, ring of maxRecords
        window.reserve(maxRecords);
        std::map<uint256, CDiskBlockIndex> ringHashes;
        uint256 cur = realBest;
        for (;;)
        {
            CDataStream ssKey(SER_DISK, CLIENT_VERSION);
            ssKey << make_pair(std::string("blockindex"), cur);
            std::string v;
            leveldb::Status gs = sdb->Get(leveldb::ReadOptions(), ssKey.str(), &v);
            if (!gs.ok()) { fprintf(stderr, "fixture: ancestor lookup failed at %s\n", cur.GetHex().c_str()); return 1; }
            CDataStream ssValue(SER_DISK, CLIENT_VERSION);
            ssValue.write(v.data(), v.size());
            CDiskBlockIndex di;
            ssValue >> di;
            if (window.size() >= maxRecords)
            {
                // evict the OLDEST (highest height) element
                const std::string& evictKey = window.front().first;
                size_t start = evictKey.size() - 32; // key suffix = the hash (DISPLAY order)
                // reconstruct hash from the evicted row instead (cheap, display->uint256)
                CDataStream ssEvict(SER_DISK, CLIENT_VERSION);
                ssEvict.write(window.front().second.data(), window.front().second.size());
                CDiskBlockIndex edi;
                ssEvict >> edi;
                ringHashes.erase(edi.GetBlockHash());
                window.erase(window.begin());
            }
            window.push_back(std::make_pair(ssKey.str(), v));
            ringHashes[di.GetBlockHash()] = di;
            if (di.hashPrev == uint256(0))
                break;
            cur = di.hashPrev;
            if (window.size() >= maxRecords && di.nHeight == 0)
                break;
        }
        for (size_t i = 0; i < window.size(); ++i)
            keptRows[window[i].first] = window[i].second;
        keptByHash = ringHashes;
    }
    if (keptRows.size() < 2)
    {
        fprintf(stderr, "fixture: kept set too small\n");
        return 1;
    }
    // hashBestChain = the kept set's TOP (max height within the kept set).
    uint256 bestHash = uint256(0);
    int bestH = -1;
    for (std::map<uint256, CDiskBlockIndex>::const_iterator kt = keptByHash.begin();
         kt != keptByHash.end(); ++kt)
        if (kt->second.nHeight > bestH)
        { bestH = kt->second.nHeight; bestHash = kt->first; }
    if (bestHash == uint256(0) || bestH <= 0)
    {
        fprintf(stderr, "fixture: kept set lacks a valid top\n");
        return 1;
    }

    leveldb::Options wo;
    wo.create_if_missing = true;
    wo.error_if_exists = true;
    wo.filter_policy = leveldb::NewBloomFilterPolicy(10);
    leveldb::DB* ddb = NULL;
    st = leveldb::DB::Open(wo, dst, &ddb);
    if (!st.ok()) { fprintf(stderr, "fixture: dst open failed: %s\n", st.ToString().c_str()); return 1; }

    for (std::map<std::string, std::string>::const_iterator it = keptRows.begin();
         it != keptRows.end(); ++it)
    {
        leveldb::Status ws = ddb->Put(leveldb::WriteOptions(), it->first, it->second);
        if (!ws.ok()) { fprintf(stderr, "fixture: put failed\n"); return 1; }
    }
    {
        CDataStream ssBestKey(SER_DISK, CLIENT_VERSION);
        ssBestKey << std::string("hashBestChain");
        CDataStream ssVal(SER_DISK, CLIENT_VERSION);
        ssVal << bestHash;
        leveldb::Status ws = ddb->Put(leveldb::WriteOptions(), ssBestKey.str(), ssVal.str());
        if (!ws.ok()) { fprintf(stderr, "fixture: put hashBestChain failed\n"); return 1; }
    }
    delete sdb; delete ddb;
    fprintf(stderr, "fixture: wrote %zu records, tip=%s (contiguous genesis-side set)\n",
            keptRows.size(), bestHash.GetHex().c_str());
    return 0;
}
