// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "blockindex_dag_restart_seam.h"
#include "blockindex_v2_reader.h"
#include "dag.h"
#include "txdb-leveldb.h"

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

// R3 / C6 section 6 + R3.8 — report the provenance custody state of the SAME durable
// store, using ONLY the read-only engine accessor and the durable custody records. No
// side effect (this seam never seals, never writes, never creates a certificate) and no
// interpretation: every non-VERIFIED outcome means "coverage unavailable / suspended",
// i.e. the affected reconstruction must fail closed rather than read empty state.
bool ReadCustodyReportImpl(const std::string& dir, int* state, std::string* detail)
{
    if (state) *state = 2;   // UNAVAILABLE by default: never assume coverage
    if (detail) detail->clear();

    leveldb::Options options;
    options.create_if_missing = false;
    options.error_if_exists = false;
    leveldb::DB* db = NULL;
    leveldb::Status status = leveldb::DB::Open(options, dir, &db);
    if (!status.ok())
    {
        if (detail) *detail = std::string("custody report: LevelDB open failure: ") + status.ToString();
        return false;
    }

    CDataStream ssSealKey(SER_DISK, CLIENT_VERSION);
    ssSealKey << std::string("dagcustodyseal");
    std::string rawSeal;
    const leveldb::Status sealStatus = db->Get(leveldb::ReadOptions(), ssSealKey.str(), &rawSeal);
    if (sealStatus.IsNotFound())
    {
        if (state) *state = 2;
        if (detail) *detail = "no custody seal: cross-session provenance coverage unavailable";
        delete db;
        return true;
    }
    if (!sealStatus.ok())
    {
        if (detail) *detail = "custody report: seal read failure: " + sealStatus.ToString();
        delete db;
        return false;
    }
    uint64_t sealedWatermark = 0;
    try
    {
        CDataStream s(rawSeal.data(), rawSeal.data() + rawSeal.size(), SER_DISK, CLIENT_VERSION);
        uint64_t epoch = 0, counter = 0;
        s >> sealedWatermark >> epoch >> counter;
        if (!s.empty()) { sealedWatermark = 0; }
    }
    catch (const std::exception&)
    {
        if (state) *state = 1;
        if (detail) *detail = "custody seal is malformed: coverage suspended";
        delete db;
        return true;
    }
    if (sealedWatermark == 0)
    {
        if (state) *state = 1;
        if (detail) *detail = "custody seal is malformed: coverage suspended";
        delete db;
        return true;
    }

    CDataStream ssCertKey(SER_DISK, CLIENT_VERSION);
    ssCertKey << std::string("dagcert");
    std::string rawCert;
    const leveldb::Status certStatus = db->Get(leveldb::ReadOptions(), ssCertKey.str(), &rawCert);
    if (!certStatus.ok() && !certStatus.IsNotFound())
    {
        if (detail) *detail = "custody report: certificate read failure: " + certStatus.ToString();
        delete db;
        return false;
    }
    const bool haveCert = certStatus.ok() && !rawCert.empty();

    std::string value;
    if (!db->GetProperty("leveldb.last-sequence", &value) || value.empty())
    {
        if (state) *state = 2;
        if (detail) *detail = "engine watermark capability unavailable: cross-session coverage does not survive";
        delete db;
        return true;
    }
    uint64_t last = 0;
    for (size_t i = 0; i < value.size(); ++i)
    {
        const char c = value[i];
        if (c < '0' || c > '9') { last = 0; break; }
        last = last * 10 + (uint64_t)(c - '0');
    }
    delete db;

    if (last != sealedWatermark)
    {
        if (state) *state = 1;
        if (detail) *detail = "custody watermark mismatch: LastSequence != sealed W (untagged/foreign write)";
        return true;
    }
    if (!haveCert)
    {
        if (state) *state = 2;
        if (detail) *detail = "no coverage certificate: cross-session provenance coverage unavailable";
        return true;
    }
    if (state) *state = 0;
    if (detail) *detail = "custody continuity verified; certificate present";
    return true;
}

} // namespace

bool EstablishDAGProvenanceCustodyAtStartup(int* custodyState, uint64_t* epoch, std::string* detail)
{
    if (custodyState) *custodyState = 2;
    if (epoch) *epoch = 0;
    if (detail) detail->clear();

    CTxDB db("r");   // read-only handle: this seam never writes, and Close() therefore never seals
    const DAGCustodyState state = db.GetDAGCustodyState();
    if (custodyState) *custodyState = (int)state;
    {
        uint64_t curEpoch = 0;
        if (db.ReadDAGCustodyEpoch(&curEpoch)) { if (epoch) *epoch = curEpoch; }
    }
    if (state == DAGCustodyState::VERIFIED)
    {
        if (detail) *detail = "custody continuity verified before publication";
        db.Close();
        return true;
    }
    if (state == DAGCustodyState::SUSPENDED)
    {
        if (detail) *detail = "custody suspended (watermark continuity lost): prior provenance inadmissible, never re-certified";
        db.Close();
        return true;
    }

    // UNAVAILABLE. Distinguish "nothing certified yet" (certifiable now from the store's own
    // durable state) from "a seal exists but did not verify" (capability loss / mismatch —
    // adversarial, must never be overridden by certification).
    DAGCustodySeal seal;
    bool haveSeal = false;
    if (!db.ReadDAGCustodySeal(&seal, &haveSeal))
    {
        if (detail) *detail = "custody seal unreadable";
        db.Close();
        return false;
    }
    if (haveSeal)
    {
        if (detail) *detail = "custody seal present but not verified: cross-session coverage unavailable (never re-certified, never overridden)";
        db.Close();
        return true;
    }

    int32_t hClean = 0, hFloor = 0;
    const bool haveClean = db.ReadDAGCleanHeight(hClean);
    const bool haveFloor = db.ReadDAGPruneFloor(hFloor);
    if (!haveClean && !haveFloor)
    {
        if (detail) *detail = "no certifiable provenance domain (no clean height and no prune floor): store remains UNCERTIFIED";
        db.Close();
        return true;
    }
    int32_t hCert = haveClean ? hClean : hFloor;
    if (haveFloor && hFloor > hCert) hCert = hFloor;

    uint64_t newEpoch = 0;
    std::string certErr;
    if (db.CertifyDAGProvenanceCoverage(hCert, &newEpoch, &certErr))
    {
        if (custodyState) *custodyState = 0;
        if (epoch) *epoch = newEpoch;
        if (detail) *detail = "certified at startup: custody epoch " + std::to_string(newEpoch) +
                              " established from durable state; no historical provenance manufactured";
        db.Close();
        return true;
    }
    if (custodyState) *custodyState = 2;
    if (detail) *detail = "certification at startup refused: " + certErr;
    db.Close();
    return true;
}

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

    // R3 / C6 section 6 + R3.8: report the provenance custody state of this same durable
    // store before it is used. This is a REPORT, never an interpretation of absence: a
    // watermark mismatch / absent-or-invalid certificate / unavailable engine capability
    // is not empty DAG state and must never be read as an objectively pruned row.
    {
        int custodyState = 2;
        std::string custodyErr;
        ReadCustodyReportImpl(dagLinksDir, &custodyState, &custodyErr);
        out->custodyState = custodyState;
        out->provenanceUnavailable = (custodyState != 0);
        out->custodyDetail = custodyErr;
    }

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