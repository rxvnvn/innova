// Copyright (c) 2009-2012 The Bitcoin Developers.
// Authored by Google, Inc.
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_LEVELDB_H
#define BITCOIN_LEVELDB_H

#include "main.h"
#include "epoch_state.h"
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

// R3 / C6 section 1 — typed DAG-row read outcome. A bare absence NEVER proves a
// protocol prune; a malformed present row is never "missing"; an I/O failure is
// never "missing". Only positive, incarnation-bound and continuity-bound
// evidence may produce ROW_OBJECTIVELY_PRUNED.

// Sentinel for values that are not known at an erase site: a PRUNE erase that
// cannot bind height/floor records NO event (fail closed), never a partial one.

// R3 / C6 section 3 — one append-only prune-event journal entry. `E` (the key) is
// a monotone, never-reused event identity; entries are hash-chained through
// `prev_event_hash`. Bound to exactly one row incarnation of exactly one vertex.

// R3.9 case M — test-only: simulate a backend that does not provide the read-only
// last-sequence accessor. This can only make the capability unavailable, never the
// reverse, so it strengthens fail-closed behaviour and never weakens it.

// R3 / C6 section 5/6 — custody seal. The published watermark W = P + C of the
// supported session that sealed this epoch, with the epoch it belongs to.

// R3 / C6 section 6 — the coverage certificate. It positively binds the certified
// provenance domain, and every digest in it is BOTH (a) maintained incrementally by
// the supported writer in the same batch as the mutation it describes and (b)
// independently recomputable by a full scan of the committed store, so "the
// certificate verifies" is a real recomputation, never a timestamp, a floor
// advancement or a final-state digest alone.

// Session custody state. UNAVAILABLE = no cross-session custody continuity could be
// established (no certificate, or the engine capability is missing) -> coverage does
// not survive a session boundary, and never a false positive. SUSPENDED = continuity
// was established once but the engine watermark no longer matches (an unsupported or
// foreign writer intervened) -> all prior prune evidence is inadmissible.

// Result of a coverage-certificate verification (R3.6). NO_CERTIFICATE and
// STORAGE_ERROR are fail-closed results, never "covered".

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
    // Erases the canonical row of `hash` and records, in the same batch, whether
    // this erase is prune-attributed (PRUNE) or a non-prune de-materialization
    // (REORGANIZE / FAILED_ADD_CLEANUP) in the row-erase provenance record. A
    // no-op erase (row already absent) records NOTHING: it materializes no
    // absence, so it must never (re)attribute one.
    // (declaration of EraseDAGLinks, see below for the R3 height-carrying overload)
    // R3 / C6 section 3: the prune-event journal entry binds height(X) and the
    // post-prune floor, neither of which is carried by the row payload
    // (the legacy daglinks row has no height field) and both of which are ALREADY known at
    // the prune site (dag.cpp resolves height by value at :1619 and owns
    // nPruneBelow). This overload lets that caller pass them; the erase semantics
    // are otherwise identical. With DAGROW_HEIGHT_UNKNOWN for either value a PRUNE
    // erase records NO event (fail closed: no positive attribution without a
    // complete binding). Non-prune origins never record an event.
    // S3 staged-view enumeration: return the staged daglinks writes/tombstones
    // held in the active WriteBatch (daglinks prefix only). Fail closed on any
    // batch scan error or when no active transaction is open.
    // Canonical bounded reverse-edge projection: number of retained child
    // records whose canonical vDAGParents contains this parent exactly once.
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
    // Typed form of the predicate above (B-1). IsDAGScoreAuthorityHealthy()
    // collapses every non-healthy state into `false`; an absent certificate
    // marker is NOT the same fact as a revoked/corrupt/mismatched one. The
    // accepted S3/S5 prune-reorg lifecycle requires that distinction: a scored
    // set that is merely NOT CERTIFIED IN THIS SESSION must not be reported as
    // an unhealthy authority. This accessor is strictly a refinement: the
    // healthy/not-healthy verdict and the error strings are unchanged.
    // Atomic production publish: write the supported-version certificate marker
    // bound to the CURRENT SourceStateId AND clear the revocation poison in one
    // durable LevelDB batch. Callers must first complete the authoritative
    // recolor (retired) at a stable source; this method only seals it.
    // Test-only: persistently revoke the score authority certificate against the
    // shared live source handle (simulates detection at a chosen point).
    // Test-only: persistently revoke the child-count projection against the
    // shared live source handle (simulates detection at a chosen point). Not
    // called by production paths.
    // Test-only: stage a raw daglinks write/erase directly into the active
    // WriteBatch (no child-count maintenance), matching the serialized daglinks
    // records ScanBatchDAGLinks observes. Used by the S3 staged-readback tests to
    // exercise pure batch-view precedence. Requires an open transaction.
    // Lenient legacy enumeration (skips malformed records) for legacy/recovery
    // callers. Do NOT use for authoritative source construction.
    // STRICT persisted daglinks enumeration for authoritative source construction.
    // Unlike IterateDAGLinks this does NOT skip malformed records: a malformed
    // key, malformed value, trailing bytes, or an iterator/read error returns
    // false and reports the reason, so an incomplete/malformed retained canvas
    // can never be silently certified (fail-closed authority).
    // S3 batch-only: stage the DAG-score certificate marker bound to `source`
    // (SCORE_STATE_KEY = {version 1, source}) and clear the score revocation key,
    // all INSIDE the active WriteBatch. Requires an open transaction. This is the
    // in-batch analog of the standalone quiesced-only PublishDAGScoreCertificateAtomic.
    // S3 batch-only: stage the child-count certificate marker bound to `source`
    // and clear the child-count revocation key, all INSIDE the active WriteBatch.
    // S3 rollback support: capture the DURABLE pre-operation score-certificate
    // state (raw marker bytes + presence, revocation poison presence) so a failed
    // mutation can restore it EXACTLY. Reads the durable store directly; call
    // BEFORE the mutation batch opens. A rollback must return to THIS state and
    // must never fabricate a healthy certificate.
    // S3 rollback support: stage the EXACT captured score-certificate state into
    // the active WriteBatch (marker write-or-erase + revocation set-or-clear).
    // Unlike StageDAGScoreCertificateInBatch this never fabricates a healthy
    // marker: an uncertified or revoked pre-operation state stays uncertified or
    // revoked after the rollback. Requires an open transaction.
    // Production CSPRNG mint with explicit failure status; bootstrap/source
    // mutation callers must not use a token when this returns false.
    // Zero-delta legacy upgrade: creates a token only when the key is absent.
    // A present-but-undecodable token is corruption, never treated as missing.

    // IDAG Phase 3: Epoch state persistence
    bool WriteEpochState(int nEpoch, const CEpochState& state);
    bool IterateEpochStates(std::map<int, CEpochState>& mapOut);
    bool IterateCurveTreeEpochs(std::map<int, CCurveTree>& mapOut);

    // F2 erase provenance. `dagcleanheight` above is written by THREE sites with
    // TWO meanings (the prune path stores the erase floor, Shutdown stores the
    // current tip, and the prune rollback restores a prior value), so it cannot
    // prove that a missing canonical daglinks row was erased by the accepted
    // prune lifecycle. This marker has exactly ONE writer - the prune/erase
    // lifecycle (the retired prune path) - and stores exactly the line
    // (nHeight - DAG_PRUNE_DEPTH) below which that same atomic commit erased the
    // persisted vertices. It is consumed only as ERASE provenance.

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

    // ---- R3 / C6 provenance state (additive durable state; C6 sections 2-4) ----
    // Typed row read (C6 section 1): distinguishes present-valid / absent /
    // malformed / I/O instead of collapsing everything into one bool. Malformed
    // and I/O failure are NEVER reported as absence.

    // Per-row incarnation (C6 section 2): durable, monotone, advanced by the
    // SUPPORTED writer inside the same batch as the row write, never derived from
    // the row's bytes and never inferred from absence. Supersession witness only:
    // it carries no consensus meaning.

    // Positive prune evidence (C6 section 3), written in the SAME batch as the
    // erase it describes: an append-only hash-chained journal entry plus a
    // per-row index. The per-row index also carries the row's event history so
    // supersession on restore is bounded and exact (additive field; the frozen
    // predicate consumes only {latest_event, latest_incarnation}).

    // Provenance mutation counter and custody epoch (C6 sections 3/5/6): monotone
    // custody state advanced only by supported provenance-relevant transitions.

    // ---- R3 / C6 sections 5-6 — coverage certificate + custody seal ----
    // Certification scan + publication (R3.6): recomputes EVERY digest over the
    // committed store (never a timestamp, floor advancement or final-state digest
    // alone), establishes the custody epoch on first certification, and publishes
    // the certificate in its own synchronous batch. A store without a floor and
    // without a clean height has no certifiable domain -> fails closed.
    // Full-scan verification of the stored certificate against the committed store
    // (the certification scan's counterpart). An unexplained legacy hole changes the
    // recomputed digests, so an uncertifiable legacy store stays uncertified.
    // O(1) binding check used on the consensus path: the durable accumulators, the
    // journal head/length and the mutation counter must all equal the certificate's.
    // R3.7 seal: ONE synchronous batch whose only record is the seal itself, so
    // C == 1 exactly and W = P + C is committed by that same record. After the
    // single commit, actual LastSequence() == W must hold exactly; a mismatch
    // suspends coverage and is never repaired by a second write.
    // Open-time continuity: current LastSequence() == sealed W. Missing capability,
    // missing seal or mismatch => UNAVAILABLE/SUSPENDED, never "covered".

    // R3.6 scan: recompute the certified-domain digests from the COMMITTED store.
    // This is the certification scan's primitive; it never consults timestamps,
    // first-write times, floor advancement or a final-state digest alone.
    // enforceAdmission: run the coverage-certification ADMISSION check (an unexplained
    // known-vertex absence refuses). Certification passes true; the verification path passes
    // false because it independently compares every digest against the published certificate.
    // prospectiveEpoch: the custody epoch this certification transition is about to establish
    // (curEpoch==0 ? 1 : curEpoch). Admission validates any absent-row prune event against it, so
    // an event that will not be admissible after certification refuses certification.
    // R3.6 incremental republication: apply one supported provenance mutation to the
    // published certificate and re-publish it IN THE SAME BATCH (a no-op when no
    // certificate exists yet, i.e. an uncertified store never gets a certificate
    // fabricated by a mutation).
    // R3.6: a floor/clean-height change re-binds the floor-state digest and re-publishes
    // the certificate in the same batch (no-op on an uncertified store).
    // Shared tail of the two republication paths: re-bind floor/hCert/counter/journal
    // from the durable state and re-publish the certificate in the ACTIVE batch.

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
