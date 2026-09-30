// Copyright (c) 2009-2012 The Bitcoin Developers.
// Authored by Google, Inc.
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_LEVELDB_H
#define BITCOIN_LEVELDB_H

#include "main.h"
#include "dag.h"
#include "finality.h"
#include "ringsig.h"
#include "curvetree.h"

#include <map>
#include <string>
#include <vector>

#include <leveldb/db.h>
#include <leveldb/write_batch.h>

// Class that provides access to a LevelDB. Note that this class is frequently
// instantiated on the stack and then destroyed again, so instantiation has to
// be very cheap. Unfortunately that means, a CTxDB instance is actually just a
// wrapper around some global state.
//
// A LevelDB is a key/value store that is optimized for fast usage on hard
// disks. It prefers long read/writes to seeks and is based on a series of
// sorted key/value mapping files that are stacked on top of each other, with
// newer files overriding older files. A background thread compacts them
// together when too many files stack up.
//
// Learn more: http://code.google.com/p/leveldb/
// F2-B1-R (erase lifecycle): WHY a canonical daglinks row was erased. The prune's
// erase is the only origin whose semantics allow Option-R boundary reconstruction,
// so every erase call site states its origin EXPLICITLY and the origin is bound
// durably in the SAME atomic batch as the erase (see CTxDB::EraseDAGLinks). A
// height floor alone can only prove that SOME prune passed a height; it cannot
// attribute a PARTICULAR row's absence to that prune.
enum class DAGRowEraseOrigin
{
    PRUNE = 0,               // CDAGManager::PruneDAGData (floor owner; Option-R legal)
    REORGANIZE = 1,          // Reorganize disconnect erase (no prune provenance)
    FAILED_ADD_CLEANUP = 2   // AddToBlockIndex SetBestChain-failure cleanup erase
};

// R3 / C6 section 1 — typed DAG-row read outcome. A bare absence NEVER proves a
// protocol prune; a malformed present row is never "missing"; an I/O failure is
// never "missing". Only positive, incarnation-bound and continuity-bound
// evidence may produce ROW_OBJECTIVELY_PRUNED.
enum class DAGRowTypedOutcome
{
    ROW_PRESENT_VALID = 0,       // row present and typed-valid
    ROW_OBJECTIVELY_PRUNED = 1,  // row absent WITH positive bound prune evidence
    ROW_MISSING_UNEXPLAINED = 2, // row absent, no admissible positive evidence (fail closed)
    ROW_CORRUPT = 3,             // present row unreadable/malformed (never missing)
    STORAGE_ERROR = 4            // I/O or iterator-status failure (never missing/pruned)
};

// Sentinel for values that are not known at an erase site: a PRUNE erase that
// cannot bind height/floor records NO event (fail closed), never a partial one.
static const int32_t DAGROW_HEIGHT_UNKNOWN = INT32_MIN;

// R3 / C6 section 3 — one append-only prune-event journal entry. `E` (the key) is
// a monotone, never-reused event identity; entries are hash-chained through
// `prev_event_hash`. Bound to exactly one row incarnation of exactly one vertex.
struct DAGPruneEvent
{
    uint256 hash;            // X
    uint64_t incarnation;    // N (dagrowinc[X] at prune time)
    uint64_t epoch;          // custody epoch id in force at prune time
    int32_t height;          // height(X)
    int32_t floor_after;     // prune floor after this prune
    uint256 prev_event_hash; // hash-chain link to the previous journal entry (0 for the first)
    uint64_t superseded_by;  // 0 = still admissible; else the incarnation that superseded it
};

// R3.9 case M — test-only: simulate a backend that does not provide the read-only
// last-sequence accessor. This can only make the capability unavailable, never the
// reverse, so it strengthens fail-closed behaviour and never weakens it.
extern bool g_testSuppressDagCustodyWatermark;

// R3 / C6 section 5/6 — custody seal. The published watermark W = P + C of the
// supported session that sealed this epoch, with the epoch it belongs to.
struct DAGCustodySeal
{
    uint64_t watermark;   // W = pre-commit LastSequence + exact record count of that final batch
    uint64_t epoch;       // custody epoch id sealed
    uint64_t counter;     // provenance mutation counter at seal time
};

// R3 / C6 section 6 — the coverage certificate. It positively binds the certified
// provenance domain, and every digest in it is BOTH (a) maintained incrementally by
// the supported writer in the same batch as the mutation it describes and (b)
// independently recomputable by a full scan of the committed store, so "the
// certificate verifies" is a real recomputation, never a timestamp, a floor
// advancement or a final-state digest alone.
struct DAGProvenanceCertificate
{
    uint32_t version;             // certificate layout version
    uint32_t capabilityVersion;   // provenance capability version
    uint256  storeInstanceId;     // durable store identity (never regenerated silently)
    uint64_t epoch;               // custody epoch this certificate certifies (>0 when admissible)
    int32_t  hCert;               // certified domain = [FORK_HEIGHT_DAG, hCert)
    uint64_t watermark;           // engine watermark binding this certificate to a custody session
    uint64_t mutationCounter;     // provenance mutation counter at publish time
    uint64_t coveredVertexCount;  // |vertices with a durable incarnation record|
    uint256  coveredRowDigest;    // XOR-set digest over covered daglinks rows (hash + payload bytes)
    uint256  perRowIncarnationDigest; // XOR-set digest over (hash, incarnation)
    uint256  knownVertexDigest;   // XOR-set digest over the provenance-known vertex set
    uint256  floorStateDigest;    // digest over (prune floor, clean height)
    uint256  journalHeadHash;     // prune-journal head digest at publish time
    uint64_t journalLength;       // prune-journal record count at publish time
    int32_t  floorValue;          // advisory copy of the floor-state inputs
    int32_t  cleanHeightValue;    // advisory copy of the floor-state inputs
};

// Session custody state. UNAVAILABLE = no cross-session custody continuity could be
// established (no certificate, or the engine capability is missing) -> coverage does
// not survive a session boundary, and never a false positive. SUSPENDED = continuity
// was established once but the engine watermark no longer matches (an unsupported or
// foreign writer intervened) -> all prior prune evidence is inadmissible.
enum class DAGCustodyState
{
    UNAVAILABLE = 0,
    SUSPENDED = 1,
    VERIFIED = 2
};

// Result of a coverage-certificate verification (R3.6). NO_CERTIFICATE and
// STORAGE_ERROR are fail-closed results, never "covered".
enum class DAGCertVerifyResult
{
    OK = 0,
    NO_CERTIFICATE = 1,
    STORE_INSTANCE_MISMATCH = 2,
    EPOCH_MISMATCH = 3,
    DIGEST_MISMATCH = 4,
    JOURNAL_MISMATCH = 5,
    STORAGE_ERROR = 6
};

class CTxDB
{
public:
    CTxDB(const char* pszMode="r+");
    ~CTxDB() {
        // Note that this is not the same as Close() because it deletes only
        // data scoped to this TxDB object.
        if (activeBatch)
            TxnAbort();
    }

    // Destroys the underlying shared global state accessed by this TxDB.
    void Close();

private:
    leveldb::DB *pdb;  // Points to the global instance.

    // A batch stores up writes and deletes for atomic application. When this
    // field is non-NULL, writes/deletes go there instead of directly to disk.
    leveldb::WriteBatch *activeBatch;
    leveldb::Options options;
    bool fReadOnly;
    int nVersion;

protected:
    // Returns true and sets (value,false) if activeBatch contains the given key
    // or leaves value alone and sets deleted = true if activeBatch contains a
    // delete for it.
    bool ScanBatch(const CDataStream &key, std::string *value, bool *deleted) const;

    template<typename K, typename T>
    bool Read(const K& key, T& value)
    {
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey.reserve(1000);
        ssKey << key;
        std::string strValue;

        bool readFromDb = true;
        if (activeBatch) {
            // First we must search for it in the currently pending set of
            // changes to the db. If not found in the batch, go on to read disk.
            bool deleted = false;
            readFromDb = ScanBatch(ssKey, &strValue, &deleted) == false;
            if (deleted) {
                return false;
            }
        }
        if (readFromDb) {
            leveldb::Status status = pdb->Get(leveldb::ReadOptions(),
                                              ssKey.str(), &strValue);
            if (!status.ok()) {
                if (status.IsNotFound())
                    return false;
                // Some unexpected error.
                printf("LevelDB read failure: %s\n", status.ToString().c_str());
                return false;
            }
        }
        // Unserialize value
        try {
            CDataStream ssValue(strValue.data(), strValue.data() + strValue.size(),
                                SER_DISK, CLIENT_VERSION);
            ssValue >> value;
        }
        catch (std::exception &e) {
            printf("LevelDB deserialization failure: %s (value size=%zu)\n",
                   e.what(), strValue.size());
            return false;
        }
        return true;
    }

    template<typename K, typename T>
    bool Write(const K& key, const T& value)
    {
        if (fReadOnly)
        {
            printf("ERROR: Write called on database in read-only mode\n");
            return false;
        }

        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey.reserve(1000);
        ssKey << key;
        CDataStream ssValue(SER_DISK, CLIENT_VERSION);
        ssValue.reserve(10000);
        ssValue << value;

        if (activeBatch) {
            activeBatch->Put(ssKey.str(), ssValue.str());
            return true;
        }
        leveldb::Status status = pdb->Put(leveldb::WriteOptions(), ssKey.str(), ssValue.str());
        if (!status.ok()) {
            printf("LevelDB write failure: %s\n", status.ToString().c_str());
            return false;
        }
        return true;
    }

    template<typename K>
    bool Erase(const K& key)
    {
        if (!pdb)
            return false;
        if (fReadOnly)
        {
            printf("ERROR: Erase called on database in read-only mode\n");
            return false;
        }

        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey.reserve(1000);
        ssKey << key;
        if (activeBatch) {
            activeBatch->Delete(ssKey.str());
            return true;
        }
        leveldb::Status status = pdb->Delete(leveldb::WriteOptions(), ssKey.str());
        return (status.ok() || status.IsNotFound());
    }

    template<typename K>
    bool Exists(const K& key)
    {
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey.reserve(1000);
        ssKey << key;
        std::string unused;

        if (activeBatch) {
            bool deleted;
            if (ScanBatch(ssKey, &unused, &deleted) && !deleted) {
                return true;
            }
        }


        leveldb::Status status = pdb->Get(leveldb::ReadOptions(), ssKey.str(), &unused);
        return status.IsNotFound() == false;
    }


public:
    bool TxnBegin();
    bool TxnCommit();
    bool TxnAbort();

    // True when a LevelDB WriteBatch is open on this CTxDB handle (i.e. an
    // active source transaction). Batch-only authoritative helpers must reject
    // calls without an open batch BEFORE any write, so a "no active
    // transaction" failure can never leave a durable mutation behind.
    bool HasActiveBatch() const { return activeBatch != NULL; }

    leveldb::DB* GetInstance()
    {
        return pdb;
    }

    bool ReadVersion(int& nVersion)
    {
        nVersion = 0;
        return Read(std::string("version"), nVersion);
    }

    bool WriteVersion(int nVersion)
    {
        return Write(std::string("version"), nVersion);
    }

    bool WriteKeyImage(ec_point& keyImage, CKeyImageSpent& keyImageSpent);
    bool ReadKeyImage(ec_point& keyImage, CKeyImageSpent& keyImageSpent);
    bool EraseKeyImage(ec_point& keyImage);

    bool WriteAnonOutput(CPubKey& pkCoin, CAnonOutput& ao);
    bool ReadAnonOutput(CPubKey& pkCoin, CAnonOutput& ao);
    bool EraseAnonOutput(CPubKey& pkCoin);

    bool WriteShieldedNullifier(const uint256& nullifier, const CShieldedNullifierSpent& nfs);
    bool ReadShieldedNullifier(const uint256& nullifier, CShieldedNullifierSpent& nfs);
    bool EraseShieldedNullifier(const uint256& nullifier);

    bool WriteShieldedAnchor(const uint256& anchor);
    bool ReadShieldedAnchor(const uint256& anchor);
    bool EraseShieldedAnchor(const uint256& anchor);
    bool WriteShieldedAnchorHeight(const uint256& anchor, int nHeight);
    bool ReadShieldedAnchorHeight(const uint256& anchor, int& nHeight);

    bool WriteShieldedTree(const CIncrementalMerkleTree& tree);
    bool ReadShieldedTree(CIncrementalMerkleTree& tree);

    bool WriteShieldedTreeAtBlock(const uint256& blockHash, const CIncrementalMerkleTree& tree);
    bool ReadShieldedTreeAtBlock(const uint256& blockHash, CIncrementalMerkleTree& tree);

    bool WriteShieldedPoolValue(int64_t nValue);
    bool ReadShieldedPoolValue(int64_t& nValue);

    bool WriteShieldedCommitment(uint64_t nIndex, const CPedersenCommitment& commit);
    bool ReadShieldedCommitment(uint64_t nIndex, CPedersenCommitment& commit);
    bool ReadAllShieldedCommitments(std::vector<CPedersenCommitment>& vCommitments);
    bool ReadShieldedCommitmentCount(uint64_t& nCount);
    bool WriteShieldedCommitmentCount(uint64_t nCount);

    bool WriteShieldedCommitmentHeight(uint64_t nIndex, int nHeight);
    bool ReadShieldedCommitmentHeight(uint64_t nIndex, int& nHeight);
    bool WriteShieldedCommitmentIndex(const std::vector<unsigned char>& vchCommitment, uint64_t nIndex);
    bool ReadShieldedCommitmentIndex(const std::vector<unsigned char>& vchCommitment, uint64_t& nIndex);

    bool WriteCurveTree(const CCurveTree& tree);
    bool ReadCurveTree(CCurveTree& tree);
    bool WriteCurveTreeAtBlock(const uint256& blockHash, const CCurveTree& tree);
    bool ReadCurveTreeAtBlock(const uint256& blockHash, CCurveTree& tree);
    bool EraseCurveTreeAtBlock(const uint256& blockHash);
    bool WriteCurveTreeAtEpoch(int nEpoch, const CCurveTree& tree);
    bool ReadCurveTreeAtEpoch(int nEpoch, CCurveTree& tree);

	bool ReadAddrIndex(uint160 addrHash, std::vector<uint256>& txHashes);
    bool WriteAddrIndex(uint160 addrHash, uint256 txHash);
    bool ReadTxIndex(uint256 hash, CTxIndex& txindex);
    bool UpdateTxIndex(uint256 hash, const CTxIndex& txindex);
    bool AddTxIndex(const CTransaction& tx, const CDiskTxPos& pos, int nHeight);
    bool EraseTxIndex(const CTransaction& tx);
    bool ContainsTx(uint256 hash);
    bool ReadDiskTx(uint256 hash, CTransaction& tx, CTxIndex& txindex);
    bool ReadDiskTx(uint256 hash, CTransaction& tx);
    bool ReadDiskTx(COutPoint outpoint, CTransaction& tx, CTxIndex& txindex);
    bool ReadDiskTx(COutPoint outpoint, CTransaction& tx);
    bool WriteBlockIndex(const CDiskBlockIndex& blockindex);
    bool EraseBlockIndex(const uint256& blockhash);
    bool ReadHashBestChain(uint256& hashBestChain);
    bool WriteHashBestChain(uint256 hashBestChain);
    bool ReadHashBestHeaderChain(uint256& hashBestChain);
    bool WriteHashBestHeaderChain(uint256 hashBestChain);
    bool ReadBestInvalidTrust(CBigNum& bnBestInvalidTrust);
    bool WriteBestInvalidTrust(CBigNum bnBestInvalidTrust);
    bool ReadInvalidBlockSet(std::set<uint256>& setInvalidBlockHash);
    bool WriteInvalidBlockSet(const std::set<uint256>& setInvalidBlockHash);
    bool ReadCandidateTips(std::map<uint256, CandidateTipRecord>& tips);
    bool WriteCandidateTips(const std::map<uint256, CandidateTipRecord>& tips);
    bool ReadSyncCheckpoint(uint256& hashCheckpoint);
    bool WriteSyncCheckpoint(uint256 hashCheckpoint);
    bool ReadCheckpointPubKey(std::string& strPubKey);
    bool WriteCheckpointPubKey(const std::string& strPubKey);
    bool LoadBlockIndex();

    // IDAG Phase 2: DAG link persistence. SourceStateId names the exact
    // logical daglinks state and must be queued in the SAME TxnBegin/Commit
    // batch as every mutation of this relation.
    bool ReadDAGLinks(const uint256& hash, CBlockDAGData& data);
    bool ReadDAGFrontierMembership(const uint256& hash, bool* member);
    // G6 authoritative row-level attestation. Identical membership contract to
    // ReadDAGFrontierMembership (row present AND childCount == 0), but it ALSO
    // attests canonical ROW PRESENCE SEPARATELY, so an authoritative consumer
    // can distinguish "legitimately not a frontier member" (row present, has
    // children / durable row absent by semantics) from "an enumerated
    // authoritative frontier tip whose canonical row is MISSING" (incomplete
    // canonical source - must fail closed, never a reduced valid vector).
    // Returns false fail-closed on any IO error, malformed/unreadable canonical
    // row, or revoked/unreadable child-count projection. On true:
    //   *rowPresent = canonical daglinks postimage exists (active-batch staged
    //                 tombstone honoured exactly like ReadDAGFrontierMembership);
    //   *member     = *rowPresent && childCount == 0.
    // Read-only attestation: never writes, never revokes, never mutates the
    // child-count or score certificates. The legacy reader above is unchanged.
    bool ReadDAGFrontierMembershipAttested(const uint256& hash, bool* member, bool* rowPresent);
    bool WriteDAGLinks(const uint256& hash, const CBlockDAGData& data);
    // Erases the canonical row of `hash` and records, in the same batch, whether
    // this erase is prune-attributed (PRUNE) or a non-prune de-materialization
    // (REORGANIZE / FAILED_ADD_CLEANUP) in the row-erase provenance record. A
    // no-op erase (row already absent) records NOTHING: it materializes no
    // absence, so it must never (re)attribute one.
    // (declaration of EraseDAGLinks, see below for the R3 height-carrying overload)
    bool EraseDAGLinks(const uint256& hash, DAGRowEraseOrigin origin);
    // R3 / C6 section 3: the prune-event journal entry binds height(X) and the
    // post-prune floor, neither of which is carried by the row payload
    // (CBlockDAGData has no height field) and both of which are ALREADY known at
    // the prune site (dag.cpp resolves height by value at :1619 and owns
    // nPruneBelow). This overload lets that caller pass them; the erase semantics
    // are otherwise identical. With DAGROW_HEIGHT_UNKNOWN for either value a PRUNE
    // erase records NO event (fail closed: no positive attribution without a
    // complete binding). Non-prune origins never record an event.
    bool EraseDAGLinks(const uint256& hash, DAGRowEraseOrigin origin, int32_t rowHeight,
                       int32_t pruneFloorAfter);
    // S3 staged-view enumeration: return the staged daglinks writes/tombstones
    // held in the active WriteBatch (daglinks prefix only). Fail closed on any
    // batch scan error or when no active transaction is open.
    bool ScanBatchDAGLinks(std::map<uint256, CBlockDAGData>* stagedWrites,
                           std::set<uint256>* stagedTombstones,
                           bool* activeBatchOpen, std::string* error);
    // Canonical bounded reverse-edge projection: number of retained child
    // records whose canonical vDAGParents contains this parent exactly once.
    bool ReadDAGChildCount(const uint256& parent, uint64_t* count, bool* present);
    bool IsDAGChildCountIndexHealthy(std::string* error);
    bool EnsureDAGChildCountIndex(std::string* error);
    // R2c.2s: authoritative mutable DAG score authority. The persisted
    // nDAGScore/fBlue/nInferredK fields are freshly derived from CURRENT ancestor
    // coloring, not frozen at first coloring, so a separate durable certificate
    // is required to bind the retained canonical score set to one SourceStateId.
    // A pre-cutover or stale score set must never be accepted as certified-current.
    //
    // Healthy predicate (this layer only): supported marker version, marker
    // bound to the CURRENT SourceStateId, no revocation poison, child-count
    // authority also healthy. It never claims that a recolor was performed;
    // that is EnsureDAGScoreAuthority's contract.
    bool IsDAGScoreAuthorityHealthy(std::string* error);
    // Typed form of the predicate above (B-1). IsDAGScoreAuthorityHealthy()
    // collapses every non-healthy state into `false`; an absent certificate
    // marker is NOT the same fact as a revoked/corrupt/mismatched one. The
    // accepted S3/S5 prune-reorg lifecycle requires that distinction: a scored
    // set that is merely NOT CERTIFIED IN THIS SESSION must not be reported as
    // an unhealthy authority. This accessor is strictly a refinement: the
    // healthy/not-healthy verdict and the error strings are unchanged.
    enum DAGScoreAuthorityStatus
    {
        DAG_SCORE_AUTHORITY_HEALTHY = 0,     // marker present, version 1, bound to the current source
        DAG_SCORE_AUTHORITY_UNCERTIFIED = 1, // no marker: never certified in this session
        DAG_SCORE_AUTHORITY_REVOKED = 2,     // revocation poison present
        DAG_SCORE_AUTHORITY_CORRUPT = 3,     // unreadable/corrupt/bad-version/token mismatch
        DAG_SCORE_AUTHORITY_UNAVAILABLE = 4  // no database handle / no source token / read error
    };
    DAGScoreAuthorityStatus GetDAGScoreAuthorityStatus(std::string* error);
    // Atomic production publish: write the supported-version certificate marker
    // bound to the CURRENT SourceStateId AND clear the revocation poison in one
    // durable LevelDB batch. Callers must first complete the authoritative
    // recolor (CDAGManager) at a stable source; this method only seals it.
    bool PublishDAGScoreCertificateAtomic(std::string* error);
    // Test-only: persistently revoke the score authority certificate against the
    // shared live source handle (simulates detection at a chosen point).
    bool RevokeDAGScoreAuthorityForTest();
    // Test-only: persistently revoke the child-count projection against the
    // shared live source handle (simulates detection at a chosen point). Not
    // called by production paths.
    bool RevokeDAGChildCountForTest();
    // Test-only: stage a raw daglinks write/erase directly into the active
    // WriteBatch (no child-count maintenance), matching the serialized daglinks
    // records ScanBatchDAGLinks observes. Used by the S3 staged-readback tests to
    // exercise pure batch-view precedence. Requires an open transaction.
    bool StageDAGLinkRawForTest(const uint256& hash, const CBlockDAGData& data, bool erase);
    // Lenient legacy enumeration (skips malformed records) for legacy/recovery
    // callers. Do NOT use for authoritative source construction.
    bool IterateDAGLinks(std::map<uint256, CBlockDAGData>& mapOut);
    // STRICT persisted daglinks enumeration for authoritative source construction.
    // Unlike IterateDAGLinks this does NOT skip malformed records: a malformed
    // key, malformed value, trailing bytes, or an iterator/read error returns
    // false and reports the reason, so an incomplete/malformed retained canvas
    // can never be silently certified (fail-closed authority).
    bool IterateDAGLinksStrict(std::map<uint256, CBlockDAGData>& mapOut, std::string* error);
    bool ReadDAGSourceStateId(uint256& out);
    bool HasDAGSourceStateId();
    bool WriteDAGSourceStateId(const uint256& id);
    // S3 batch-only: stage the DAG-score certificate marker bound to `source`
    // (SCORE_STATE_KEY = {version 1, source}) and clear the score revocation key,
    // all INSIDE the active WriteBatch. Requires an open transaction. This is the
    // in-batch analog of the standalone quiesced-only PublishDAGScoreCertificateAtomic.
    bool StageDAGScoreCertificateInBatch(const uint256& source, std::string* error);
    // S3 batch-only: stage the child-count certificate marker bound to `source`
    // and clear the child-count revocation key, all INSIDE the active WriteBatch.
    bool StageDAGChildCountCertificateInBatch(const uint256& source, std::string* error);
    // S3 rollback support: capture the DURABLE pre-operation score-certificate
    // state (raw marker bytes + presence, revocation poison presence) so a failed
    // mutation can restore it EXACTLY. Reads the durable store directly; call
    // BEFORE the mutation batch opens. A rollback must return to THIS state and
    // must never fabricate a healthy certificate.
    bool CaptureDAGScoreCertificateState(bool* markerPresent, std::string* markerRaw,
                                         bool* revoked, std::string* error);
    // S3 rollback support: stage the EXACT captured score-certificate state into
    // the active WriteBatch (marker write-or-erase + revocation set-or-clear).
    // Unlike StageDAGScoreCertificateInBatch this never fabricates a healthy
    // marker: an uncertified or revoked pre-operation state stays uncertified or
    // revoked after the rollback. Requires an open transaction.
    bool RestoreDAGScoreCertificateStateInBatch(bool markerPresent, const std::string& markerRaw,
                                                bool revoked, std::string* error);
    // Production CSPRNG mint with explicit failure status; bootstrap/source
    // mutation callers must not use a token when this returns false.
    bool MintDAGSourceStateId(uint256& out);
    // Zero-delta legacy upgrade: creates a token only when the key is absent.
    // A present-but-undecodable token is corruption, never treated as missing.
    bool BootstrapDAGSourceStateId(std::string* error);

    // IDAG Phase 3: Epoch state persistence
    bool WriteEpochState(int nEpoch, const CEpochState& state);
    bool IterateEpochStates(std::map<int, CEpochState>& mapOut);
    bool IterateCurveTreeEpochs(std::map<int, CCurveTree>& mapOut);
    bool WriteDAGCleanHeight(int nHeight);
    bool ReadDAGCleanHeight(int& nHeight);
    bool EraseDAGCleanHeight();

    // F2 erase provenance. `dagcleanheight` above is written by THREE sites with
    // TWO meanings (PruneDAGData stores the erase floor, Shutdown stores the
    // current tip, and the prune rollback restores a prior value), so it cannot
    // prove that a missing canonical daglinks row was erased by the accepted
    // prune lifecycle. This marker has exactly ONE writer - the prune/erase
    // lifecycle (CDAGManager::PruneDAGData) - and stores exactly the line
    // (nHeight - DAG_PRUNE_DEPTH) below which that same atomic commit erased the
    // persisted vertices. It is consumed only as ERASE provenance.
    bool WriteDAGPruneFloor(int nHeight);
    bool ReadDAGPruneFloor(int& nHeight);
    bool EraseDAGPruneFloor();

    // F2-B1-R row-erase provenance. ONE record per canonical daglinks row whose
    // absence was caused by a NON-prune erase lifecycle (Reorganize disconnect
    // erase, failed-ADD cleanup). `dagprunefloor` above proves only that the prune
    // later passed a height; this record proves that THIS row's absence was NOT
    // caused by the prune, so no floor advance can ever explain (launder) it.
    // Written/erased ONLY from inside CTxDB::EraseDAGLinks, in the same atomic
    // batch as the erase it describes: absence and provenance cannot commit
    // separately. Value = (int)DAGRowEraseOrigin.
    // Bounded: one record per rare non-prune de-materialization event - never per
    // pruned row, never per chain height. It is erased when the prune physically
    // erases that same row; a de-materialized row is invisible to the prune, so
    // the record survives exactly as long as its absence stays un-prune-attributed.
    bool WriteDAGRowErase(const uint256& hash, int origin);
    bool ReadDAGRowErase(const uint256& hash, int* origin, bool* present);
    bool EraseDAGRowErase(const uint256& hash);

    // R3 / C6 continuity substrate (READ-ONLY, additive). The storage engine's
    // monotone write-sequence watermark. Every durable provenance field that only
    // the NEW writer increments (capability version, provenance counter, store
    // identity, incarnation marker, prune journal) is untouched by a
    // non-participating writer, so final-state equality - including a digest over
    // all final-state bytes - cannot prove writer continuity. The one signal
    // every writer must necessarily perturb and no writer can rewind is the
    // engine's own last-sequence watermark, so provenance coverage is bound to it
    // (C6 section 5): sealed as `W` by the supported writer's closing batch and
    // verified by EXACT equality at supported open.
    //
    // Read-only: it changes no write semantics, no V2 record field, no generation
    // layout and adds no database authority. Returns false (fail closed) where a
    // backend cannot provide the accessor, in which case coverage must not
    // survive a session boundary (C6 section 5/14 fallback).
    bool ReadEngineLastSequence(uint64_t* sequence, std::string* detail) const;

    // ---- R3 / C6 provenance state (additive durable state; C6 sections 2-4) ----
    // Typed row read (C6 section 1): distinguishes present-valid / absent /
    // malformed / I/O instead of collapsing everything into one bool. Malformed
    // and I/O failure are NEVER reported as absence.
    bool ReadDAGLinksTyped(const uint256& hash, CBlockDAGData* data,
                           DAGRowTypedOutcome* outcome, std::string* detail);

    // Per-row incarnation (C6 section 2): durable, monotone, advanced by the
    // SUPPORTED writer inside the same batch as the row write, never derived from
    // the row's bytes and never inferred from absence. Supersession witness only:
    // it carries no consensus meaning.
    bool ReadDAGRowIncarnation(const uint256& hash, uint64_t* incarnation, bool* present);
    bool WriteDAGRowIncarnation(const uint256& hash, uint64_t incarnation);

    // Positive prune evidence (C6 section 3), written in the SAME batch as the
    // erase it describes: an append-only hash-chained journal entry plus a
    // per-row index. The per-row index also carries the row's event history so
    // supersession on restore is bounded and exact (additive field; the frozen
    // predicate consumes only {latest_event, latest_incarnation}).
    bool ReadDAGPruneLatest(const uint256& hash, uint64_t* event, uint64_t* incarnation,
                            std::vector<uint64_t>* events, bool* present);
    bool WriteDAGPruneLatest(const uint256& hash, uint64_t event, uint64_t incarnation,
                             const std::vector<uint64_t>& events);
    bool EraseDAGPruneLatest(const uint256& hash);
    bool ReadDAGPruneEvent(uint64_t event, DAGPruneEvent* out, bool* present);
    bool WriteDAGPruneEvent(uint64_t event, const DAGPruneEvent& ev);
    bool ReadDAGPruneJournal(uint64_t* head, uint64_t* length, uint256* head_hash);

    // Provenance mutation counter and custody epoch (C6 sections 3/5/6): monotone
    // custody state advanced only by supported provenance-relevant transitions.
    bool ReadDAGProvenanceCounter(uint64_t* counter);
    bool ReadDAGCustodyEpoch(uint64_t* epoch);

    // ---- R3 / C6 sections 5-6 — coverage certificate + custody seal ----
    bool ReadDAGProvenanceCertificate(DAGProvenanceCertificate* cert, bool* present);
    // Certification scan + publication (R3.6): recomputes EVERY digest over the
    // committed store (never a timestamp, floor advancement or final-state digest
    // alone), establishes the custody epoch on first certification, and publishes
    // the certificate in its own synchronous batch. A store without a floor and
    // without a clean height has no certifiable domain -> fails closed.
    bool CertifyDAGProvenanceCoverage(int32_t hCert, uint64_t* epoch, std::string* error);
    // Full-scan verification of the stored certificate against the committed store
    // (the certification scan's counterpart). An unexplained legacy hole changes the
    // recomputed digests, so an uncertifiable legacy store stays uncertified.
    bool VerifyDAGProvenanceCoverage(DAGCertVerifyResult* result, std::string* error);
    // O(1) binding check used on the consensus path: the durable accumulators, the
    // journal head/length and the mutation counter must all equal the certificate's.
    bool VerifyDAGProvenanceCoverageShallow(DAGCertVerifyResult* result, std::string* error);
    DAGCustodyState GetDAGCustodyState() const;
    bool IsDAGProvenanceDeepVerified() const;
    // R3.7 seal: ONE synchronous batch whose only record is the seal itself, so
    // C == 1 exactly and W = P + C is committed by that same record. After the
    // single commit, actual LastSequence() == W must hold exactly; a mismatch
    // suspends coverage and is never repaired by a second write.
    bool SealDAGCustody(uint64_t* sealedWatermark, std::string* error);
    bool ReadDAGCustodySeal(DAGCustodySeal* seal, bool* present);
    // Open-time continuity: current LastSequence() == sealed W. Missing capability,
    // missing seal or mismatch => UNAVAILABLE/SUSPENDED, never "covered".
    bool VerifyDAGCustodyAtOpen(std::string* error);

    // R3.6 scan: recompute the certified-domain digests from the COMMITTED store.
    // This is the certification scan's primitive; it never consults timestamps,
    // first-write times, floor advancement or a final-state digest alone.
    // enforceAdmission: run the coverage-certification ADMISSION check (an unexplained
    // known-vertex absence refuses). Certification passes true; the verification path passes
    // false because it independently compares every digest against the published certificate.
    // prospectiveEpoch: the custody epoch this certification transition is about to establish
    // (curEpoch==0 ? 1 : curEpoch). Admission validates any absent-row prune event against it, so
    // an event that will not be admissible after certification refuses certification.
    bool ScanDAGProvenanceDigests(DAGProvenanceCertificate* out, std::string* error, bool enforceAdmission = true,
                                  uint64_t prospectiveEpoch = 0);
    // R3.6 incremental republication: apply one supported provenance mutation to the
    // published certificate and re-publish it IN THE SAME BATCH (a no-op when no
    // certificate exists yet, i.e. an uncertified store never gets a certificate
    // fabricated by a mutation).
    bool RepublishDAGProvenanceCertificateOnWrite(const uint256& hash,
                                                 const std::string& newPayload,
                                                 const std::string& oldPayload,
                                                 bool fRowExisted, bool hadIncarnation,
                                                 uint64_t incBefore, uint64_t incAfter);
    bool RepublishDAGProvenanceCertificateOnErase(const uint256& hash,
                                                 const std::string& oldPayload);
    // R3.6: a floor/clean-height change re-binds the floor-state digest and re-publishes
    // the certificate in the same batch (no-op on an uncertified store).
    bool RepublishDAGProvenanceFloorBinding();
    // Shared tail of the two republication paths: re-bind floor/hCert/counter/journal
    // from the durable state and re-publish the certificate in the ACTIVE batch.
    bool RepublishDAGProvenanceCertificateCommon(DAGProvenanceCertificate* cert);

    // IDAG finality vote persistence
    bool WriteFinalityVote(const uint256& nullifier, const CFinalityVote& vote);
    bool ReadFinalityVote(const uint256& nullifier, CFinalityVote& vote);
    bool EraseFinalityVote(const uint256& nullifier);
    bool IterateFinalityVotes(std::map<uint256, CFinalityVote>& mapOut);
    bool WriteFinalityTallyShare(const uint256& hashShare, const CFinalityTallyShare& share);
    bool ReadFinalityTallyShare(const uint256& hashShare, CFinalityTallyShare& share);
    bool EraseFinalityTallyShare(const uint256& hashShare);
    bool IterateFinalityTallyShares(std::map<uint256, CFinalityTallyShare>& mapOut);
    bool WriteFinalityTallyCertificate(const uint256& hashCert, const CFinalityTallyCertificate& cert);
    bool ReadFinalityTallyCertificate(const uint256& hashCert, CFinalityTallyCertificate& cert);
    bool EraseFinalityTallyCertificate(const uint256& hashCert);
    bool IterateFinalityTallyCertificates(std::map<uint256, CFinalityTallyCertificate>& mapOut);
private:
    bool LoadBlockIndexGuts();
};

void InitIBDBatching();
void FlushIBDBatch();

#endif // BITCOIN_DB_H
