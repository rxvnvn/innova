// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "blockindex_catchup_tool.h"

#include "blockindex_tip.h"
#include "blockindex_v2_reader.h"
#include "blockindex_derived_state.h"
#include "hash.h"
#include "main.h"
#include "kernel.h"
#include "util.h"
#include "bignum.h"

#include <leveldb/db.h>
#include <leveldb/filter_policy.h>
#include <leveldb/cache.h>
#include <boost/filesystem.hpp>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace fs = boost::filesystem;

// ---------------------------------------------------------------------------
// G2 — production legacy S->L catch-up.
//
// Reuses the low-memory builder's EXACT CDiskBlockIndex->BlockIndexRecord decode
// and the M4 derived-state formulas (blockindex_generation_builder_lm.cpp:
// DecodeToRecord lines ~65-91, M4 derive lines ~728-818) so the blockindex_tip
// it produces is byte/logical-identical to an offline rebuild. Memory is bounded
// independently of the delta (one record in flight + a height-ordered pass; the
// base generation's derived state for S is read by value from the reader).
//
// Expects the caller to supply a CONSISTENT COPY of the legacy txleveldb (never
// the concurrently-mutating live datadir). A short final consistency stop in the
// production runbook provides that copy.
// ---------------------------------------------------------------------------

namespace {

static bool SetErr(std::string* e, const std::string& m) { if (e) *e = m; return false; }
static void Clr(std::string* e) { if (e) e->clear(); }

// Byte-for-byte the LM builder's DecodeToRecord (blockindex_generation_builder_lm.cpp:65).
static bool DecodeToRecord(const leveldb::Slice& value, BlockIndexRecord* out)
{
    CDataStream ssValue(SER_DISK, CLIENT_VERSION);
    ssValue.write(value.data(), value.size());
    CDiskBlockIndex diskindex;
    ssValue >> diskindex;
    uint256 blockHash = diskindex.GetBlockHash();
    BlockIndexRecord& r = *out;
    r.hash = blockHash;
    r.hashPrev = diskindex.hashPrev;
    r.hashMerkleRoot = diskindex.hashMerkleRoot;
    r.hashProof = diskindex.hashProof;
    r.prevoutStake = diskindex.prevoutStake;
    r.height = diskindex.nHeight;
    r.nFile = diskindex.nFile;
    r.nBlockPos = diskindex.nBlockPos;
    r.nFlags = diskindex.nFlags;
    r.nVersion = diskindex.nVersion;
    r.nTime = diskindex.nTime;
    r.nBits = diskindex.nBits;
    r.nNonce = diskindex.nNonce;
    r.nMint = diskindex.nMint;
    r.nMoneySupply = diskindex.nMoneySupply;
    r.nStakeModifier = diskindex.nStakeModifier;
    r.nStakeTime = diskindex.nStakeTime;
    return true;
}

// Derived-state cache for the delta in height order. Mirrors the LM M4 compute:
// parent-keyed map filled as we stream heights ascending, so a record's parent
// derived is always already present (or comes from the base generation for the
// S+1 parent == S). Bounded by the delta that has been processed so far; released
// after the catch-up completes.
struct DerivedCache
{
    std::map<uint256, BlockIndexDerivedEntry> byHash;
};

// Compute the derived entry for one record given its parent's derived entry (or
// an empty entry for a no-parent / base-first record). Mirrors LM M4 lines
// 728-783 (chainTrust = parent + blockTrust; checksum via parent checksum +
// flags/proof/stakeModifier; memo; nSize from blk files when requested).
static void ComputeDerived(const BlockIndexRecord& rec,
                           const BlockIndexDerivedEntry& parentDerived,
                           const std::string& blockDataDir,
                           BlockIndexDerivedEntry* out)
{
    // chainTrust = parentTrust + blockTrust
    uint256 parentTrust = parentDerived.chainTrust;
    CBigNum bn; bn.SetCompact(rec.nBits);
    uint256 bt = 0;
    if (bn > 0 && (rec.height < GetForkHeightDAG() || rec.prevoutStake.hash == uint256(0)))
        bt = ((CBigNum(1)<<256)/(bn+1)).getuint256();
    out->chainTrust = parentTrust + bt;

    // checksum
    unsigned int parentChecksum = parentDerived.stakeModifierChecksum;
    CDataStream ss(SER_GETHASH, 0);
    if (rec.hashPrev != uint256(0)) ss << parentChecksum;
    uint256 proof = (rec.nFlags & CBlockIndex::BLOCK_PROOF_OF_STAKE) ? rec.hashProof : uint256(0);
    ss << rec.nFlags << proof << rec.nStakeModifier;
    uint256 hc = Hash(ss.begin(), ss.end());
    hc >>= (256 - 32);
    out->stakeModifierChecksum = hc.Get64();

    // memo
    out->SetHasStakeModifierTime(false);
    out->stakeModifierTime = 0;
    if (rec.nFlags & CBlockIndex::BLOCK_STAKE_MODIFIER)
    {
        out->SetHasStakeModifierTime(true);
        out->stakeModifierTime = (int64_t)rec.nTime;
    }
    else if (rec.hashPrev != uint256(0) && parentDerived.HasStakeModifierTime())
    {
        out->SetHasStakeModifierTime(true);
        out->stakeModifierTime = parentDerived.stakeModifierTime;
    }

    // nSize (from blk files when available; else unavailable -> 0)
    out->SetHasBlockSize(false);
    out->nSize = 0;
    if (!blockDataDir.empty() && rec.nFile > 0)
    {
        std::string blockFn = strprintf("blk%04u.dat", rec.nFile);
        fs::path blockPath = fs::path(blockDataDir) / blockFn;
        FILE* blockFile = fopen(blockPath.string().c_str(), "rb");
        if (blockFile)
        {
            if (fseeko(blockFile, (off_t)rec.nBlockPos, SEEK_SET) == 0)
            {
                try {
                    CBlock block;
                    CAutoFile filein(blockFile, SER_DISK, CLIENT_VERSION);
                    filein >> block;
                    if (block.GetHash() == rec.hash)
                    {
                        out->nSize = ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION);
                        out->SetHasBlockSize(out->nSize > 0);
                    }
                } catch (...) { /* CAutoFile closed the FILE* */ }
            }
            else
                fclose(blockFile); // CAutoFile never constructed
        }
    }
}

} // namespace

bool RunBlockIndexCatchup(const std::string& v2Root,
                          const std::string& legacyLevelDb,
                          const std::string& blockDataDir,
                          int livetailHorizon,
                          BlockIndexCatchupResult* out)
{
    (void)livetailHorizon;
    if (!out) return false;
    *out = BlockIndexCatchupResult();
    out->error.clear();

    // 1. Open the validated AUTHORITATIVE generation reader (derive S).
    BlockIndexV2Reader reader;
    BlockIndexV2ReaderOptions opts;
    std::string err;
    if (!reader.Open(v2Root, opts, &err))
        return SetErr(&out->error, "catchup: open V2 root: " + err);
    if (!reader.IsOpen())
        return SetErr(&out->error, "catchup: V2 reader not open");
    const BlockIndexSnapshot baseTipSnap = reader.GetTip();
    if (!baseTipSnap.found)
        return SetErr(&out->error, "catchup: V2 generation has no base tip");
    const int32_t S = baseTipSnap.height;
    const uint256 sHash = baseTipSnap.hash;
    out->baseTipHeight = S;
    out->baseTipHash = sHash;

    // 2. Open the consistent legacy copy (derive L from hashBestChain).
    leveldb::Options options;
    options.create_if_missing = false;
    options.error_if_exists = false;
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    options.block_cache = leveldb::NewLRUCache(512 * 1024); // bounded
    options.write_buffer_size = 1 * 1024 * 1024;
    options.max_open_files = 64;
    leveldb::DB* db = NULL;
    leveldb::Status stOpen = leveldb::DB::Open(options, legacyLevelDb, &db);
    if (!stOpen.ok())
        return SetErr(&out->error, "catchup: open legacy LevelDB: " + stOpen.ToString());
    uint256 hashBest = 0;
    {
        CDataStream ssBestKey(SER_DISK, CLIENT_VERSION);
        ssBestKey << std::string("hashBestChain");
        std::string bestVal;
        leveldb::Status bs = db->Get(leveldb::ReadOptions(), ssBestKey.str(), &bestVal);
        if (!bs.ok())
        { delete db; return SetErr(&out->error, "catchup: hashBestChain missing in legacy copy"); }
        CDataStream ss(bestVal.data(), bestVal.data() + bestVal.size(), SER_DISK, CLIENT_VERSION);
        ss >> hashBest;
    }
    if (hashBest == uint256(0))
    { delete db; return SetErr(&out->error, "catchup: empty hashBestChain"); }

    // 3. Open (create if needed) the mutable tip under <v2Root>/blockindex_tip,
    //    anchored to the base generation (S). Fail closed on base/gen mismatch.
    const uint64_t gen = reader.Generation();
    const uint64_t baseRec = reader.RecordCount();
    BlockIndexTipAuthority tip;
    fs::path tipMeta = fs::path(v2Root) / "blockindex_tip" / "tip.meta";
    if (fs::exists(tipMeta))
    {
        if (!BlockIndexTipAuthority::Open(v2Root, gen, &tip, &err))
        { delete db; return SetErr(&out->error, "catchup: open tip: " + err); }
    }
    else
    {
        if (!BlockIndexTipAuthority::Create(v2Root, gen, baseRec, S, &tip, &err))
        { delete db; return SetErr(&out->error, "catchup: create tip: " + err); }
    }

    // 4. Stream legacy blockindex records, decode, and for each record with
    //    global height in (S, L], recompute derived state in ascending height
    //    order and append active (dense) / side to the tip. The S parent's
    //    derived is read by value from the base reader.
    {
        DerivedCache dcache;
        // Seed: base tip S derived entry (read by value from the generation).
        {
            BlockIndexSnapshot ss;
            std::string derr;
            if (reader.LookupByHash(sHash, &ss, &derr) == BLOCK_INDEX_V2_READ_FOUND && ss.found)
            {
                BlockIndexDerivedEntry d;
                d.chainTrust = ss.nChainTrust;
                d.stakeModifierChecksum = ss.nStakeModifierChecksum;
                d.SetHasStakeModifierTime(ss.hasStakeModifierTime);
                d.stakeModifierTime = ss.nStakeModifierTime;
                d.SetHasBlockSize(false); d.nSize = 0;
                dcache.byHash[sHash] = d;
            }
        }

        // Collect all blockindex records, sort by height asc (bounded by delta;
        // we use an external approach: since the active chain is dense and we
        // only need the active path S+1..L, walk from the base tip by following
        // hashPrev along the recorded records). For robustness and simplicity,
        // buffer records keyed by hash into a map (O(delta) disk-free), then walk
        // the active chain from sHash via hashPrev.
        std::map<uint256, BlockIndexRecord> byHash;
        {
            leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
            CDataStream ssStart(SER_DISK, CLIENT_VERSION);
            ssStart << make_pair(std::string("blockindex"), uint256(0));
            it->Seek(ssStart.str());
            while (it->Valid())
            {
                CDataStream ssKey(SER_DISK, CLIENT_VERSION);
                ssKey.write(it->key().data(), it->key().size());
                std::string t;
                ssKey >> t;
                if (t != "blockindex") break;
                BlockIndexRecord rec;
                if (DecodeToRecord(it->value(), &rec))
                    byHash[rec.hash] = rec;
                it->Next();
            }
            delete it;
        }

        // Walk the active chain from sHash forward to L (the legacy best), using
        // hashPrev linkage, collecting S+1..L in ascending order. Side branches
        // (records not on the walk) are appended as sides.
        std::vector<BlockIndexRecord> activePath; // ascending heights S+1..L
        std::vector<uint256> activePathHashes;
        uint256 cur = hashBest;
        // Build the path from L back to base tip, then reverse.
        std::vector<uint256> rev;
        while (cur != uint256(0) && cur != sHash && rev.size() < (size_t)0x1000000)
        {
            rev.push_back(cur);
            std::map<uint256, BlockIndexRecord>::iterator it = byHash.find(cur);
            if (it == byHash.end())
            { delete db; return SetErr(&out->error, "catchup: active link missing for " + cur.ToString()); }
            cur = it->second.hashPrev;
        }
        // rev is tip->...->S+1; reverse to ascending.
        for (int i = (int)rev.size() - 1; i >= 0; --i)
        {
            activePathHashes.push_back(rev[i]);
            activePath.push_back(byHash[rev[i]]);
        }
        if (activePath.empty())
        { delete db; return SetErr(&out->error, "catchup: no post-S active blocks (S==L?)"); }
        out->targetHeight = activePath.back().height;
        out->targetHash = activePath.back().hash;

        // Append active path S+1..L (dense heights ascending). One block per
        // AppendBatch call: BlockIndexTipAuthority::AppendBatch validates each
        // active height against the UNCHANGED committed tipHeight+1 inside the
        // loop (not a running next-height), so a multi-block active batch would
        // reject heights > tip+1. Single append per call matches the proven M7
        // usage and yields dense [S+1, S+2, ...].
        for (size_t i = 0; i < activePath.size(); ++i)
        {
            const BlockIndexRecord& rec = activePath[i];
            BlockIndexDerivedEntry parent;
            std::map<uint256, BlockIndexDerivedEntry>::iterator pIt =
                dcache.byHash.find(rec.hashPrev);
            if (pIt != dcache.byHash.end())
                parent = pIt->second;
            BlockIndexDerivedEntry d;
            ComputeDerived(rec, parent, blockDataDir, &d);
            dcache.byHash[rec.hash] = d;
            BlockIndexTipAppend a; a.record = rec; a.derived = d;
            // dense single-step append
            std::vector<BlockIndexTipAppend> one; one.push_back(a);
            std::vector<int32_t> oneH; oneH.push_back(rec.height);
            BlockIndexTipStatus as = tip.AppendBatch(one, oneH, &err);
            if (as != BLOCK_INDEX_TIP_OK)
            { delete db; return SetErr(&out->error, "catchup: append active: " + err); }
            out->appendedRecords++;
        }

        // Side branches: any record decoded that is NOT on the active path and
        // NOT == base tip -> record as side (non-active). Deterministic order by
        // height for reproducibility.
        std::vector<BlockIndexTipAppend> sideBlocks;
        std::vector<int32_t> sideHeights; // -1 => side
        for (std::map<uint256, BlockIndexRecord>::iterator it = byHash.begin();
             it != byHash.end(); ++it)
        {
            const uint256& h = it->first;
            if (h == sHash) continue;
            bool onPath = false;
            for (size_t i = 0; i < activePathHashes.size(); ++i)
                if (activePathHashes[i] == h) { onPath = true; break; }
            if (onPath) continue;
            BlockIndexTipAppend a; a.record = it->second;
            // side derived: compute from its own parent if available
            BlockIndexDerivedEntry parent;
            std::map<uint256, BlockIndexDerivedEntry>::iterator pIt =
                dcache.byHash.find(it->second.hashPrev);
            if (pIt != dcache.byHash.end())
                parent = pIt->second;
            BlockIndexDerivedEntry d;
            ComputeDerived(it->second, parent, blockDataDir, &d);
            a.derived = d;
            sideBlocks.push_back(a);
            sideHeights.push_back(-1);
            out->appendedRecords++;
        }
        if (!sideBlocks.empty())
        {
            BlockIndexTipStatus ss2 = tip.AppendBatch(sideBlocks, sideHeights, &err);
            if (ss2 != BLOCK_INDEX_TIP_OK)
            { delete db; return SetErr(&out->error, "catchup: append sides: " + err); }
        }
    }

    // 5. Effective tip after catch-up.
    out->finalTipHeight = tip.TipHeight();
    out->finalTipHash = tip.TipHash();
    out->ok = (out->finalTipHeight == out->targetHeight &&
               out->finalTipHash == out->targetHash);
    if (!out->ok)
    { delete db; return SetErr(&out->error, "catchup: effective tip != legacy target L"); }

    delete db;
    Clr(&out->error);
    return true;
}