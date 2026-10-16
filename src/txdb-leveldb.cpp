// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.

#include <algorithm>
#include <atomic>
#include <map>
#include <set>
#include <stdexcept>
#include <stdint.h>

#include <boost/version.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#include <leveldb/env.h>
#include <leveldb/cache.h>
#include <leveldb/filter_policy.h>
#include <memenv/memenv.h>
#include <openssl/rand.h>

#include "kernel.h"
#include "checkpoints.h"
#include "txdb-leveldb.h"
#include <set>
#include "util.h"
#include "hash.h"   // R3: event/journal hash chaining (C6 section 3)
#include "main.h"
#include "blockindex_residency_counters.h"

extern bool RebuildMainChainForwardLinks();

using namespace std;
namespace fs = boost::filesystem;

leveldb::DB *txdb; // global pointer for LevelDB object instance

// S12 test-only shared-handle lifetime identity probes. Inert unless a fixture
// reads them; they never alter behavior.
int g_testTxdbCloseCount = 0;
void* g_testTxdbLastClosedPtr = NULL;
int g_testTxdbOpenCount = 0;
void* g_testTxdbLastOpenedPtr = NULL;

static CCriticalSection cs_txdb;

static int nIBDBatchSize = 0;
static int nIBDBatchCount = 0;
static bool fIBDBatchPending = false;

// R2c.1d2 bootstrap-only default-off failure seams. They are deliberately
// consulted only by BootstrapDAGSourceStateId; ordinary transactions cannot be
// affected by these tests.
static CCriticalSection cs_IBDBatch;

void InitIBDBatching()
{
    nIBDBatchSize = GetArg("-ibdbatchsize", 50);
    if (nIBDBatchSize < 0) nIBDBatchSize = 0;
    if (nIBDBatchSize > 1000) nIBDBatchSize = 1000;
    if (nIBDBatchSize > 0)
        printf("IBD batching enabled: committing every %d blocks\n", nIBDBatchSize);
}

void FlushIBDBatch()
{
    LOCK(cs_IBDBatch);
    if (fIBDBatchPending && txdb)
    {
        printf("Flushing pending IBD batch (%d blocks)...\n", nIBDBatchCount);
        CTxDB txdbFlush;
        txdbFlush.TxnBegin();
        txdbFlush.TxnCommit();
        fIBDBatchPending = false;
        nIBDBatchCount = 0;
    }
}

static leveldb::Options GetOptions() {
    leveldb::Options options;
    int nCacheSizeMB = GetArg("-dbcache", 300);
    options.block_cache = leveldb::NewLRUCache(nCacheSizeMB * 1048576);
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    options.write_buffer_size = 64 * 1048576; // 64MB write buffer (default 4MB) for smoother IBD
    options.max_open_files = 1000;
    options.compression = leveldb::kSnappyCompression;
    return options;
}

// ---------------------------------------------------------------------------
// ARCHIVE-1 invariant (Stage 3B-D1-S0) — the block archive is permanent.
//
// blk%04u.dat hold every historical block payload from genesis onward. They are
// ARCHIVE data, not index data: the transaction/block INDEX in `txleveldb` is
// reconstructable (a fresh index can be rebuilt from the archive), while a
// deleted block payload is not reconstructable locally. Therefore:
//   * no initialization, version-mismatch, index-reset, migration or recovery
//     path may unlink, truncate, overwrite or rename-away a blk*.dat file;
//   * an index reset removes INDEX STATE ONLY;
//   * losing an archive file during a reset is a fail-closed condition, never a
//     silently ignored side effect.
//
// History: this reset path used to delete every blk*.dat (a prefix scan from
// blk0001.dat up to the first gap) whenever a DATABASE_VERSION mismatch armed
// the reset, turning an index upgrade into a full re-download. S0 removed that
// deletion and added the post-reset verification below. See the S0 report
// (innova-v2-chainstate-stage3b-d1-s0-20261016.md) and
// src/test/d1_s0_archive_guard_tests.cpp.
// ---------------------------------------------------------------------------

// Name grammar of a block-archive file: "blk" + 4..10 decimal digits + ".dat" (the writer emits
// strprintf("blk%04u.dat", nFile) with an unsigned int nFile, so 4 digits is the minimum and 10 is
// the maximum). Everything else — near-misses such as "blk1.dat", "blk0001.DAT", "blk0001.dat.bak",
// "xblk0001.dat", "blk 0001.dat" — is NOT archive data and must never be counted as a block file.
bool IsBlockArchiveName(const std::string& name)
{
    if (name.size() < 11 || name.size() > 17)   // "blk" + 4..10 digits + ".dat"
        return false;
    if (name.compare(0, 3, "blk") != 0)
        return false;
    if (name.compare(name.size() - 4, 4, ".dat") != 0)
        return false;
    for (std::string::size_type i = 3; i + 4 < name.size(); ++i)
        if (name[i] < '0' || name[i] > '9')
            return false;
    return true;
}

// Read-only enumeration of the block archive in `dir` (never creates, renames, truncates or removes
// anything; never reads block payloads — the cost is one directory entry per file, never its size).
//
// Every outcome that leaves the archive set unknowable is reported as such: an unreadable or
// non-directory datadir, an enumeration failure, and an archive-named entry that is not a usable
// payload (non-regular file, dangling symlink, or an entry whose type cannot be determined). Only
// the entries returned in `payloads` may be treated as verified block data, and only ARCHIVE_SCAN_OK
// means the enumeration itself is trustworthy.
ArchiveScan ScanBlockArchive(const fs::path& dir)
{
    ArchiveScan scan;
    scan.status = ARCHIVE_SCAN_OK;

    boost::system::error_code ec;
    const fs::file_status dirStatus = fs::status(dir, ec);   // the datadir itself may be a symlink
    if (ec) {
        // In this boost version status(p, ec) reports a missing path through ec rather than through
        // the returned not-found status, so classify that case explicitly: "the datadir is not there
        // yet" is a different operator situation from "the datadir cannot be examined".
        scan.status = ARCHIVE_SCAN_DATADIR_INVALID;
        scan.detail = (ec == boost::system::errc::no_such_file_or_directory)
            ? strprintf("data directory %s does not exist", dir.string().c_str())
            : strprintf("data directory %s cannot be examined: %s",
                        dir.string().c_str(), ec.message().c_str());
        return scan;
    }
    if (!fs::is_directory(dirStatus)) {
        scan.status = ARCHIVE_SCAN_DATADIR_INVALID;
        scan.detail = fs::exists(dirStatus)
            ? strprintf("data directory %s is not a directory", dir.string().c_str())
            : strprintf("data directory %s does not exist", dir.string().c_str());
        return scan;
    }

    fs::directory_iterator it(dir, ec);
    if (ec) {
        scan.status = ARCHIVE_SCAN_UNREADABLE;
        scan.detail = strprintf("data directory %s cannot be enumerated: %s",
                                dir.string().c_str(), ec.message().c_str());
        return scan;
    }

    const fs::directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            // An enumeration error mid-scan leaves the set unknowable: a partial list is never
            // reported as a verified one.
            scan.status = ARCHIVE_SCAN_UNREADABLE;
            scan.detail = strprintf("enumeration of data directory %s failed: %s",
                                    dir.string().c_str(), ec.message().c_str());
            return scan;
        }

        const fs::path entry = it->path();
        const std::string name = entry.filename().string();
        if (!IsBlockArchiveName(name))
            continue;   // not archive data: never counted, never verified, never touched

        // Classify with lstat FIRST (symlink_status does not follow links). The question is not
        // "does this path resolve?" but "is this path a usable block payload?".
        boost::system::error_code lec;
        const fs::file_status linkStatus = fs::symlink_status(entry, lec);
        if (lec) {
            scan.status = ARCHIVE_SCAN_ENTRY_BAD;
            scan.detail = strprintf("block-archive entry %s cannot be examined: %s",
                                    name.c_str(), lec.message().c_str());
            return scan;
        }

        if (fs::is_symlink(linkStatus)) {
            // A symlink is followed EXPLICITLY: it counts as a payload only while it resolves to a
            // regular file. A dangling link is an entry the archive cannot account for — neither
            // "missing" (nothing was removed) nor "intact" (no payload is reachable through it).
            boost::system::error_code tec;
            const fs::file_status targetStatus = fs::status(entry, tec);
            if (tec || !fs::is_regular_file(targetStatus)) {
                scan.status = ARCHIVE_SCAN_ENTRY_BAD;
                scan.detail = tec
                    ? strprintf("block-archive entry %s is a symlink whose target cannot be resolved: %s",
                                name.c_str(), tec.message().c_str())
                    : strprintf("block-archive entry %s is a symlink to a %s, not to a regular file",
                                name.c_str(),
                                fs::is_directory(targetStatus) ? "directory" : "special file");
                return scan;
            }
            scan.notes.push_back(strprintf(
                "%s is a symlink to a regular file: presence is verified, payload identity is not",
                name.c_str()));
        } else if (!fs::is_regular_file(linkStatus)) {
            scan.status = ARCHIVE_SCAN_ENTRY_BAD;
            scan.detail = strprintf("block-archive entry %s is not a regular file (it is a %s)",
                                    name.c_str(),
                                    fs::is_directory(linkStatus) ? "directory"
                                                                 : "special file (fifo/socket/device)");
            return scan;
        }

        scan.payloads.push_back(name);
    }

    std::sort(scan.payloads.begin(), scan.payloads.end());
    std::sort(scan.notes.begin(), scan.notes.end());
    return scan;
}

void init_blockindex(leveldb::Options& options, bool fResetIndex = false) {
    // First time init.
    fs::path directory = GetDataDir() / "txleveldb";

    // ARCHIVE-1 (S0-A): an index reset resets the INDEX ONLY. The block archive is
    // deliberately NOT touched here — it used to be deleted by this function.
    ArchiveScan archiveBefore;
    if (fResetIndex) {
        archiveBefore = ScanBlockArchive(GetDataDir());
        if (archiveBefore.status == ARCHIVE_SCAN_OK)
            printf("Index reset: index state only, %u block-archive file(s) under %s are preserved\n",
                   (unsigned int)archiveBefore.payloads.size(), GetDataDir().string().c_str());
        else
            // Never print a count the enumeration could not establish: an archive that cannot be
            // accounted for is reported as exactly that, before anything is removed.
            printf("Index reset: index state only, but the block archive under %s cannot be accounted "
                   "for: %s\n", GetDataDir().string().c_str(), archiveBefore.detail.c_str());
        for (size_t i = 0; i < archiveBefore.notes.size(); ++i)
            printf("Index reset: note: %s\n", archiveBefore.notes[i].c_str());

        // ARCHIVE-1 (S0-F1): an archive the enumeration cannot account for is refused BEFORE
        // anything is removed. The diagnostic describes what was actually found, so a pre-existing
        // oddity is never reported as damage caused by the reset, and an invalid or unreadable
        // datadir can never pass as a verified (empty) archive.
        if (archiveBefore.status != ARCHIVE_SCAN_OK)
            throw runtime_error(strprintf(
                "ARCHIVE-1 VIOLATION: refusing to reset the index in %s: %s. The block archive "
                "cannot be accounted for, so the reset cannot be proven non-destructive; refusing "
                "to continue. Fix or remove the offending entry before restarting.",
                directory.string().c_str(), archiveBefore.detail.c_str()));

        fs::remove_all(directory);
    }

    fs::create_directory(directory);
    printf("Opening LevelDB in %s\n", directory.string().c_str());
    leveldb::Status status = leveldb::DB::Open(options, directory.string(), &txdb);
    if (!status.ok()) {
        throw runtime_error(strprintf("init_blockindex(): error opening database environment %s", status.ToString().c_str()));
    }

    // ARCHIVE-1 guard (S0-B): verify that the reset destroyed nothing. Read-only, runs only on the
    // reset path, and fails CLOSED: a missing archive entry is a loud startup error with a clear
    // diagnostic, never a silently ignored side effect.
    if (fResetIndex) {
        ArchiveScan archiveAfter = ScanBlockArchive(GetDataDir());
        for (size_t i = 0; i < archiveAfter.notes.size(); ++i)
            printf("Index reset: note: %s\n", archiveAfter.notes[i].c_str());
        if (archiveAfter.status != ARCHIVE_SCAN_OK)
            throw runtime_error(strprintf(
                "ARCHIVE-1 VIOLATION: the block archive in %s could not be accounted for after the "
                "index was reset: %s. Block data is permanent and must never be changed by "
                "index/version logic; refusing to continue. Inspect the archive before restarting.",
                GetDataDir().string().c_str(), archiveAfter.detail.c_str()));

        // Set comparison against the pre-reset enumeration: a payload that is gone, or that stopped
        // being a regular file, is a hard failure. (Byte identity is proven by the regression suite;
        // this guard is the runtime tripwire, deliberately bounded by the entry count.)
        std::vector<std::string> missing;
        for (size_t i = 0; i < archiveBefore.payloads.size(); ++i)
            if (!std::binary_search(archiveAfter.payloads.begin(), archiveAfter.payloads.end(),
                                    archiveBefore.payloads[i]))
                missing.push_back(archiveBefore.payloads[i]);
        if (!missing.empty()) {
            std::string list;
            for (size_t i = 0; i < missing.size(); ++i)
                list += (i ? ", " : "") + missing[i];
            throw runtime_error(strprintf(
                "ARCHIVE-1 VIOLATION: block-archive file(s) %s disappeared or stopped being a "
                "regular file while the index in %s was reset. Block data is permanent and must "
                "never be removed by index/version logic; refusing to continue. Restore the file(s) "
                "before restarting.",
                list.c_str(), directory.string().c_str()));
        }
        printf("Index reset verified non-destructive: %u block-archive file(s) intact\n",
               (unsigned int)archiveAfter.payloads.size());
    }
}

// CDB subclasses are created and destroyed VERY OFTEN. That's why
// we shouldn't treat this as a free operations.
CTxDB::CTxDB(const char* pszMode)
{
    assert(pszMode);
    activeBatch = NULL;
    fReadOnly = (!strchr(pszMode, '+') && !strchr(pszMode, 'w'));

    LOCK(cs_txdb);

    if (txdb) {
        pdb = txdb;
        return;
    }

    bool fCreate = strchr(pszMode, 'c');

    options = GetOptions();
    options.create_if_missing = true; //fCreate
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);

    init_blockindex(options); // Init directory
    pdb = txdb;
    ++g_testTxdbOpenCount;
    g_testTxdbLastOpenedPtr = (void*)txdb;

    if (Exists(string("version")))
    {
        ReadVersion(nVersion);
        printf("Transaction index version is %d\n", nVersion);

        // ROUND-3 P3b: the version-mismatch backup/reset dance RENAMES the
        // whole legacy store and recreates it. On a READ-ONLY handle (e.g. the
        // Checkpoints::LoadPersistedSyncCheckpoint boot probe) that is a
        // destructive write behind the caller's back; skip the dance entirely
        // and let the caller proceed against the existing store (any
        // incompatible read is reported through the typed read seam).
        if (nVersion < DATABASE_VERSION && !fReadOnly)
        {
            printf("CTxDB() : database version %d is older than expected %d, creating backup\n",
                   nVersion, DATABASE_VERSION);
            bool fBackupOk = false;
            try {
                boost::filesystem::path backupPath = GetDataDir() / "txleveldb_backup";
                if (boost::filesystem::exists(backupPath))
                    boost::filesystem::remove_all(backupPath);
                boost::filesystem::rename(GetDataDir() / "txleveldb", backupPath);
                printf("CTxDB() : backed up old database to %s\n", backupPath.filename().string().c_str());
                fBackupOk = true;
            } catch (const boost::filesystem::filesystem_error& e) {
                printf("CTxDB() : CRITICAL - failed to backup database: %s\n", e.what());
                printf("CTxDB() : Database reset aborted. Please manually backup txleveldb and restart.\n");
            }

            if (!fBackupOk)
            {
                printf("CTxDB() : Continuing with old database version %d\n", nVersion);
            }
            else
            {

            printf("Required index version is %d, removing old database\n", DATABASE_VERSION);

            // Leveldb instance destruction
            delete txdb;
            txdb = pdb = NULL;
            delete activeBatch;
            activeBatch = NULL;

            init_blockindex(options, true); // Reset index state ONLY (ARCHIVE-1: block files preserved)
            pdb = txdb;

            bool fTmp = fReadOnly;
            fReadOnly = false;
            WriteVersion(DATABASE_VERSION); // Save transaction index version
            fReadOnly = fTmp;
            } // end fBackupOk else block
        }
    }
    else if (fCreate)
    {
        bool fTmp = fReadOnly;
        fReadOnly = false;
        WriteVersion(DATABASE_VERSION);
        fReadOnly = fTmp;
    }

    printf("Opened LevelDB successfully\n");
}

void CTxDB::Close()
{
    LOCK(cs_txdb);
    if (txdb) {
        ++g_testTxdbCloseCount;
        g_testTxdbLastClosedPtr = (void*)txdb;
    }
    delete txdb;
    txdb = pdb = NULL;
    delete options.filter_policy;
    options.filter_policy = NULL;
    delete options.block_cache;
    options.block_cache = NULL;
    delete activeBatch;
    activeBatch = NULL;
}

// S12 test-only accessor: current shared/global txleveldb handle identity.
void* GetGlobalTxdbPtrForTest()
{
    return (void*)txdb;
}

bool CTxDB::TxnAbort()
{
    delete activeBatch;
    activeBatch = NULL;
    return true;
}

bool CTxDB::TxnBegin()
{
    if (activeBatch)
        return false;
    activeBatch = new leveldb::WriteBatch();
    return true;
}

bool CTxDB::TxnCommit()
{
    if (!activeBatch)
        return false;

    leveldb::WriteOptions writeOptions;
    if (IsInitialBlockDownload() && nIBDBatchSize > 0)
    {
        writeOptions.sync = false;
        LOCK(cs_IBDBatch);
        fIBDBatchPending = true;
        nIBDBatchCount++;
    }

    leveldb::Status status = pdb->Write(writeOptions, activeBatch);
    delete activeBatch;
    activeBatch = NULL;
    if (!status.ok()) {
        printf("LevelDB batch commit failure: %s\n", status.ToString().c_str());
        return false;
    }
    return true;
}

bool CTxDB::WriteKeyImage(ec_point& keyImage, CKeyImageSpent& keyImageSpent)
{
    return Write(make_pair(string("ki"), keyImage), keyImageSpent);
};

bool CTxDB::ReadKeyImage(ec_point& keyImage, CKeyImageSpent& keyImageSpent)
{
    return Read(make_pair(string("ki"), keyImage), keyImageSpent);
};

bool CTxDB::EraseKeyImage(ec_point& keyImage)
{
    return Erase(make_pair(string("ki"), keyImage));
}

bool CTxDB::WriteAnonOutput(CPubKey& pkCoin, CAnonOutput& ao)
{
    return Write(make_pair(string("ao"), pkCoin), ao);
};

bool CTxDB::ReadAnonOutput(CPubKey& pkCoin, CAnonOutput& ao)
{
    return Read(make_pair(string("ao"), pkCoin), ao);
};

bool CTxDB::EraseAnonOutput(CPubKey& pkCoin)
{
    return Erase(make_pair(string("ao"), pkCoin));
}




// IDAG Phase 2: DAG link persistence. vDAGParents is canonical; the
// child-count projection is updated in this same active WriteBatch.


// Bounded keyed source authority, never a resident DAG or overlay query.


// G6 authoritative row-level attestation. Same membership postimage as
// ReadDAGFrontierMembership, but canonical ROW PRESENCE is attested
// separately so an authoritative enumeration can distinguish a legitimate
// non-member (row present, has children) from an enumerated frontier tip whose
// canonical row is MISSING (incomplete canonical source => fail closed; never
// a silently reduced VALID vector). Additive: the legacy reader is unchanged
// for S5 preview / delta capture consumers.


// R3 / C6 section 6 helpers (defined with the certificate machinery below; declared
// here because the provenance transitions above need the canonical row serialization).








// ---------------------------------------------------------------------------
// R3 / C6 provenance state (additive). Sections 1-4 of the accepted C6 contract:
// typed row outcome, per-row incarnation, positive prune evidence, custody state.
// (DAGROW_HEIGHT_UNKNOWN is declared in txdb-leveldb.h.)
// ---------------------------------------------------------------------------

// R3 / C6 section 1 — typed DAG-row read. The legacy bool read collapses
// not-found, I/O failure and deserialization failure into one false, which the
// consensus path then reads as "row absent". This classifies the three apart and
// NEVER reports a present-but-malformed row or an I/O failure as absence.
// Returns false only on a programming error (null out-params); on true the
// caller MUST branch on *outcome.






















// ---------------------------------------------------------------------------
// R3 / C6 sections 5-6 — coverage certificate + custody seal.
//
// The certificate IS the accumulator: every digest is maintained incrementally by
// the supported writer in the SAME batch as the mutation it describes, and each is
// independently recomputable by a full scan of the committed store (the
// certification scan). "The certificate verifies" is therefore a recomputation,
// never a timestamp, a floor advancement or a final-state digest alone.
// ---------------------------------------------------------------------------
static bool g_dagCertDeepVerified = false;
bool g_testSuppressDagCustodyWatermark = false;   // R3.9 case M (test-only; see header)

static uint256 XorUint256(uint256 a, uint256 b)
{
    uint256 r = a;
    for (int i = 0; i < 32; ++i) r.begin()[i] = a.begin()[i] ^ b.begin()[i];
    return r;
}

static uint256 HashRowCoverage(const uint256& hash, const std::string& raw)
{
    CDataStream s(SER_GETHASH, CLIENT_VERSION);
    s << (unsigned char)'P' << hash << raw;
    return Hash(s.begin(), s.end());
}

static uint256 HashIncarnation(const uint256& hash, uint64_t inc)
{
    CDataStream s(SER_GETHASH, CLIENT_VERSION);
    s << (unsigned char)'I' << hash << inc;
    return Hash(s.begin(), s.end());
}

static uint256 HashKnownVertex(const uint256& hash)
{
    CDataStream s(SER_GETHASH, CLIENT_VERSION);
    s << (unsigned char)'V' << hash;
    return Hash(s.begin(), s.end());
}


























// R3.6: a floor / clean-height change is part of the certified coverage state, so it
// re-binds the floor-state digest and re-publishes the certificate in the ACTIVE batch.
// A store that is not certified is never certified by this side effect.


















































// F2 erase provenance: written ONLY by the prune/erase lifecycle
// (the retired prune path, in the same atomic batch as the erasures) and
// consumed ONLY as ERASE provenance (LegitimatePrunedBoundary). Distinct key,
// distinct meaning - never advanced by Shutdown().






// F2-B1-R row-erase provenance (see header). Read is three-state: false means the
// store could not be read (callers fail closed); true with *present=false means
// there is no record (the absence is not attributable to a non-prune lifecycle).










// STRICT persisted daglinks enumeration for authoritative source construction.
// Rejects (returns false, populates *error) any malformed record instead of
// skipping it: malformed key, unexpected key-prefix field, malformed value,
// missing/trailing bytes, or an iterator/read error. A canonical daglinks
// record is exactly: key = pair<string("daglinks"), uint256>, value = the
// legacy daglinks-row serialization (vDAGParents, vDAGChildren, fBlue, nDAGScore,
// nDAGOrder, then optional nInferredK for Phase-4/Phase-5 records). Duplicate
// canonical identity (same hash twice) is rejected. After reading every
// well-formed record the iterator status is checked and a read error yields
// false. This is the fail-closed drain for EnumerateAuthoritativeStagedScope:
// an incomplete/malformed retained canvas can never be silently certified.

// S3 staged-view enumeration: scan the active WriteBatch for daglinks
// writes/tombstones (prefix "daglinks"), so the authoritative staged view can be
// built as (persisted daglinks + staged writes - staged tombstones). Fail closed
// on any batch scan error; report activeBatchOpen=false when no txn is open.


class CBatchScanner : public leveldb::WriteBatch::Handler {
public:
    std::string needle;
    bool *deleted;
    std::string *foundValue;
    bool foundEntry;

    CBatchScanner() : foundEntry(false) {}

    virtual void Put(const leveldb::Slice& key, const leveldb::Slice& value) {
        if (key.ToString() == needle) {
            foundEntry = true;
            *deleted = false;
            *foundValue = value.ToString();
        }
    }

    virtual void Delete(const leveldb::Slice& key) {
        if (key.ToString() == needle) {
            foundEntry = true;
            *deleted = true;
        }
    }
};

// When performing a read, if we have an active batch we need to check it first
// before reading from the database, as the rest of the code assumes that once
// a database transaction begins reads are consistent with it. It would be good
// to change that assumption in future and avoid the performance hit, though in
// practice it does not appear to be large.
bool CTxDB::ScanBatch(const CDataStream &key, string *value, bool *deleted) const {
    assert(activeBatch);
    *deleted = false;
    CBatchScanner scanner;
    scanner.needle = key.str();
    scanner.deleted = deleted;
    scanner.foundValue = value;
    leveldb::Status status = activeBatch->Iterate(&scanner);
    if (!status.ok()) {
        throw runtime_error(status.ToString());
    }
    return scanner.foundEntry;
}

bool CTxDB::WriteAddrIndex(uint160 addrHash, uint256 txHash)
{
    std::vector<uint256> txHashes;
    if(!ReadAddrIndex(addrHash, txHashes))
    {
	txHashes.push_back(txHash);
        return Write(make_pair(string("adr"), addrHash), txHashes);
    }
    else
    {
	if(std::find(txHashes.begin(), txHashes.end(), txHash) == txHashes.end())
    	{
    	    txHashes.push_back(txHash);
            return Write(make_pair(string("adr"), addrHash), txHashes);
	}
	else
	{
	    return true; // already have this tx hash
	}
    }
}

bool CTxDB::ReadAddrIndex(uint160 addrHash, std::vector<uint256>& txHashes)
{
    return Read(make_pair(string("adr"), addrHash), txHashes);
}

bool CTxDB::ReadTxIndex(uint256 hash, CTxIndex& txindex)
{
    txindex.SetNull();
    return Read(make_pair(string("tx"), hash), txindex);
}

bool CTxDB::UpdateTxIndex(uint256 hash, const CTxIndex& txindex)
{
    return Write(make_pair(string("tx"), hash), txindex);
}

bool CTxDB::AddTxIndex(const CTransaction& tx, const CDiskTxPos& pos, int nHeight)
{
    // Add to tx index
    uint256 hash = tx.GetHash();
    CTxIndex txindex(pos, tx.vout.size());
    return Write(make_pair(string("tx"), hash), txindex);
}

bool CTxDB::EraseTxIndex(const CTransaction& tx)
{
    uint256 hash = tx.GetHash();

    return Erase(make_pair(string("tx"), hash));
}

bool CTxDB::ContainsTx(uint256 hash)
{
    return Exists(make_pair(string("tx"), hash));
}

bool CTxDB::ReadDiskTx(uint256 hash, CTransaction& tx, CTxIndex& txindex)
{
    tx.SetNull();
    if (!ReadTxIndex(hash, txindex))
        return false;
    return (tx.ReadFromDisk(txindex.pos));
}

bool CTxDB::ReadDiskTx(uint256 hash, CTransaction& tx)
{
    CTxIndex txindex;
    return ReadDiskTx(hash, tx, txindex);
}

bool CTxDB::ReadDiskTx(COutPoint outpoint, CTransaction& tx, CTxIndex& txindex)
{
    return ReadDiskTx(outpoint.hash, tx, txindex);
}

bool CTxDB::ReadDiskTx(COutPoint outpoint, CTransaction& tx)
{
    CTxIndex txindex;
    return ReadDiskTx(outpoint.hash, tx, txindex);
}

bool CTxDB::WriteBlockIndex(const CDiskBlockIndex& blockindex)
{
    return Write(make_pair(string("blockindex"), blockindex.GetBlockHash()), blockindex);
}

bool CTxDB::EraseBlockIndex(const uint256& blockhash)
{
    return Erase(make_pair(string("blockindex"), blockhash));
}

bool CTxDB::ReadHashBestChain(uint256& hashBestChain)
{
    return Read(string("hashBestChain"), hashBestChain);
}

bool CTxDB::WriteHashBestChain(uint256 hashBestChain)
{
    return Write(string("hashBestChain"), hashBestChain);
}

bool CTxDB::ReadBestInvalidTrust(CBigNum& bnBestInvalidTrust)
{
    return Read(string("bnBestInvalidTrust"), bnBestInvalidTrust);
}

bool CTxDB::WriteBestInvalidTrust(CBigNum bnBestInvalidTrust)
{
    return Write(string("bnBestInvalidTrust"), bnBestInvalidTrust);
}

bool CTxDB::ReadInvalidBlockSet(std::set<uint256>& setInvalidBlockHash)
{
    setInvalidBlockHash.clear();
    // Missing key means the operator has not invalidated anything yet.
    return Read(string("setInvalidBlockHash"), setInvalidBlockHash);
}

bool CTxDB::WriteInvalidBlockSet(const std::set<uint256>& setInvalidBlockHash)
{
    return Write(string("setInvalidBlockHash"), setInvalidBlockHash);
}

bool CTxDB::ReadCandidateTips(std::map<uint256, CandidateTipRecord>& tips)
{
    return Read(string("candidateTips"), tips);
}

bool CTxDB::WriteCandidateTips(const std::map<uint256, CandidateTipRecord>& tips)
{
    return Write(string("candidateTips"), tips);
}

bool CTxDB::ReadSyncCheckpoint(uint256& hashCheckpoint)
{
    return Read(string("hashSyncCheckpoint"), hashCheckpoint);
}

// V2-R2B (FIX A): typed persisted sync-checkpoint read. ABSENT and CORRUPT are
// distinct so the V2 authoritative startup can fail closed on a durable
// inconsistency while treating a never-established checkpoint as the canonical
// ppcoin 0 ("no synchronized checkpoint"). No genesis substitution.
SyncCheckpointLoadStatus CTxDB::LoadSyncCheckpoint(uint256& hashCheckpoint)
{
    hashCheckpoint = uint256(0);
    if (!Exists(string("hashSyncCheckpoint")))
        return SYNC_CHECKPOINT_ABSENT;
    uint256 valueRead = uint256(0);
    if (!Read(string("hashSyncCheckpoint"), valueRead))
        return SYNC_CHECKPOINT_CORRUPT;
    hashCheckpoint = valueRead;
    return SYNC_CHECKPOINT_LOADED;
}

bool CTxDB::WriteSyncCheckpoint(uint256 hashCheckpoint)
{
    return Write(string("hashSyncCheckpoint"), hashCheckpoint);
}

bool CTxDB::ReadCheckpointPubKey(string& strPubKey)
{
    return Read(string("strCheckpointPubKey"), strPubKey);
}

bool CTxDB::WriteCheckpointPubKey(const string& strPubKey)
{
    return Write(string("strCheckpointPubKey"), strPubKey);
}

static CBlockIndex *InsertBlockIndex(uint256 hash)
{
    if (hash == 0)
        return NULL;

    // Return existing
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hash);
    if (mi != mapBlockIndex.end())
        return (*mi).second;

    // Create new
    CBlockIndex* pindexNew = new CBlockIndex();
    if (!pindexNew)
        throw runtime_error("LoadBlockIndex() : new CBlockIndex failed");
    mi = mapBlockIndex.insert(make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);
    g_res_cblockindex_constructed++;
    g_res_mapinserts++;

    return pindexNew;
}

bool CTxDB::LoadBlockIndex()
{
    g_res_loadblockindex_calls++;
    if (mapBlockIndex.size() > 0) {
        // Already loaded once in this session. It can happen during migration
        // from BDB.
        return true;
    }
    // The block index is an in-memory structure that maps hashes to on-disk
    // locations where the contents of the block can be found. Here, we scan it
    // out of the DB and into mapBlockIndex.
    leveldb::Iterator *iterator = pdb->NewIterator(leveldb::ReadOptions());
    // Seek to start key.
    CDataStream ssStartKey(SER_DISK, CLIENT_VERSION);
    ssStartKey << make_pair(string("blockindex"), uint256(0));
    iterator->Seek(ssStartKey.str());
    // Now read each entry.
    while (iterator->Valid())
    {
        // Unpack keys and values.
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey.write(iterator->key().data(), iterator->key().size());
        CDataStream ssValue(SER_DISK, CLIENT_VERSION);
        ssValue.write(iterator->value().data(), iterator->value().size());
        string strType;
        ssKey >> strType;
        // Did we reach the end of the data to read?
        if (fRequestShutdown || strType != "blockindex")
            break;
        CDiskBlockIndex diskindex;
        ssValue >> diskindex;

        uint256 blockHash = diskindex.GetBlockHash();

        // Construct block index object
        CBlockIndex* pindexNew    = InsertBlockIndex(blockHash);
        pindexNew->pprev          = InsertBlockIndex(diskindex.hashPrev);
        pindexNew->hashPrevStable = diskindex.hashPrev;   // C12: pointer-free parent identity (survives edge release / restart)
        pindexNew->pnext          = InsertBlockIndex(diskindex.hashNext);
        g_res_pprev_links++;
        g_res_pnext_links++;
        pindexNew->nFile          = diskindex.nFile;
        pindexNew->nBlockPos      = diskindex.nBlockPos;
        pindexNew->nHeight        = diskindex.nHeight;
        pindexNew->nMint          = diskindex.nMint;
        pindexNew->nMoneySupply   = diskindex.nMoneySupply;
        pindexNew->nFlags         = diskindex.nFlags;
        pindexNew->nStakeModifier = diskindex.nStakeModifier;
        pindexNew->prevoutStake   = diskindex.prevoutStake;
        pindexNew->nStakeTime     = diskindex.nStakeTime;
        pindexNew->hashProof      = diskindex.hashProof;
        pindexNew->nVersion       = diskindex.nVersion;
        pindexNew->hashMerkleRoot = diskindex.hashMerkleRoot;
        pindexNew->nTime          = diskindex.nTime;
        pindexNew->nBits          = diskindex.nBits;
        pindexNew->nNonce         = diskindex.nNonce;
        // nSize populated later during chain trust calculation pass (not serialized for backward compat)

        // Watch for genesis block
        if (pindexGenesisBlock == NULL && blockHash == GetGenesisBlockHash())
            pindexGenesisBlock = pindexNew;

        if (!pindexNew->CheckIndex()) {
            delete iterator;
            return error("LoadBlockIndex() : CheckIndex failed at %d", pindexNew->nHeight);
        }

        // NovaCoin: build setStakeSeen
        if (pindexNew->IsProofOfStake())
            setStakeSeen.insert(make_pair(pindexNew->prevoutStake, pindexNew->nStakeTime));

        iterator->Next();
    }
    delete iterator;

    if (fRequestShutdown)
        return true;

    // Calculate nChainTrust
    vector<pair<int, CBlockIndex*> > vSortedByHeight;
    vSortedByHeight.reserve(mapBlockIndex.size());
    for (const PAIRTYPE(uint256, CBlockIndex*)& item : mapBlockIndex)
    {
        CBlockIndex* pindex = item.second;
        vSortedByHeight.push_back(make_pair(pindex->nHeight, pindex));
    }
    sort(vSortedByHeight.begin(), vSortedByHeight.end());
    for (const PAIRTYPE(int, CBlockIndex*)& item : vSortedByHeight)
    {
        CBlockIndex* pindex = item.second;
        pindex->BuildSkip();
        pindex->nChainTrust = (pindex->pprev ? pindex->pprev->nChainTrust : 0) + pindex->GetBlockTrust();
        // NovaCoin: calculate stake modifier checksum
        pindex->nStakeModifierChecksum = GetStakeModifierChecksum(pindex);
        if (!CheckStakeModifierCheckpoints(pindex->nHeight, pindex->nStakeModifierChecksum))
            return error("CTxDB::LoadBlockIndex() : Failed stake modifier checkpoint height=%d, modifier=0x%016" PRIx64, pindex->nHeight, pindex->nStakeModifier);
    }

    // Load operator-invalidated block hashes BEFORE best-chain reconstruction so
    // a stored best chain that descends from an invalidated block is healed
    // below and never accepted as the active chain.
    ReadInvalidBlockSet(setInvalidBlockHash);
    if (fDebug && !setInvalidBlockHash.empty())
    {
        for (std::set<uint256>::const_iterator it = setInvalidBlockHash.begin();
             it != setInvalidBlockHash.end(); ++it)
        {
            std::map<uint256, CBlockIndex*>::const_iterator mi =
                mapBlockIndex.find(*it);
            printf("INVALIDBLOCKSET hash=%s height=%d\n",
                   it->ToString().c_str(),
                   mi != mapBlockIndex.end() ? mi->second->nHeight : -1);
        }
    }

    // Load hashBestChain pointer to end of best chain
    if (!ReadHashBestChain(hashBestChain))
    {
        if (pindexGenesisBlock == NULL)
            return true;
        return error("CTxDB::LoadBlockIndex() : hashBestChain not loaded");
    }
    if (!mapBlockIndex.count(hashBestChain))
        return error("CTxDB::LoadBlockIndex() : hashBestChain not found in the block index");
    pindexBest = mapBlockIndex[hashBestChain];
    nBestHeight = pindexBest->nHeight;
    nBestChainTrust = pindexBest->nChainTrust;

    // Crash-window healing: a crash between persisting an operator-invalidated
    // hash and rolling back the active chain leaves hashBestChain descending
    // from that hash. Roll back to the highest valid ancestor before the node
    // starts operating.
    if (!RecoverFromInvalidatedBestChain())
        return error("CTxDB::LoadBlockIndex() : failed to recover from invalidated best chain");

    // Legacy releases did not reliably persist hashNext. Rebuild the
    // authoritative forward chain from pprev and hashBestChain so getblocks
    // and getheaders work after a restart.
    if (!RebuildMainChainForwardLinks())
        return false;

    printf("LoadBlockIndex(): hashBestChain=%s  height=%d  trust=%s  date=%s\n",
      hashBestChain.ToString().substr(0,20).c_str(), nBestHeight, CBigNum(nBestChainTrust).ToString().c_str(),
      DateTimeStrFormat("%x %H:%M:%S", pindexBest->GetBlockTime()).c_str());

    // NovaCoin: load hashSyncCheckpoint
    if (!ReadSyncCheckpoint(Checkpoints::hashSyncCheckpoint))
        return error("CTxDB::LoadBlockIndex() : hashSyncCheckpoint not loaded");
    printf("LoadBlockIndex(): synchronized checkpoint %s\n", Checkpoints::hashSyncCheckpoint.ToString().c_str());

    // Load bnBestInvalidTrust, OK if it doesn't exist
    CBigNum bnBestInvalidTrust;
    ReadBestInvalidTrust(bnBestInvalidTrust);
    nBestInvalidTrust = bnBestInvalidTrust.getuint256();

    // Verify blocks in the best chain
    int nCheckLevel = GetArg("-checklevel", 1);
    int nCheckDepth = GetArg( "-checkblocks", 2500);
    if (nCheckDepth == 0)
        nCheckDepth = 1000000000; // suffices until the year 19000
    if (nCheckDepth > nBestHeight)
        nCheckDepth = nBestHeight;
    printf("Verifying last %i blocks at level %i\n", nCheckDepth, nCheckLevel);
    CBlockIndex* pindexFork = NULL;
    map<pair<unsigned int, unsigned int>, CBlockIndex*> mapBlockPos;
    for (CBlockIndex* pindex = pindexBest; pindex && pindex->pprev; pindex = pindex->pprev)
    {
        if (fRequestShutdown || pindex->nHeight < nBestHeight-nCheckDepth)
            break;
        CBlock block;
        if (!block.ReadFromDisk(pindex))
            return error("LoadBlockIndex() : block.ReadFromDisk failed");
        // check level 1: verify block validity
        // check level 7: verify block signature too
        if (nCheckLevel>0 && !block.CheckBlock(true, true, (nCheckLevel>6)))
        {
            printf("LoadBlockIndex() : *** found bad block at %d, hash=%s\n", pindex->nHeight, pindex->GetBlockHash().ToString().c_str());
            pindexFork = pindex->pprev;
        }
        // check level 2: verify transaction index validity
        if (nCheckLevel>1)
        {
            pair<unsigned int, unsigned int> pos = make_pair(pindex->nFile, pindex->nBlockPos);
            mapBlockPos[pos] = pindex;
            for (const CTransaction &tx : block.vtx)
            {
                uint256 hashTx = tx.GetHash();
                CTxIndex txindex;
                if (ReadTxIndex(hashTx, txindex))
                {
                    // check level 3: checker transaction hashes
                    if (nCheckLevel>2 || pindex->nFile != txindex.pos.nFile || pindex->nBlockPos != txindex.pos.nBlockPos)
                    {
                        // either an error or a duplicate transaction
                        CTransaction txFound;
                        if (!txFound.ReadFromDisk(txindex.pos))
                        {
                            printf("LoadBlockIndex() : *** cannot read mislocated transaction %s\n", hashTx.ToString().c_str());
                            pindexFork = pindex->pprev;
                        }
                        else
                            if (txFound.GetHash() != hashTx) // not a duplicate tx
                            {
                                printf("LoadBlockIndex(): *** invalid tx position for %s\n", hashTx.ToString().c_str());
                                pindexFork = pindex->pprev;
                            }
                    }
                    // check level 4: check whether spent txouts were spent within the main chain
                    unsigned int nOutput = 0;
                    if (nCheckLevel>3)
                    {
                        for (const CDiskTxPos &txpos : txindex.vSpent)
                        {
                            if (!txpos.IsNull())
                            {
                                pair<unsigned int, unsigned int> posFind = make_pair(txpos.nFile, txpos.nBlockPos);
                                if (!mapBlockPos.count(posFind))
                                {
                                    printf("LoadBlockIndex(): *** found bad spend at %d, hashBlock=%s, hashTx=%s\n", pindex->nHeight, pindex->GetBlockHash().ToString().c_str(), hashTx.ToString().c_str());
                                    pindexFork = pindex->pprev;
                                }
                                // check level 6: check whether spent txouts were spent by a valid transaction that consume them
                                if (nCheckLevel>5)
                                {
                                    CTransaction txSpend;
                                    if (!txSpend.ReadFromDisk(txpos))
                                    {
                                        printf("LoadBlockIndex(): *** cannot read spending transaction of %s:%i from disk\n", hashTx.ToString().c_str(), nOutput);
                                        pindexFork = pindex->pprev;
                                    }
                                    else if (!txSpend.CheckTransaction())
                                    {
                                        printf("LoadBlockIndex(): *** spending transaction of %s:%i is invalid\n", hashTx.ToString().c_str(), nOutput);
                                        pindexFork = pindex->pprev;
                                    }
                                    else
                                    {
                                        bool fFound = false;
                                        for (const CTxIn &txin : txSpend.vin)
                                            if (txin.prevout.hash == hashTx && txin.prevout.n == nOutput)
                                                fFound = true;
                                        if (!fFound)
                                        {
                                            printf("LoadBlockIndex(): *** spending transaction of %s:%i does not spend it\n", hashTx.ToString().c_str(), nOutput);
                                            pindexFork = pindex->pprev;
                                        }
                                    }
                                }
                            }
                            nOutput++;
                        }
                    }
                }
                // check level 5: check whether all prevouts are marked spent
                if (nCheckLevel>4)
                {
                     for (const CTxIn &txin : tx.vin)
                     {
                          CTxIndex txindex;
                          if (ReadTxIndex(txin.prevout.hash, txindex))
                              if (txindex.vSpent.size()-1 < txin.prevout.n || txindex.vSpent[txin.prevout.n].IsNull())
                              {
                                  printf("LoadBlockIndex(): *** found unspent prevout %s:%i in %s\n", txin.prevout.hash.ToString().c_str(), txin.prevout.n, hashTx.ToString().c_str());
                                  pindexFork = pindex->pprev;
                              }
                     }
                }
            }
        }
    }
    if (pindexFork && !fRequestShutdown)
    {
        // Reorg back to the fork
        printf("LoadBlockIndex() : *** moving best chain pointer back to block %d\n", pindexFork->nHeight);
        CBlock block;
        if (!block.ReadFromDisk(pindexFork))
            return error("LoadBlockIndex() : block.ReadFromDisk failed");
        CTxDB txdb;
        block.SetBestChain(txdb, pindexFork);
    }

    return true;
}
