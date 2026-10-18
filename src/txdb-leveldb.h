// Copyright (c) 2009-2012 The Bitcoin Developers.
// Authored by Google, Inc.
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_LEVELDB_H
#define BITCOIN_LEVELDB_H

#include "main.h"
#include "ringsig.h"

#include <map>
#include <string>
#include <vector>

#include <leveldb/db.h>
#include <leveldb/write_batch.h>

// ---------------------------------------------------------------------------
// ARCHIVE-1 archive enumeration (Stage 3B-D1-S0-F1) — read-only, internal.
//
// The reset-path guard has to answer one question truthfully: "is the set of block-archive files
// still the set that was there before the index was reset?". That needs an enumeration which
// (a) cannot mistake a near-miss name for a block file, (b) classifies symlinks and other
// non-regular entries explicitly instead of following them blindly, and (c) fails CLOSED when the
// directory cannot be read at all: an unverifiable archive is never reported as a verified one.
//
// Declared here (rather than static in the .cpp) only so the regression suite can exercise the
// classification directly on synthetic directories; production reaches it through init_blockindex.
// ---------------------------------------------------------------------------
enum ArchiveScanStatus {
    ARCHIVE_SCAN_OK = 0,           // every archive-named entry is a usable (regular-file) payload
    ARCHIVE_SCAN_DATADIR_INVALID,  // datadir missing, not a directory, or not examinable at all
    ARCHIVE_SCAN_UNREADABLE,       // datadir exists but could not be enumerated (permissions / I/O)
    ARCHIVE_SCAN_ENTRY_BAD         // an archive-named entry is not a usable payload (non-regular
                                   // file, dangling symlink, or its type could not be determined)
};

// One archive enumeration. `payloads` = the archive-named entries that ARE usable payloads (regular
// files, or symlinks resolving to a regular file); these are what a reset must leave untouched.
// `notes` = explicit classification lines for entries accepted with a caveat; `detail` explains a
// non-OK status.
struct ArchiveScan {
    ArchiveScanStatus status;
    std::string detail;
    std::vector<std::string> payloads;
    std::vector<std::string> notes;
};

// Name grammar of a block-archive file: "blk" + 4..10 decimal digits + ".dat" (the writer emits
// strprintf("blk%04u.dat", nFile) with an unsigned int nFile, so 4 digits is the minimum and 10 the
// maximum). Everything else — near-misses such as "blk1.dat", "blk0001.DAT", "blk0001.dat.bak",
// "xblk0001.dat", "blk 0001.dat" — is NOT archive data and must never be counted as a block file.
bool IsBlockArchiveName(const std::string& name);

// Read-only enumeration of the block archive in `dir`. Never creates, renames, truncates or removes
// anything, and never reads block payloads (the cost is bounded by the NUMBER of entries, never by
// their size — a 2 GB blk file costs one directory entry).
ArchiveScan ScanBlockArchive(const boost::filesystem::path& dir);

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
// F2-B1-R (erase lifecycle): WHY a canonical row was erased. The prune's
// erase is the only origin whose semantics allow Option-R boundary reconstruction,
// so every erase call site states its origin EXPLICITLY and the origin is bound
// durably in the SAME atomic batch as the erase. A
// height floor alone can only prove that SOME prune passed a height; it cannot
// attribute a PARTICULAR row's absence to that prune.

// R3 / C6 section 1 — typed row read outcome. A bare absence NEVER proves a
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

#ifndef INNOVA_SYNC_CHECKPOINT_LOAD_STATUS_DEFINED
#define INNOVA_SYNC_CHECKPOINT_LOAD_STATUS_DEFINED
// V2-R2B (FIX A): typed result of reading the persisted synchronized checkpoint.
// ABSENT and CORRUPT are deliberately distinct: an absent key is the canonical
// ppcoin "no synchronized checkpoint" state (hashSyncCheckpoint stays 0), while a
// present-but-undecodable value is a durable authority inconsistency and must
// fail closed at startup.
enum SyncCheckpointLoadStatus
{
    SYNC_CHECKPOINT_ABSENT  = 0,
    SYNC_CHECKPOINT_LOADED  = 1,
    SYNC_CHECKPOINT_CORRUPT = 2,
};
#endif

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

bool ReadAddrIndex(uint160 addrHash, std::vector<uint256>& txHashes);
    bool WriteAddrIndex(uint160 addrHash, uint256 txHash);
    bool ReadTxIndex(uint256 hash, CTxIndex& txindex);
    bool ContainsTx(uint256 hash);
    bool ReadDiskTx(uint256 hash, CTransaction& tx, CTxIndex& txindex);
    bool ReadDiskTx(uint256 hash, CTransaction& tx);
    bool ReadDiskTx(COutPoint outpoint, CTransaction& tx, CTxIndex& txindex);
    bool ReadDiskTx(COutPoint outpoint, CTransaction& tx);
    bool ReadHashBestChain(uint256& hashBestChain);
    bool WriteHashBestChain(uint256 hashBestChain);
    bool ReadBestInvalidTrust(CBigNum& bnBestInvalidTrust);
    bool WriteBestInvalidTrust(CBigNum bnBestInvalidTrust);
    bool ReadInvalidBlockSet(std::set<uint256>& setInvalidBlockHash);
    bool WriteInvalidBlockSet(const std::set<uint256>& setInvalidBlockHash);
    bool ReadCandidateTips(std::map<uint256, CandidateTipRecord>& tips);
    bool WriteCandidateTips(const std::map<uint256, CandidateTipRecord>& tips);
    bool ReadSyncCheckpoint(uint256& hashCheckpoint);
    bool WriteSyncCheckpoint(uint256 hashCheckpoint);
    // V2-R2B (FIX A): typed persisted sync-checkpoint read that distinguishes a
    // key that is ABSENT (no synchronized checkpoint was ever established on
    // this datadir -> the canonical ppcoin "none" value 0) from a key that is
    // PRESENT but not decodable (corrupt -> the caller must fail closed).
    // Read-only; ReadSyncCheckpoint() semantics are unchanged.
    // NOTE: no genesis substitution is performed here.
    SyncCheckpointLoadStatus LoadSyncCheckpoint(uint256& hashCheckpoint);
    bool ReadCheckpointPubKey(std::string& strPubKey);
    bool WriteCheckpointPubKey(const std::string& strPubKey);
    bool LoadBlockIndex();


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

private:
    bool LoadBlockIndexGuts();
};

void InitIBDBatching();
void FlushIBDBatch();

#endif // BITCOIN_DB_H
