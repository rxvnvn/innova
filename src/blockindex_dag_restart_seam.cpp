// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "blockindex_dag_restart_seam.h"
#include "blockindex_v2_reader.h"
#include "dag.h"

#include <leveldb/db.h>
#include <leveldb/filter_policy.h>
#include <leveldb/iterator.h>

#include <stdio.h>

namespace {

// Read canonical nDAGScore (== restored nChainTrust) per block from the LevelDB
// "daglinks" store (same prefix/serialization as CTxDB::IterateDAGLinks / the
// builder's ReadDAGLinksFromSnapshot). A block is emitted only when its
// persisted score is nonzero (mirroring the legacy mapDAGData[hash]==0 skip).
bool ReadCanonicalScoresImpl(const std::string& dagLinksDir,
                             std::map<uint256, uint256>* scores,
                             std::string* error)
{
    scores->clear();

    leveldb::Options options;
    options.create_if_missing = false;
    options.error_if_exists = false;
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    leveldb::DB* db = NULL;
    leveldb::Status status = leveldb::DB::Open(options, dagLinksDir, &db);
    if (!status.ok())
    {
        if (error) *error = std::string("DAG restart: LevelDB open failure: ") + status.ToString();
        return false;
    }

    leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
    CDataStream ssPrefix(SER_DISK, CLIENT_VERSION);
    ssPrefix << std::string("daglinks");
    const std::string strPrefix = ssPrefix.str();
    it->Seek(strPrefix);

    while (it->Valid())
    {
        leveldb::Slice keySlice = it->key();
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

        CDataStream ssValue(SER_DISK, CLIENT_VERSION);
        ssValue.write(it->value().data(), it->value().size());
        CBlockDAGData dagData;
        ssValue >> dagData;

        if (!(dagData.nDAGScore == uint256(0)))
            (*scores)[blockHash] = dagData.nDAGScore;

        it->Next();
    }

    delete it;
    delete db;
    if (error) error->clear();
    return true;
}

} // namespace

BlockIndexDagRestartSeam::BlockIndexDagRestartSeam()
{
}

bool BlockIndexDagRestartSeam::ComputeRestore(
    const std::string& dagLinksDir,
    const BlockIndexStartupAuthority& authority,
    int forkHeightDAG,
    DagRestartResult* out,
    std::string* error) const
{
    if (!out)
        return false;
    out->ok = false;
    out->totalRestored = 0;
    out->restore.clear();
    out->error.clear();

    // Canonical DAG scores from the persisted daglinks store.
    std::map<uint256, uint256> scores;
    if (!ReadCanonicalScoresImpl(dagLinksDir, &scores, error))
        return false;

    // The seam is a by-value, read-only reconstruction. To decide height + PoW
    // status per hash WITHOUT mapBlockIndex, we need the by-value V2 reader of
    // the selected authoritative generation. This is exposed only on the
    // concrete V2BlockIndexStartupAuthority.
    const V2BlockIndexStartupAuthority* v2 =
        dynamic_cast<const V2BlockIndexStartupAuthority*>(&authority);
    if (!v2 || !v2->ReaderPtr())
    {
        if (error) *error = "DAG restart: authority is not V2 by-value (no bound reader)";
        return false;
    }
    const BlockIndexV2Reader* reader = v2->ReaderPtr();

    // Legacy oracle (dag.cpp:958-982): restore nChainTrust = nDAGScore for every
    // block that is (a) post-DAG height, (b) PoW (not PoS), and (c) present with
    // a nonzero score. We reproduce the SAME decision by iterating the persisted
    // daglinks scores (O(N) on disk, no mapBlockIndex) and resolving height/PoW
    // from the by-value reader snapshot.
    for (const auto& sc : scores)
    {
        const uint256& hash = sc.first;
        const uint256& score = sc.second;
        if (score == uint256(0))
            continue;

        BlockIndexSnapshot snap;
        std::string err;
        BlockIndexV2ReadStatus st = reader->LookupByHash(hash, &snap, &err);
        if (st == BLOCK_INDEX_V2_READ_NOT_FOUND)
            continue; // not in this generation -> legacy sees no entry -> skip
        if (st != BLOCK_INDEX_V2_READ_FOUND)
        {
            if (error) *error = "DAG restart: corrupt read for " + hash.ToString();
            return false;
        }
        if ((int)snap.height < forkHeightDAG)
            continue;
        if (snap.fProofOfStake || (snap.nFlags & (CBlockIndex::BLOCK_PROOF_OF_STAKE)))
            continue; // PoS blocks are not restored (legacy IsProofOfWork)

        DagRestartRestoreEntry e;
        e.hash = hash;
        e.dagScore = score;
        e.height = snap.height;
        out->restore.push_back(e);
    }

    out->totalRestored = out->restore.size();
    out->ok = true;
    return true;
}