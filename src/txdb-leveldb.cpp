// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.

#include "dag_tips_delta.h"
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
bool g_testFailDAGSourceStateBootstrapMint = false;
bool g_testFailDAGSourceStateBootstrapTxnBegin = false;
bool g_testFailDAGSourceStateBootstrapTxnCommit = false;
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

void init_blockindex(leveldb::Options& options, bool fRemoveOld = false) {
    // First time init.
    fs::path directory = GetDataDir() / "txleveldb";

    if (fRemoveOld) {
        fs::remove_all(directory);
        unsigned int nFile = 1;

        while (true)
        {
            fs::path strBlockFile = GetDataDir() / strprintf("blk%04u.dat", nFile);

            // Break if no such file
            if( !fs::exists( strBlockFile ) )
                break;

            fs::remove(strBlockFile);

            nFile++;
        }
    }

    fs::create_directory(directory);
    printf("Opening LevelDB in %s\n", directory.string().c_str());
    leveldb::Status status = leveldb::DB::Open(options, directory.string(), &txdb);
    if (!status.ok()) {
        throw runtime_error(strprintf("init_blockindex(): error opening database environment %s", status.ToString().c_str()));
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
        VerifyDAGCustodyAtOpen(NULL);   // R3.7 open-time custody continuity
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
    VerifyDAGCustodyAtOpen(NULL);   // R3.7: open-time custody continuity is established
                                    // from the seal, never assumed from row bytes

    if (Exists(string("version")))
    {
        ReadVersion(nVersion);
        printf("Transaction index version is %d\n", nVersion);

        if (nVersion < DATABASE_VERSION)
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

            init_blockindex(options, true); // Remove directory and create new database
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
    // R3.7: a supported clean close self-verifies and seals the custody watermark in a
    // single-record synchronous batch committing to W = P + C. It refuses (and never
    // fabricates) when the store is read-only, is not certified, or has an open batch;
    // an exact post-commit mismatch suspends coverage inside SealDAGCustody.
    if (pdb != NULL && !fReadOnly && activeBatch == NULL)
    {
        std::string sealErr;
        SealDAGCustody(NULL, &sealErr);
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
    if (activeBatch) InvalidateDagTipDeltaTransaction();
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
        InvalidateDagTipDeltaTransaction();
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

bool CTxDB::WriteShieldedNullifier(const uint256& nullifier, const CShieldedNullifierSpent& nfs)
{
    return Write(make_pair(string("sn"), nullifier), nfs);
}

bool CTxDB::ReadShieldedNullifier(const uint256& nullifier, CShieldedNullifierSpent& nfs)
{
    return Read(make_pair(string("sn"), nullifier), nfs);
}

bool CTxDB::EraseShieldedNullifier(const uint256& nullifier)
{
    return Erase(make_pair(string("sn"), nullifier));
}

bool CTxDB::WriteShieldedAnchor(const uint256& anchor)
{
    return Write(make_pair(string("sa"), anchor), true);
}

bool CTxDB::ReadShieldedAnchor(const uint256& anchor)
{
    bool fValid = false;
    if (!Read(make_pair(string("sa"), anchor), fValid))
        return false;
    return fValid;
}

bool CTxDB::EraseShieldedAnchor(const uint256& anchor)
{
    return Erase(make_pair(string("sa"), anchor));
}

bool CTxDB::WriteShieldedAnchorHeight(const uint256& anchor, int nHeight)
{
    return Write(make_pair(string("sah"), anchor), nHeight);
}

bool CTxDB::ReadShieldedAnchorHeight(const uint256& anchor, int& nHeight)
{
    return Read(make_pair(string("sah"), anchor), nHeight);
}

bool CTxDB::WriteShieldedTree(const CIncrementalMerkleTree& tree)
{
    return Write(string("st"), tree);
}

bool CTxDB::ReadShieldedTree(CIncrementalMerkleTree& tree)
{
    return Read(string("st"), tree);
}

bool CTxDB::WriteShieldedTreeAtBlock(const uint256& blockHash, const CIncrementalMerkleTree& tree)
{
    return Write(make_pair(string("sb"), blockHash), tree);
}

bool CTxDB::ReadShieldedTreeAtBlock(const uint256& blockHash, CIncrementalMerkleTree& tree)
{
    return Read(make_pair(string("sb"), blockHash), tree);
}

bool CTxDB::WriteShieldedPoolValue(int64_t nValue)
{
    return Write(string("sv"), nValue);
}

bool CTxDB::ReadShieldedPoolValue(int64_t& nValue)
{
    return Read(string("sv"), nValue);
};

bool CTxDB::WriteShieldedCommitment(uint64_t nIndex, const CPedersenCommitment& commit)
{
    return Write(make_pair(string("sc"), nIndex), commit);
}

bool CTxDB::ReadShieldedCommitment(uint64_t nIndex, CPedersenCommitment& commit)
{
    return Read(make_pair(string("sc"), nIndex), commit);
}

bool CTxDB::WriteShieldedCommitmentCount(uint64_t nCount)
{
    return Write(string("scc"), nCount);
}

bool CTxDB::ReadShieldedCommitmentCount(uint64_t& nCount)
{
    return Read(string("scc"), nCount);
}

bool CTxDB::WriteShieldedCommitmentHeight(uint64_t nIndex, int nHeight)
{
    return Write(make_pair(string("sch"), nIndex), nHeight);
}

bool CTxDB::ReadShieldedCommitmentHeight(uint64_t nIndex, int& nHeight)
{
    return Read(make_pair(string("sch"), nIndex), nHeight);
}

bool CTxDB::WriteShieldedCommitmentIndex(const std::vector<unsigned char>& vchCommitment, uint64_t nIndex)
{
    return Write(make_pair(string("sci"), vchCommitment), nIndex);
}

bool CTxDB::ReadShieldedCommitmentIndex(const std::vector<unsigned char>& vchCommitment, uint64_t& nIndex)
{
    return Read(make_pair(string("sci"), vchCommitment), nIndex);
}

bool CTxDB::ReadAllShieldedCommitments(std::vector<CPedersenCommitment>& vCommitments)
{
    vCommitments.clear();
    uint64_t nCount = 0;
    if (!ReadShieldedCommitmentCount(nCount))
        return false;
    vCommitments.reserve(nCount);
    for (uint64_t i = 0; i < nCount; i++)
    {
        CPedersenCommitment commit;
        if (ReadShieldedCommitment(i, commit))
            vCommitments.push_back(commit);
    }
    return true;
}

bool CTxDB::WriteCurveTree(const CCurveTree& tree)
{
    return Write(string("ct"), tree);
}

bool CTxDB::ReadCurveTree(CCurveTree& tree)
{
    return Read(string("ct"), tree);
}

bool CTxDB::WriteCurveTreeAtBlock(const uint256& blockHash, const CCurveTree& tree)
{
    return Write(make_pair(string("cb"), blockHash), tree);
}

bool CTxDB::ReadCurveTreeAtBlock(const uint256& blockHash, CCurveTree& tree)
{
    return Read(make_pair(string("cb"), blockHash), tree);
}

bool CTxDB::WriteCurveTreeAtEpoch(int nEpoch, const CCurveTree& tree)
{
    return Write(make_pair(string("ce"), nEpoch), tree);
}

bool CTxDB::ReadCurveTreeAtEpoch(int nEpoch, CCurveTree& tree)
{
    return Read(make_pair(string("ce"), nEpoch), tree);
}

bool CTxDB::EraseCurveTreeAtBlock(const uint256& blockHash)
{
    return Erase(make_pair(string("cb"), blockHash));
}

// Revocation is deliberately outside activeBatch: abort cannot resurrect trust.
// Failure to persist remains fail-closed in this process until a successful
// explicit rebuild. The durable key survives close/reopen and crashes.
static std::atomic<bool> dagChildCountRevocationWriteFailed(false);
bool g_testFailDAGChildCountRevocation = false;
bool g_testFailDAGChildCountRebuild = false;
static std::string ChildCountRevocationKey()
{
    CDataStream key(SER_DISK, CLIENT_VERSION);
    key << make_pair(std::string("dagchildcountinvalid"), uint8_t(0));
    return key.str();
}
static bool ChildCountRevoked(leveldb::DB* db)
{
    if (dagChildCountRevocationWriteFailed || !db) return true;
    std::string value;
    const leveldb::Status status = db->Get(leveldb::ReadOptions(), ChildCountRevocationKey(), &value);
    return !status.IsNotFound(); // IO error is unavailable, never healthy.
}
static bool RevokeChildCount(leveldb::DB* db)
{
    leveldb::WriteOptions options; options.sync = true;
    if (!db || g_testFailDAGChildCountRevocation ||
        !db->Put(options, ChildCountRevocationKey(), "rebuild-required").ok()) {
        dagChildCountRevocationWriteFailed = true;
        return false;
    }
    return true;
}

// R2c.2s: mutable DAG score authority certificate. Mirrors the child-count
// lifecycle: a durable revocation poison asserts REBUILD_REQUIRED and a
// versioned state marker binds the retained canonical score set to one
// SourceStateId. A marker/token alone never certifies score freshness; only a
// matching supported marker bound to the current token is healthy. Publishing
// the marker is atomic with clearing the revocation poison.
static const std::string SCORE_STATE_KEY = std::string("dagscorestate");
static const std::string SCORE_INVALID_KEY = std::string("dagscoreinvalid");
static std::atomic<bool> dagScoreRevocationWriteFailed(false);
bool g_testFailDAGScoreRevocation = false;
static std::string ScoreAuthorityRevocationKey()
{
    CDataStream key(SER_DISK, CLIENT_VERSION);
    key << make_pair(SCORE_INVALID_KEY, uint8_t(0));
    return key.str();
}
static bool ScoreAuthorityRevoked(leveldb::DB* db)
{
    if (dagScoreRevocationWriteFailed || !db) return true;
    std::string value;
    const leveldb::Status status = db->Get(leveldb::ReadOptions(), ScoreAuthorityRevocationKey(), &value);
    return !status.IsNotFound(); // IO error is unavailable, never healthy.
}
static bool RevokeScoreAuthority(leveldb::DB* db)
{
    leveldb::WriteOptions options; options.sync = true;
    if (!db || g_testFailDAGScoreRevocation ||
        !db->Put(options, ScoreAuthorityRevocationKey(), "rebuild-required").ok()) {
        dagScoreRevocationWriteFailed = true;
        return false;
    }
    return true;
}

// IDAG Phase 2: DAG link persistence. vDAGParents is canonical; the
// child-count projection is updated in this same active WriteBatch.
bool CTxDB::ReadDAGLinks(const uint256& hash, CBlockDAGData& data)
{
    return Read(make_pair(string("daglinks"), hash), data);
}

// Bounded keyed source authority, never a resident DAG or overlay query.
bool CTxDB::ReadDAGFrontierMembership(const uint256& hash, bool* member)
{
    if (!member) return false;
    *member = false;
    // Generic Exists falls through to disk after a staged tombstone. Frontier
    // postimages must honor the active batch's canonical deletion instead.
    CDataStream key(SER_DISK, CLIENT_VERSION);
    key << make_pair(string("daglinks"), hash);
    std::string raw; bool deleted = false;
    if (activeBatch && ScanBatch(key, &raw, &deleted) && deleted) return true;
    CBlockDAGData data;
    if (!ReadDAGLinks(hash, data))
        return !Exists(make_pair(string("daglinks"), hash));
    uint64_t count = 0; bool present = false;
    if (!ReadDAGChildCount(hash, &count, &present)) return false;
    *member = count == 0;
    return true;
}

// G6 authoritative row-level attestation. Same membership postimage as
// ReadDAGFrontierMembership, but canonical ROW PRESENCE is attested
// separately so an authoritative enumeration can distinguish a legitimate
// non-member (row present, has children) from an enumerated frontier tip whose
// canonical row is MISSING (incomplete canonical source => fail closed; never
// a silently reduced VALID vector). Additive: the legacy reader is unchanged
// for S5 preview / delta capture consumers.
bool CTxDB::ReadDAGFrontierMembershipAttested(const uint256& hash, bool* member, bool* rowPresent)
{
    if (!member || !rowPresent) return false;
    *member = false;
    *rowPresent = false;
    CDataStream key(SER_DISK, CLIENT_VERSION);
    key << make_pair(string("daglinks"), hash);
    // Active batch first, exactly like ReadDAGFrontierMembership: a staged
    // tombstone is the canonical postimage even while the durable row exists
    // on disk. A staged write is a row-present postimage whose child-count
    // projection is not yet sealed, so it can never be attested as a tip.
    if (activeBatch) {
        std::string staged; bool deleted = false;
        bool batchOpen = false;
        try { batchOpen = ScanBatch(key, &staged, &deleted); }
        catch (const std::exception&) { return false; } // fail closed on batch scan error
        if (deleted) return true;                       // staged tombstone: *rowPresent = false
        if (batchOpen) { *rowPresent = true; return true; } // staged write: present, not a tip
    }
    std::string raw;
    const leveldb::Status status = GetInstance()->Get(leveldb::ReadOptions(), key.str(), &raw);
    if (status.IsNotFound()) return true;               // canonical row absent: *rowPresent = false
    if (!status.ok()) return false;                     // IO failure: fail closed
    {
        CBlockDAGData data;
        try {
            CDataStream ssValue(raw.data(), raw.data() + raw.size(), SER_DISK, CLIENT_VERSION);
            ssValue >> data;
        } catch (const std::exception&) { return false; } // malformed row: fail closed
    }
    uint64_t count = 0; bool present = false;
    // Revoked/unreadable child-count projection must fail the attestation, not
    // degrade into "not a member".
    if (!ReadDAGChildCount(hash, &count, &present)) return false;
    *rowPresent = true;
    *member = (count == 0);
    return true;
}

namespace {
// Lifetime is ONE keyed relation mutation: O(unique old/new parents), not
// O(history) or O(transaction). Records stream to the existing bounded journal.
class SourceFrontierChange {
    CTxDB& db;
    std::map<uint256, bool> before;
    bool recording, complete;
public:
    explicit SourceFrontierChange(CTxDB& source) : db(source),
        recording(GetDagTipDeltaState().active), complete(!recording) {
        if (!recording) return;
        uint256 token; std::string error;
        if (!db.IsDAGChildCountIndexHealthy(&error) || !db.ReadDAGSourceStateId(token)) {
            InvalidateDagTipDeltaTransaction(); recording = false; return;
        }
        SetDagTipDeltaInitialSourceStateId(token);
    }
    ~SourceFrontierChange() { if (!complete) InvalidateDagTipDeltaTransaction(); }
    void Capture(const uint256& hash) {
        if (!recording || before.count(hash)) return;
        bool member = false;
        if (!db.ReadDAGFrontierMembership(hash, &member)) {
            InvalidateDagTipDeltaTransaction(); recording = false; return;
        }
        before[hash] = member;
    }
    void Finish() {
        if (!recording) return;
        for (const auto& entry : before) {
            bool member = false;
            if (!db.ReadDAGFrontierMembership(entry.first, &member)) return;
            if (member != entry.second)
                AppendDagTipDelta(DagTipDeltaRecord(member ? DagTipDeltaRecord::TIP_ADD :
                    DagTipDeltaRecord::TIP_REMOVE, entry.first));
        }
        complete = true;
    }
};
}

// R3 / C6 section 6 helpers (defined with the certificate machinery below; declared
// here because the provenance transitions above need the canonical row serialization).
static std::string SerializeRowPayload(const CBlockDAGData& data);

bool CTxDB::WriteDAGLinks(const uint256& hash, const CBlockDAGData& data)
{
    if (!activeBatch || ChildCountRevoked(GetInstance())) return false;
    CBlockDAGData old;
    const bool existed = ReadDAGLinks(hash, old);
    if (!existed && Exists(make_pair(string("daglinks"), hash))) return false;
    const std::set<uint256> before(old.vDAGParents.begin(), old.vDAGParents.end());
    const std::set<uint256> after(data.vDAGParents.begin(), data.vDAGParents.end());
    SourceFrontierChange delta(*this);
    delta.Capture(hash);
    for (const auto& p : before) if (!after.count(p)) delta.Capture(p);
    for (const auto& p : after) if (!before.count(p)) delta.Capture(p);
    if (ChildCountRevoked(GetInstance())) return false;
    for (std::set<uint256>::const_iterator p = before.begin(); p != before.end(); ++p)
        if (!after.count(*p)) {
            uint64_t count = 0; bool present = false;
            if (!ReadDAGChildCount(*p, &count, &present)) return false;
            if (!present || count == 0) { RevokeChildCount(GetInstance()); return false; }
            if (count == 1) { if (!Erase(make_pair(string("dagchildcount"), *p))) return false; }
            else if (!Write(make_pair(string("dagchildcount"), *p), count - 1)) return false;
        }
    for (std::set<uint256>::const_iterator p = after.begin(); p != after.end(); ++p)
        if (!before.count(*p)) {
            uint64_t count = 0; bool present = false;
            if (!ReadDAGChildCount(*p, &count, &present) || count == UINT64_MAX) return false;
            if (!Write(make_pair(string("dagchildcount"), *p), count + 1)) return false;
        }
    // R3 / C6 section 4 — RESTORE INVALIDATION (property 6-B), in this SAME batch
    // and BEFORE the row payload is published. A supported materialization
    // establishes the row's incarnation (first materialization -> 1, every later
    // rematerialization -> N+1) and invalidates/supersedes every prior prune event
    // of this vertex, so no crash point can leave the restored row explained by a
    // dead PRUNE(X, N) record. Evidence invalidation precedes publication.
    uint64_t priorIncarnation = 0; bool haveIncarnation = false;
    uint64_t newIncarnation = 1;
    {
        if (!ReadDAGRowIncarnation(hash, &priorIncarnation, &haveIncarnation)) return false;
        newIncarnation = haveIncarnation ? priorIncarnation + 1 : 1;
        if (haveIncarnation && newIncarnation <= priorIncarnation) return false; // overflow: fail closed
        uint64_t latestEvent = 0, latestIncarnation = 0; bool haveLatest = false;
        std::vector<uint64_t> rowEvents;
        if (!ReadDAGPruneLatest(hash, &latestEvent, &latestIncarnation, &rowEvents, &haveLatest)) return false;
        if (haveLatest)
        {
            for (size_t i = 0; i < rowEvents.size(); ++i)
            {
                DAGPruneEvent ev; bool present = false;
                if (!ReadDAGPruneEvent(rowEvents[i], &ev, &present)) return false;
                if (!present) continue;                  // history not retained: nothing to supersede
                if (ev.hash != hash) return false;        // index/event disagreement: fail closed
                if (ev.superseded_by == 0)
                {
                    ev.superseded_by = newIncarnation;    // inadmissible from this incarnation on
                    if (!WriteDAGPruneEvent(rowEvents[i], ev)) return false;
                }
            }
            if (!EraseDAGPruneLatest(hash)) return false;
        }
        if (!WriteDAGRowIncarnation(hash, newIncarnation)) return false;
        uint64_t provCounter = 0;
        if (!ReadDAGProvenanceCounter(&provCounter)) return false;
        if (!Write(string("dagprovcounter"), provCounter + 1)) return false;
    }
    if (!Write(make_pair(string("daglinks"), hash), data)) return false;
    // R3 / C6 section 6: a supported materialization is a provenance mutation, so the
    // coverage certificate is re-published IN THIS SAME BATCH (no-op when the store is
    // not certified: a mutation may never fabricate a certificate).
    if (!RepublishDAGProvenanceCertificateOnWrite(hash, SerializeRowPayload(data),
                                                 existed ? SerializeRowPayload(old) : std::string(),
                                                 existed, haveIncarnation,
                                                 priorIncarnation, newIncarnation))
        return false;
    delta.Finish();
    return true;
}

bool CTxDB::EraseDAGLinks(const uint256& hash, DAGRowEraseOrigin origin)
{
    return EraseDAGLinks(hash, origin, DAGROW_HEIGHT_UNKNOWN, DAGROW_HEIGHT_UNKNOWN);
}

bool CTxDB::EraseDAGLinks(const uint256& hash, DAGRowEraseOrigin origin, int32_t rowHeight,
                          int32_t pruneFloorAfter)
{
    if (!activeBatch || ChildCountRevoked(GetInstance())) return false;
    CBlockDAGData old;
    if (!ReadDAGLinks(hash, old))
        // Nothing was materialized here: this erase removes no canonical row and
        // therefore attributes NOTHING. In particular it must not register a
        // non-prune de-materialization for a row another lifecycle already erased,
        // and must not clear a record it did not supersede.
        return !Exists(make_pair(string("daglinks"), hash));
    const std::set<uint256> parents(old.vDAGParents.begin(), old.vDAGParents.end());
    SourceFrontierChange delta(*this);
    delta.Capture(hash);
    for (const auto& p : parents) delta.Capture(p);
    if (ChildCountRevoked(GetInstance())) return false;
    for (std::set<uint256>::const_iterator p = parents.begin(); p != parents.end(); ++p) {
        uint64_t count = 0; bool present = false;
        if (!ReadDAGChildCount(*p, &count, &present)) return false;
        if (!present || count == 0) { RevokeChildCount(GetInstance()); return false; }
        if (count == 1) { if (!Erase(make_pair(string("dagchildcount"), *p))) return false; }
        else if (!Write(make_pair(string("dagchildcount"), *p), count - 1)) return false;
    }
    if (!Erase(make_pair(string("daglinks"), hash))) return false;
    // F2-B1-R: bind THIS row's absence to the lifecycle that caused it, inside the
    // SAME atomic batch as the erase (same function, same activeBatch), so no
    // crash can leave the absence without its provenance or the provenance
    // without the absence.
    if (origin == DAGRowEraseOrigin::PRUNE)
    {
        // The row WAS materialized and is now erased by the accepted prune
        // lifecycle: the absence is genuinely prune-attributed, so any stale
        // non-prune de-materialization record for this vertex is superseded.
        // Conditional, because an unconditional Delete would write a tombstone for
        // every pruned row although the record is almost never present.
        // R3 / C6 section 3-4 (retirement): the legacy row-erase marker convention is
        // RETIRED as evidence in this same delta as the final predicate. Nothing reads
        // it any more (the predicate attributes absence only from bound positive prune
        // evidence), so this origin no longer writes or clears a marker record. The
        // record API is retained only so historical stores and existing callers keep
        // working; those records are inert.
        (void)origin;
        // R3 / C6 section 3 — POSITIVE PRUNE EVIDENCE, in this same atomic batch as
        // the erase: a monotone, hash-chained journal entry binding (X, N, epoch,
        // height, floor_after) plus the per-row index binding {E, N}. This replaces
        // the prune-writes-nothing convention. A prune that cannot bind a COMPLETE
        // identity records nothing at all (fail closed) — partial evidence is never
        // written, because partial evidence is exactly what could be mistaken for a
        // protocol prune later.
        if (rowHeight != DAGROW_HEIGHT_UNKNOWN && pruneFloorAfter != DAGROW_HEIGHT_UNKNOWN)
        {
            uint64_t incarnation = 0; bool haveIncarnation = false;
            if (!ReadDAGRowIncarnation(hash, &incarnation, &haveIncarnation)) return false;
            if (haveIncarnation)
            {
                uint64_t head = 0, length = 0; uint256 headHash;
                if (!ReadDAGPruneJournal(&head, &length, &headHash)) return false;
                const uint64_t event = head + 1;
                if (event == 0) return false;                 // event identity overflow: fail closed
                uint64_t epoch = 0;
                if (!ReadDAGCustodyEpoch(&epoch)) return false;
                DAGPruneEvent ev;
                ev.hash = hash;
                ev.incarnation = incarnation;
                ev.epoch = epoch;
                ev.height = rowHeight;
                ev.floor_after = pruneFloorAfter;
                ev.prev_event_hash = headHash;
                ev.superseded_by = 0;
                if (!WriteDAGPruneEvent(event, ev)) return false;
                CDataStream hs(SER_GETHASH, CLIENT_VERSION);
                hs << event << ev.hash << ev.incarnation << ev.epoch << ev.height
                   << ev.floor_after << ev.prev_event_hash;
                const uint256 eventHash = Hash(hs.begin(), hs.end());
                CDataStream js(SER_DISK, CLIENT_VERSION);
                js << event << (length + 1) << eventHash;
                if (!Write(string("dagprunejournal"), js.str())) return false;
                std::vector<uint64_t> rowEvents;
                rowEvents.push_back(event);               // events of the current incarnation epoch
                if (!WriteDAGPruneLatest(hash, event, incarnation, rowEvents)) return false;
                uint64_t provCounter = 0;
                if (!ReadDAGProvenanceCounter(&provCounter)) return false;
                if (!Write(string("dagprovcounter"), provCounter + 1)) return false;
            }
            // else: the row had no durable incarnation (legacy or unsupported
            // materialization). Positive attribution is impossible, so none is
            // recorded and this absence stays unexplained (C6 migration rule).
        }
        // else: the caller could not supply the complete prune identity. No event.
    }
    else
    {
        // A NON-prune lifecycle erased this row. R3 / C6 (retirement): no marker is
        // written any more — a non-prune de-materialization has NO admissible evidence
        // by construction (it produces no bound prune event for the current
        // incarnation), so under the final predicate its absence is
        // ROW_MISSING_UNEXPLAINED and fails closed. The row's incarnation is
        // deliberately NOT advanced (nothing was rematerialized), but this is still a
        // provenance-relevant custody mutation, so the mutation counter advances.
        uint64_t provCounter = 0;
        if (!ReadDAGProvenanceCounter(&provCounter)) return false;
        if (!Write(string("dagprovcounter"), provCounter + 1)) return false;
    }
    // R3 / C6 section 6: the erased row left the covered set, so the coverage
    // certificate is re-published in this same batch (no-op for an uncertified store).
    if (!RepublishDAGProvenanceCertificateOnErase(hash, SerializeRowPayload(old))) return false;
    delta.Finish();
    return true;
}

// R3 / C6 continuity substrate (section 5). READ-ONLY: the engine's monotone
// write-sequence watermark, reached through the vendored engine's existing
// property dispatch. Failure to provide it is a fail-closed result, never a
// default: coverage must not survive a session boundary without it.
bool CTxDB::ReadEngineLastSequence(uint64_t* sequence, std::string* detail) const
{
    if (!sequence) return false;
    *sequence = 0;
    if (!pdb)
    {
        if (detail) *detail = "engine watermark unavailable: no DB instance";
        return false;
    }
    // R3.9 case M: deterministic simulation of a backend WITHOUT the read-only accessor.
    // The suppression only makes the capability unavailable (it can never make an
    // unavailable capability look available), so fail-closed semantics are strengthened,
    // never weakened.
    if (g_testSuppressDagCustodyWatermark)
    {
        if (detail) *detail = "engine watermark unavailable: accessor not provided by this backend (simulated)";
        return false;
    }
    std::string value;
    if (!pdb->GetProperty("leveldb.last-sequence", &value) || value.empty())
    {
        // Legacy/foreign backend without the read-only accessor: the conservative
        // fallback applies (only PRUNE evidence recorded in the current custody
        // epoch is admissible; coverage does not survive a session boundary).
        if (detail) *detail = "engine watermark unavailable: accessor not provided by this backend";
        return false;
    }
    uint64_t parsed = 0;
    bool anyDigit = false;
    for (size_t i = 0; i < value.size(); ++i)
    {
        const char c = value[i];
        if (c < '0' || c > '9')
        {
            if (detail) *detail = "engine watermark malformed: " + value;
            return false;
        }
        if (parsed > (UINT64_MAX - (uint64_t)(c - '0')) / 10)
        {
            if (detail) *detail = "engine watermark overflow: " + value;
            return false;
        }
        parsed = parsed * 10 + (uint64_t)(c - '0');
        anyDigit = true;
    }
    if (!anyDigit)
    {
        if (detail) *detail = "engine watermark malformed: empty value";
        return false;
    }
    *sequence = parsed;
    if (detail) detail->clear();
    return true;
}

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
bool CTxDB::ReadDAGLinksTyped(const uint256& hash, CBlockDAGData* data,
                              DAGRowTypedOutcome* outcome, std::string* detail)
{
    if (!outcome) return false;
    *outcome = DAGRowTypedOutcome::STORAGE_ERROR;
    if (detail) detail->clear();
    if (data) *data = CBlockDAGData();
    if (!pdb)
    {
        if (detail) *detail = "typed row read: no DB instance";
        return true;
    }
    CDataStream ssKey(SER_DISK, CLIENT_VERSION);
    ssKey << make_pair(string("daglinks"), hash);
    std::string raw;
    bool fromDb = true;
    if (activeBatch)
    {
        // ScanBatch THROWS on a batch-iterate failure (it cannot report one
        // otherwise). An iterator/status failure is a storage error, never an
        // absence: classify it, do not let it escape as an absence and do not
        // silently fall back to the committed store.
        try
        {
            bool deleted = false;
            fromDb = (ScanBatch(ssKey, &raw, &deleted) == false);
            if (deleted)
            {
                *outcome = DAGRowTypedOutcome::ROW_MISSING_UNEXPLAINED;
                return true;
            }
        }
        catch (const std::exception& e)
        {
            *outcome = DAGRowTypedOutcome::STORAGE_ERROR;
            if (detail) *detail = std::string("typed row read: staged-view scan failed: ") + e.what();
            return true;
        }
    }
    if (fromDb)
    {
        const leveldb::Status status = pdb->Get(leveldb::ReadOptions(), ssKey.str(), &raw);
        if (status.IsNotFound())
        {
            *outcome = DAGRowTypedOutcome::ROW_MISSING_UNEXPLAINED;
            return true;
        }
        if (!status.ok())
        {
            *outcome = DAGRowTypedOutcome::STORAGE_ERROR;
            if (detail) *detail = "typed row read: leveldb status: " + status.ToString();
            return true;
        }
    }
    // Present: it MUST decode. A present-but-undecodable row is CORRUPT, never
    // missing, and never pruned.
    try
    {
        CDataStream ssValue(raw.data(), raw.data() + raw.size(), SER_DISK, CLIENT_VERSION);
        CBlockDAGData parsed;
        ssValue >> parsed;
        if (!ssValue.empty())
        {
            *outcome = DAGRowTypedOutcome::ROW_CORRUPT;
            if (detail) *detail = "typed row read: trailing bytes after row payload";
            return true;
        }
        if (data) *data = parsed;
        *outcome = DAGRowTypedOutcome::ROW_PRESENT_VALID;
    }
    catch (const std::exception& e)
    {
        *outcome = DAGRowTypedOutcome::ROW_CORRUPT;
        if (detail) *detail = std::string("typed row read: malformed row payload: ") + e.what();
    }
    return true;
}

bool CTxDB::ReadDAGRowIncarnation(const uint256& hash, uint64_t* incarnation, bool* present)
{
    if (!incarnation || !present) return false;
    *incarnation = 0; *present = false;
    const std::pair<std::string, uint256> key = make_pair(string("dagrowinc"), hash);
    if (!Read(key, *incarnation))
        return !Exists(key);   // present but unreadable -> fail closed
    *present = true;
    return true;
}

bool CTxDB::WriteDAGRowIncarnation(const uint256& hash, uint64_t incarnation)
{
    return Write(make_pair(string("dagrowinc"), hash), incarnation);
}

bool CTxDB::ReadDAGPruneLatest(const uint256& hash, uint64_t* event, uint64_t* incarnation,
                               std::vector<uint64_t>* events, bool* present)
{
    if (!event || !incarnation || !present) return false;
    *event = 0; *incarnation = 0; *present = false;
    if (events) events->clear();
    const std::pair<std::string, uint256> key = make_pair(string("dagprunelatest"), hash);
    std::string raw;
    if (!Read(key, raw))
        return !Exists(key);   // present but unreadable -> fail closed
    try
    {
        CDataStream ss(raw.data(), raw.data() + raw.size(), SER_DISK, CLIENT_VERSION);
        std::vector<uint64_t> evs;
        ss >> *event >> *incarnation >> evs;
        if (!ss.empty()) return false;
        if (events) *events = evs;
        *present = true;
        return true;
    }
    catch (const std::exception&) { return false; }
}

bool CTxDB::WriteDAGPruneLatest(const uint256& hash, uint64_t event, uint64_t incarnation,
                                const std::vector<uint64_t>& events)
{
    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << event << incarnation << events;
    return Write(make_pair(string("dagprunelatest"), hash), ss.str());
}

bool CTxDB::EraseDAGPruneLatest(const uint256& hash)
{
    return Erase(make_pair(string("dagprunelatest"), hash));
}

bool CTxDB::ReadDAGPruneEvent(uint64_t event, DAGPruneEvent* out, bool* present)
{
    if (!out || !present) return false;
    *out = DAGPruneEvent(); *present = false;
    const std::pair<std::string, uint64_t> key = make_pair(string("dagprunevent"), event);
    std::string raw;
    if (!Read(key, raw))
        return !Exists(key);   // present but unreadable -> fail closed
    try
    {
        CDataStream ss(raw.data(), raw.data() + raw.size(), SER_DISK, CLIENT_VERSION);
        ss >> out->hash >> out->incarnation >> out->epoch >> out->height >> out->floor_after
           >> out->prev_event_hash >> out->superseded_by;
        if (!ss.empty()) { *out = DAGPruneEvent(); return false; }
    }
    catch (const std::exception&) { *out = DAGPruneEvent(); return false; }
    *present = true;
    return true;
}

bool CTxDB::WriteDAGPruneEvent(uint64_t event, const DAGPruneEvent& ev)
{
    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << ev.hash << ev.incarnation << ev.epoch << ev.height << ev.floor_after
       << ev.prev_event_hash << ev.superseded_by;
    return Write(make_pair(string("dagprunevent"), event), ss.str());
}

bool CTxDB::ReadDAGPruneJournal(uint64_t* head, uint64_t* length, uint256* head_hash)
{
    if (!head || !length || !head_hash) return false;
    *head = 0; *length = 0; *head_hash = 0;
    const std::string key = string("dagprunejournal");
    std::string raw;
    if (!Read(key, raw))
        return !Exists(key);   // present but unreadable -> fail closed
    try
    {
        CDataStream ss(raw.data(), raw.data() + raw.size(), SER_DISK, CLIENT_VERSION);
        ss >> *head >> *length >> *head_hash;
        return ss.empty();
    }
    catch (const std::exception&) { return false; }
}

bool CTxDB::ReadDAGProvenanceCounter(uint64_t* counter)
{
    if (!counter) return false;
    *counter = 0;
    const std::string key = string("dagprovcounter");
    if (!Read(key, *counter))
        return !Exists(key);   // absent = counter 0 (never started); corrupt -> fail closed
    return true;
}

bool CTxDB::ReadDAGCustodyEpoch(uint64_t* epoch)
{
    if (!epoch) return false;
    // 0 = no custody epoch established yet. Events recorded in epoch 0 are not
    // admissible for positive attribution (C6 section 5 migration rule), so this
    // is a fail-closed default, never a fabricated epoch identity.
    *epoch = 0;
    const std::string key = string("dagcustodyepoch");
    if (!Read(key, *epoch))
        return !Exists(key);
    return true;
}

// ---------------------------------------------------------------------------
// R3 / C6 sections 5-6 — coverage certificate + custody seal.
//
// The certificate IS the accumulator: every digest is maintained incrementally by
// the supported writer in the SAME batch as the mutation it describes, and each is
// independently recomputable by a full scan of the committed store (the
// certification scan). "The certificate verifies" is therefore a recomputation,
// never a timestamp, a floor advancement or a final-state digest alone.
// ---------------------------------------------------------------------------
static DAGCustodyState g_dagCustodyState = DAGCustodyState::UNAVAILABLE;
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

static uint256 HashFloorState(int32_t floorValue, int32_t cleanHeight)
{
    CDataStream s(SER_GETHASH, CLIENT_VERSION);
    s << (unsigned char)'F' << floorValue << cleanHeight;
    return Hash(s.begin(), s.end());
}

static std::string SerializeRowPayload(const CBlockDAGData& data)
{
    CDataStream s(SER_DISK, CLIENT_VERSION);
    s << data;
    return s.str();
}

static std::string SerializeDAGProvenanceCertificate(const DAGProvenanceCertificate& c)
{
    CDataStream s(SER_DISK, CLIENT_VERSION);
    s << c.version << c.capabilityVersion << c.storeInstanceId << c.epoch << c.hCert
      << c.watermark << c.mutationCounter << c.coveredVertexCount << c.coveredRowDigest
      << c.perRowIncarnationDigest << c.knownVertexDigest << c.floorStateDigest
      << c.journalHeadHash << c.journalLength << c.floorValue << c.cleanHeightValue;
    return s.str();
}

static bool DeserializeDAGProvenanceCertificate(const std::string& raw, DAGProvenanceCertificate* out)
{
    if (!out) return false;
    try
    {
        CDataStream s(raw.data(), raw.data() + raw.size(), SER_DISK, CLIENT_VERSION);
        s >> out->version >> out->capabilityVersion >> out->storeInstanceId >> out->epoch
          >> out->hCert >> out->watermark >> out->mutationCounter >> out->coveredVertexCount
          >> out->coveredRowDigest >> out->perRowIncarnationDigest >> out->knownVertexDigest
          >> out->floorStateDigest >> out->journalHeadHash >> out->journalLength
          >> out->floorValue >> out->cleanHeightValue;
        return s.empty();
    }
    catch (const std::exception&) { return false; }
}

static std::string SerializeDAGCustodySeal(const DAGCustodySeal& s)
{
    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << s.watermark << s.epoch << s.counter;
    return ss.str();
}

static bool DeserializeDAGCustodySeal(const std::string& raw, DAGCustodySeal* out)
{
    if (!out) return false;
    try
    {
        CDataStream s(raw.data(), raw.data() + raw.size(), SER_DISK, CLIENT_VERSION);
        s >> out->watermark >> out->epoch >> out->counter;
        return s.empty();
    }
    catch (const std::exception&) { return false; }
}

bool CTxDB::ReadDAGProvenanceCertificate(DAGProvenanceCertificate* cert, bool* present)
{
    if (!cert || !present) return false;
    *cert = DAGProvenanceCertificate();
    *present = false;
    const std::string key = string("dagcert");
    std::string raw;
    if (!Read(key, raw))
        return !Exists(key);   // present but unreadable -> fail closed
    if (!DeserializeDAGProvenanceCertificate(raw, cert)) return false;
    *present = true;
    return true;
}

bool CTxDB::ReadDAGCustodySeal(DAGCustodySeal* seal, bool* present)
{
    if (!seal || !present) return false;
    *seal = DAGCustodySeal();
    *present = false;
    const std::string key = string("dagcustodyseal");
    std::string raw;
    if (!Read(key, raw))
        return !Exists(key);
    if (!DeserializeDAGCustodySeal(raw, seal)) return false;
    *present = true;
    return true;
}

DAGCustodyState CTxDB::GetDAGCustodyState() const { return g_dagCustodyState; }
bool CTxDB::IsDAGProvenanceDeepVerified() const { return g_dagCertDeepVerified; }

bool CTxDB::ScanDAGProvenanceDigests(DAGProvenanceCertificate* out, std::string* error, bool enforceAdmission,
                                    uint64_t prospectiveEpoch)
{
    if (!out) return false;
    if (!pdb) { if (error) *error = "provenance scan: no DB instance"; return false; }
    uint256 covAcc = 0, incAcc = 0, vertAcc = 0;
    uint64_t vertCount = 0;
    // R3 certification-admission repair (audit 20260930): a certificate may not positively
    // certify a domain that contains an unexplained missing KNOWN vertex. The scan therefore
    // also collects the two key families so the semantic admission check below can run
    // non-circularly on durable provenance facts only.
    std::set<uint256> rowHashes, knownVertexHashes;
    leveldb::Iterator* it = pdb->NewIterator(leveldb::ReadOptions());
    if (it == NULL) { if (error) *error = "provenance scan: no iterator"; return false; }
    for (it->SeekToFirst(); it->Valid(); it->Next())
    {
        const leveldb::Slice k = it->key();
        // Keys are serialized pair<std::string,uint256> (prefix || 32-byte key) with no
        // extra marker; the prefix length is a CompactSize. Parse the prefix as a string
        // rather than assuming a byte offset, so a key of any other family (or a
        // malformed key) is simply skipped instead of mis-attributed.
        std::string prefix;
        uint256 h;
        bool parsed = false;
        try
        {
            CDataStream ssKey(SER_DISK, CLIENT_VERSION);
            ssKey.write(k.data(), k.size());
            ssKey >> prefix;
            if (prefix == "daglinks" || prefix == "dagrowinc")
            {
                ssKey >> h;
                parsed = true;
            }
        }
        catch (const std::exception&)
        {
            // A malformed key of a provenance family is a storage-integrity failure, not
            // something to skip silently.
            if (prefix == "daglinks" || prefix == "dagrowinc")
            {
                delete it;
                if (error) *error = "provenance scan: malformed provenance key";
                return false;
            }
        }
        if (parsed)
        {
            if (prefix == "daglinks")
            {
                // A PRESENT row must be well-formed: a corrupt payload is a storage-integrity
                // failure, never silently certified (frozen: ROW_CORRUPT must refuse).
                try
                {
                    CBlockDAGData parsedRow;
                    CDataStream rs(it->value().data(), it->value().data() + it->value().size(),
                                   SER_DISK, CLIENT_VERSION);
                    rs >> parsedRow;
                    if (!rs.empty()) { delete it; if (error) *error = "provenance scan: trailing bytes in daglinks payload"; return false; }
                }
                catch (const std::exception&)
                {
                    delete it;
                    if (error) *error = "provenance scan: corrupt daglinks payload (ROW_CORRUPT)";
                    return false;
                }
                rowHashes.insert(h);
                covAcc = XorUint256(covAcc, HashRowCoverage(h, it->value().ToString()));
            }
            else if (prefix == "dagrowinc")
            {
                uint64_t inc = 0;
                try
                {
                    CDataStream s(it->value().data(), it->value().data() + it->value().size(),
                                  SER_DISK, CLIENT_VERSION);
                    s >> inc;
                    if (!s.empty()) { delete it; if (error) *error = "provenance scan: trailing bytes in incarnation record"; return false; }
                }
                catch (const std::exception&)
                {
                    delete it;
                    if (error) *error = "provenance scan: malformed incarnation record";
                    return false;
                }
                knownVertexHashes.insert(h);
                incAcc = XorUint256(incAcc, HashIncarnation(h, inc));
                vertAcc = XorUint256(vertAcc, HashKnownVertex(h));
                ++vertCount;
            }
        }
    }
    const leveldb::Status st = it->status();
    delete it;
    if (!st.ok()) { if (error) *error = "provenance scan: iterator status: " + st.ToString(); return false; }

    // ---------------------------------------------------------------------------------
    // R3 coverage-certification ADMISSION (repair of the confirmed audit defect).
    //
    // For every KNOWN DAG vertex (a vertex with a durable dagrowinc record) inside the
    // proposed covered domain:
    //   * daglinks[X] present and well-formed  -> certification may continue (checked above);
    //   * daglinks[X] absent                  -> certification may continue ONLY when the
    //     absence is positively explained by the frozen provenance facts applicable at
    //     certification time: the per-row prune index must bind the CURRENT durable
    //     incarnation N, and the bound journal event must exist, be non-superseded, be
    //     bound to exactly (X, N), and lie inside the proposed domain.
    // Anything else is an UNEXPLAINED hole and certification MUST refuse.
    //
    // NON-CIRCULARITY: this check reads only durable provenance facts (daglinks / dagrowinc /
    // dagprunelatest / dagprunevent) plus the store's own domain bounds. It never consults the
    // certificate being created and never calls the certificate-dependent resolution
    // predicate, so there is no certificate<->prune circularity. Admitting an absence here
    // does NOT make it resolvable: per-row resolution separately applies the stricter
    // frozen rules (verified certificate, admissible custody epoch, journal/domain binding).
    // Deliberately NOT positive explanation: floor alone, the retired erase marker, timestamps,
    // final row digests, or mere incarnation existence.
    // ---------------------------------------------------------------------------------
    if (enforceAdmission)
    {
        int32_t admissionFloor = 0, admissionClean = 0;
        const bool admissionHasFloor = ReadDAGPruneFloor(admissionFloor);
        const bool admissionHasClean = ReadDAGCleanHeight(admissionClean);
        int32_t hDomain = 0;
        if (admissionHasFloor && admissionHasClean) hDomain = admissionFloor > admissionClean ? admissionFloor : admissionClean;
        else if (admissionHasFloor) hDomain = admissionFloor;
        else if (admissionHasClean) hDomain = admissionClean;
        for (std::set<uint256>::const_iterator vi = knownVertexHashes.begin(); vi != knownVertexHashes.end(); ++vi)
        {
            const uint256& vertex = *vi;
            if (rowHashes.count(vertex)) continue;   // present + well-formed
            uint64_t curInc = 0; bool incPresent = false;
            if (!ReadDAGRowIncarnation(vertex, &curInc, &incPresent))
            {
                if (error) *error = "provenance scan: incarnation read failed for " + vertex.GetHex();
                return false;
            }
            if (!incPresent) continue;               // raced/removed: nothing to explain
            uint64_t latestEvent = 0, latestInc = 0; bool latestPresent = false;
            std::vector<uint64_t> latestEvents;
            if (!ReadDAGPruneLatest(vertex, &latestEvent, &latestInc, &latestEvents, &latestPresent))
            {
                if (error) *error = "provenance scan: prune index read failed for " + vertex.GetHex();
                return false;
            }
            bool explained = false;
            if (latestPresent && latestInc == curInc)
            {
                DAGPruneEvent ev; bool evPresent = false;
                if (!ReadDAGPruneEvent(latestEvent, &ev, &evPresent))
                {
                    if (error) *error = "provenance scan: prune event read failed for " + vertex.GetHex();
                    return false;
                }
                if (evPresent && ev.hash == vertex && ev.incarnation == curInc && ev.superseded_by == 0 &&
                    ev.height != DAGROW_HEIGHT_UNKNOWN && (hDomain <= 0 || (ev.height >= 0 && ev.height < hDomain)) &&
                    ev.epoch == prospectiveEpoch)
                {
                    // The event is admissible under the epoch this transition is about to publish,
                    // so certification cannot cover a row the resolver will refuse.
                    explained = true;
                }
            }
            if (!explained)
            {
                if (error) *error = "provenance scan: unexplained missing known vertex " + vertex.GetHex() +
                                    " (no admissible positive prune explanation for the current incarnation " +
                                    std::to_string((unsigned long long)curInc) + " in the prospective certified custody epoch " +
                                    std::to_string((unsigned long long)prospectiveEpoch) + ") — certification refused";
                return false;
            }
        }
    }
    out->version = 1;
    out->capabilityVersion = 1;
    out->coveredRowDigest = covAcc;
    out->perRowIncarnationDigest = incAcc;
    out->knownVertexDigest = vertAcc;
    out->coveredVertexCount = vertCount;
    return true;
}

bool CTxDB::CertifyDAGProvenanceCoverage(int32_t hCert, uint64_t* epoch, std::string* error)
{
    if (activeBatch) { if (error) *error = "certification refused: an active batch is open"; return false; }
    // A suspended custody (watermark continuity lost) is NEVER re-certified into
    // admissibility: certification may establish a fresh session-scoped custody epoch, but
    // it may never launder a store whose continuity already failed.
    if (g_dagCustodyState == DAGCustodyState::SUSPENDED)
    {
        if (error) *error = "certification refused: custody is suspended (watermark continuity lost)";
        return false;
    }
    int32_t floorValue = 0, cleanHeight = 0;
    const bool haveFloor = ReadDAGPruneFloor(floorValue);
    const bool haveClean = ReadDAGCleanHeight(cleanHeight);
    if (!haveFloor && !haveClean)
    {
        // No floor and no clean height: there is no certifiable provenance domain.
        // Fail closed; never certify an inferred domain.
        if (error) *error = "certification refused: no prune floor and no clean height establish a certifiable domain";
        return false;
    }
    // EPOCH-SEAM REPAIR (confirmed re-audit defect 20260930-122133): the custody epoch this
    // certification transition is ABOUT TO publish must be known BEFORE admission. It is a value
    // determined by the transition itself (frozen selection: curEpoch == 0 ? 1 : curEpoch), not
    // authority derived from the certificate, so this is not circular. An absent known vertex may
    // only be admitted when its bound prune event's epoch equals this prospective certified epoch;
    // otherwise the event would be refused by the resolver immediately after certification, which
    // would let the certificate positively cover an effectively unexplained absence.
    uint64_t prospectiveCertifiedEpoch = 0;
    if (!ReadDAGCustodyEpoch(&prospectiveCertifiedEpoch)) { if (error) *error = "certification refused: custody epoch unreadable"; return false; }
    prospectiveCertifiedEpoch = (prospectiveCertifiedEpoch == 0) ? 1 : prospectiveCertifiedEpoch;
    DAGProvenanceCertificate cert;
    if (!ScanDAGProvenanceDigests(&cert, error, true, prospectiveCertifiedEpoch)) return false;
    // Durable store identity: created exactly once, never silently regenerated.
    uint256 storeId;
    bool haveId = false;
    {
        const std::string key = string("dagstoreid");
        std::string raw;
        if (Read(key, raw))
        {
            try
            {
                CDataStream s(raw.data(), raw.data() + raw.size(), SER_DISK, CLIENT_VERSION);
                s >> storeId;
                haveId = s.empty();
            }
            catch (const std::exception&) { haveId = false; }
            if (!haveId) { if (error) *error = "certification refused: store instance id unreadable"; return false; }
        }
        else if (Exists(key))
        {
            if (error) *error = "certification refused: store instance id unreadable";
            return false;
        }
        else
        {
            storeId = GetRandHash();
            haveId = false;   // written below, inside the publication batch
        }
    }
    const uint64_t newEpoch = prospectiveCertifiedEpoch;   // read and validated before admission
    int32_t hDomain = hCert;
    if (haveClean && cleanHeight > hDomain) hDomain = cleanHeight;
    if (haveFloor && floorValue > hDomain) hDomain = floorValue;
    uint64_t head = 0, length = 0; uint256 headHash;
    if (!ReadDAGPruneJournal(&head, &length, &headHash)) { if (error) *error = "certification refused: prune journal unreadable"; return false; }
    uint64_t counter = 0;
    if (!ReadDAGProvenanceCounter(&counter)) { if (error) *error = "certification refused: provenance counter unreadable"; return false; }
    uint64_t w = 0; std::string werr;
    const bool haveWatermark = ReadEngineLastSequence(&w, &werr);
    cert.version = 1;
    cert.capabilityVersion = 1;
    cert.storeInstanceId = storeId;
    cert.epoch = newEpoch;
    cert.hCert = hDomain;
    cert.watermark = haveWatermark ? w : 0;
    cert.mutationCounter = counter;
    cert.journalHeadHash = headHash;
    cert.journalLength = length;
    cert.floorValue = haveFloor ? floorValue : -1;
    cert.cleanHeightValue = haveClean ? cleanHeight : -1;
    cert.floorStateDigest = HashFloorState(cert.floorValue, cert.cleanHeightValue);
    if (!TxnBegin()) return false;
    if (!haveId)
    {
        CDataStream s(SER_DISK, CLIENT_VERSION);
        s << storeId;
        if (!Write(string("dagstoreid"), s.str())) { TxnAbort(); if (error) *error = "certification failed: store id write"; return false; }
    }
    if (!Write(string("dagcustodyepoch"), newEpoch)) { TxnAbort(); if (error) *error = "certification failed: epoch write"; return false; }
    if (!Write(string("dagcert"), SerializeDAGProvenanceCertificate(cert))) { TxnAbort(); if (error) *error = "certification failed: certificate write"; return false; }
    if (!TxnCommit()) { if (error) *error = "certification failed: commit"; return false; }
    g_dagCertDeepVerified = true;      // the scan was performed against the committed store
    // Certification IS the custody-establishing act: it binds the certificate, the epoch
    // and the certified domain to the durable state as it exists now. Custody therefore
    // becomes VERIFIED for this session (a clean shutdown seals W, so the next open
    // verifies exact cross-session continuity; a suspended store never reaches here).
    g_dagCustodyState = DAGCustodyState::VERIFIED;
    if (epoch) *epoch = newEpoch;
    return true;
}

bool CTxDB::VerifyDAGProvenanceCoverage(DAGCertVerifyResult* result, std::string* error)
{
    if (result) *result = DAGCertVerifyResult::OK;
    DAGProvenanceCertificate cert;
    bool haveCert = false;
    if (!ReadDAGProvenanceCertificate(&cert, &haveCert))
    {
        if (result) *result = DAGCertVerifyResult::STORAGE_ERROR;
        if (error) *error = "certificate unreadable";
        return false;
    }
    if (!haveCert) { if (result) *result = DAGCertVerifyResult::NO_CERTIFICATE; return true; }
    uint256 storeId;
    {
        const std::string key = string("dagstoreid");
        std::string raw;
        bool haveId = false;
        if (Read(key, raw))
        {
            try { CDataStream s(raw.data(), raw.data() + raw.size(), SER_DISK, CLIENT_VERSION); s >> storeId; haveId = s.empty(); }
            catch (const std::exception&) { haveId = false; }
        }
        else if (Exists(key)) { if (result) *result = DAGCertVerifyResult::STORAGE_ERROR; if (error) *error = "store id unreadable"; return false; }
        if (!haveId || storeId != cert.storeInstanceId)
        {
            if (result) *result = DAGCertVerifyResult::STORE_INSTANCE_MISMATCH;
            return true;
        }
    }
    uint64_t curEpoch = 0;
    if (!ReadDAGCustodyEpoch(&curEpoch)) { if (result) *result = DAGCertVerifyResult::STORAGE_ERROR; return false; }
    if (curEpoch == 0 || curEpoch != cert.epoch)
    {
        if (result) *result = DAGCertVerifyResult::EPOCH_MISMATCH;
        return true;
    }
    uint64_t head = 0, length = 0; uint256 headHash;
    if (!ReadDAGPruneJournal(&head, &length, &headHash)) { if (result) *result = DAGCertVerifyResult::STORAGE_ERROR; return false; }
    if (headHash != cert.journalHeadHash || length != cert.journalLength)
    {
        if (result) *result = DAGCertVerifyResult::JOURNAL_MISMATCH;
        return true;
    }
    DAGProvenanceCertificate scanned;
    if (!ScanDAGProvenanceDigests(&scanned, error, false))
    {
        if (result) *result = DAGCertVerifyResult::STORAGE_ERROR;
        return false;
    }
    int32_t floorValue = 0, cleanHeight = 0;
    const bool haveFloor = ReadDAGPruneFloor(floorValue);
    const bool haveClean = ReadDAGCleanHeight(cleanHeight);
    const uint256 floorDigest = HashFloorState(haveFloor ? floorValue : -1, haveClean ? cleanHeight : -1);
    if (scanned.coveredRowDigest != cert.coveredRowDigest ||
        scanned.perRowIncarnationDigest != cert.perRowIncarnationDigest ||
        scanned.knownVertexDigest != cert.knownVertexDigest ||
        scanned.coveredVertexCount != cert.coveredVertexCount ||
        floorDigest != cert.floorStateDigest)
    {
        if (result) *result = DAGCertVerifyResult::DIGEST_MISMATCH;
        if (error)
            *error = "certification scan mismatch: the committed store no longer reproduces the certified digests"
                     " (an unexplained hole, or a hole produced by an unsupported writer)";
        return true;
    }
    g_dagCertDeepVerified = true;
    if (result) *result = DAGCertVerifyResult::OK;
    return true;
}

bool CTxDB::VerifyDAGProvenanceCoverageShallow(DAGCertVerifyResult* result, std::string* error)
{
    if (result) *result = DAGCertVerifyResult::OK;
    DAGProvenanceCertificate cert;
    bool haveCert = false;
    if (!ReadDAGProvenanceCertificate(&cert, &haveCert))
    {
        if (result) *result = DAGCertVerifyResult::STORAGE_ERROR;
        if (error) *error = "certificate unreadable";
        return false;
    }
    if (!haveCert) { if (result) *result = DAGCertVerifyResult::NO_CERTIFICATE; return true; }
    if (cert.version != 1 || cert.capabilityVersion != 1)
    {
        if (result) *result = DAGCertVerifyResult::DIGEST_MISMATCH;
        if (error) *error = "certificate layout/capability version mismatch";
        return true;
    }
    uint64_t curEpoch = 0;
    if (!ReadDAGCustodyEpoch(&curEpoch)) { if (result) *result = DAGCertVerifyResult::STORAGE_ERROR; return false; }
    if (curEpoch == 0 || curEpoch != cert.epoch) { if (result) *result = DAGCertVerifyResult::EPOCH_MISMATCH; return true; }
    uint64_t head = 0, length = 0; uint256 headHash;
    if (!ReadDAGPruneJournal(&head, &length, &headHash)) { if (result) *result = DAGCertVerifyResult::STORAGE_ERROR; return false; }
    if (headHash != cert.journalHeadHash || length != cert.journalLength)
    {
        if (result) *result = DAGCertVerifyResult::JOURNAL_MISMATCH;
        return true;
    }
    uint64_t counter = 0;
    if (!ReadDAGProvenanceCounter(&counter)) { if (result) *result = DAGCertVerifyResult::STORAGE_ERROR; return false; }
    if (counter != cert.mutationCounter)
    {
        if (result) *result = DAGCertVerifyResult::DIGEST_MISMATCH;
        if (error) *error = "certificate is stale: the provenance mutation counter has advanced without republication";
        return true;
    }
    if (result) *result = DAGCertVerifyResult::OK;
    return true;
}

bool CTxDB::RepublishDAGProvenanceCertificateOnWrite(const uint256& hash,
                                                    const std::string& newPayload,
                                                    const std::string& oldPayload,
                                                    bool fRowExisted, bool hadIncarnation,
                                                    uint64_t incBefore, uint64_t incAfter)
{
    DAGProvenanceCertificate cert;
    bool haveCert = false;
    if (!ReadDAGProvenanceCertificate(&cert, &haveCert)) return false;
    if (!haveCert) return true;   // uncertified store: no mutation may fabricate a certificate
    if (fRowExisted) cert.coveredRowDigest = XorUint256(cert.coveredRowDigest, HashRowCoverage(hash, oldPayload));
    cert.coveredRowDigest = XorUint256(cert.coveredRowDigest, HashRowCoverage(hash, newPayload));
    if (!hadIncarnation)
    {
        cert.knownVertexDigest = XorUint256(cert.knownVertexDigest, HashKnownVertex(hash));
        cert.coveredVertexCount += 1;
    }
    else
    {
        cert.perRowIncarnationDigest = XorUint256(cert.perRowIncarnationDigest, HashIncarnation(hash, incBefore));
    }
    cert.perRowIncarnationDigest = XorUint256(cert.perRowIncarnationDigest, HashIncarnation(hash, incAfter));
    return RepublishDAGProvenanceCertificateCommon(&cert);
}

bool CTxDB::RepublishDAGProvenanceCertificateOnErase(const uint256& hash, const std::string& oldPayload)
{
    DAGProvenanceCertificate cert;
    bool haveCert = false;
    if (!ReadDAGProvenanceCertificate(&cert, &haveCert)) return false;
    if (!haveCert) return true;
    cert.coveredRowDigest = XorUint256(cert.coveredRowDigest, HashRowCoverage(hash, oldPayload));
    return RepublishDAGProvenanceCertificateCommon(&cert);
}

// R3.6: a floor / clean-height change is part of the certified coverage state, so it
// re-binds the floor-state digest and re-publishes the certificate in the ACTIVE batch.
// A store that is not certified is never certified by this side effect.
bool CTxDB::RepublishDAGProvenanceFloorBinding()
{
    DAGProvenanceCertificate cert;
    bool haveCert = false;
    if (!ReadDAGProvenanceCertificate(&cert, &haveCert)) return false;
    if (!haveCert) return true;
    return RepublishDAGProvenanceCertificateCommon(&cert);
}

bool CTxDB::RepublishDAGProvenanceCertificateCommon(DAGProvenanceCertificate* cert)
{
    if (!cert) return false;
    int32_t floorValue = 0, cleanHeight = 0;
    const bool haveFloor = ReadDAGPruneFloor(floorValue);
    const bool haveClean = ReadDAGCleanHeight(cleanHeight);
    cert->floorValue = haveFloor ? floorValue : -1;
    cert->cleanHeightValue = haveClean ? cleanHeight : -1;
    cert->floorStateDigest = HashFloorState(cert->floorValue, cert->cleanHeightValue);
    if (haveClean && cleanHeight > cert->hCert) cert->hCert = cleanHeight;
    if (haveFloor && floorValue > cert->hCert) cert->hCert = floorValue;
    uint64_t counter = 0;
    if (!ReadDAGProvenanceCounter(&counter)) return false;
    cert->mutationCounter = counter;
    uint64_t head = 0, length = 0; uint256 headHash;
    if (!ReadDAGPruneJournal(&head, &length, &headHash)) return false;
    cert->journalHeadHash = headHash;
    cert->journalLength = length;
    uint64_t w = 0; std::string werr;
    cert->watermark = ReadEngineLastSequence(&w, &werr) ? w : 0;
    return Write(string("dagcert"), SerializeDAGProvenanceCertificate(*cert));
}

bool CTxDB::SealDAGCustody(uint64_t* sealedWatermark, std::string* error)
{
    if (activeBatch) { if (error) *error = "seal refused: an active batch is open"; return false; }
    DAGProvenanceCertificate cert;
    bool haveCert = false;
    if (!ReadDAGProvenanceCertificate(&cert, &haveCert))
    {
        if (error) *error = "seal unavailable: certificate unreadable";
        return false;
    }
    if (!haveCert)
    {
        if (error) *error = "seal unavailable: no certificate (provenance coverage not established)";
        return false;
    }
    if (g_dagCustodyState == DAGCustodyState::SUSPENDED)
    {
        if (error) *error = "seal refused: custody is already suspended";
        return false;
    }
    uint64_t p = 0; std::string werr;
    if (!ReadEngineLastSequence(&p, &werr))
    {
        g_dagCustodyState = DAGCustodyState::UNAVAILABLE;   // capability lost: coverage does not survive
        if (error) *error = "seal unavailable: engine watermark capability: " + werr;
        return false;
    }
    // This batch contains exactly ONE record (the seal itself), so C == 1 exactly and
    // the seal commits to W = P + C in the same record that the commit publishes.
    const uint64_t c = 1;
    const uint64_t w = p + c;
    DAGCustodySeal seal;
    seal.watermark = w;
    seal.epoch = cert.epoch;
    {
        uint64_t counter = 0;
        if (!ReadDAGProvenanceCounter(&counter)) { if (error) *error = "seal unavailable: counter unreadable"; return false; }
        seal.counter = counter;
    }
    if (!TxnBegin()) return false;
    if (!Write(string("dagcustodyseal"), SerializeDAGCustodySeal(seal)))
    {
        TxnAbort();
        if (error) *error = "seal failed: write";
        return false;
    }
    if (!TxnCommit())
    {
        g_dagCustodyState = DAGCustodyState::SUSPENDED;
        if (error) *error = "seal failed: commit";
        return false;
    }
    uint64_t after = 0;
    if (!ReadEngineLastSequence(&after, &werr) || after != w)
    {
        // Exact mismatch: fail closed. Never a second write to "fix" the seal.
        g_dagCustodyState = DAGCustodyState::SUSPENDED;
        if (error) *error = "seal verification failed: actual LastSequence != W";
        return false;
    }
    g_dagCustodyState = DAGCustodyState::VERIFIED;
    if (sealedWatermark) *sealedWatermark = w;
    return true;
}

bool CTxDB::VerifyDAGCustodyAtOpen(std::string* error)
{
    DAGCustodySeal seal;
    bool haveSeal = false;
    if (!ReadDAGCustodySeal(&seal, &haveSeal))
    {
        g_dagCustodyState = DAGCustodyState::UNAVAILABLE;
        if (error) *error = "custody seal unreadable";
        return false;
    }
    if (!haveSeal)
    {
        g_dagCustodyState = DAGCustodyState::UNAVAILABLE;
        if (error) *error = "no custody seal: cross-session coverage is unavailable";
        return true;
    }
    uint64_t cur = 0; std::string werr;
    if (!ReadEngineLastSequence(&cur, &werr))
    {
        g_dagCustodyState = DAGCustodyState::UNAVAILABLE;
        if (error) *error = "engine watermark capability unavailable: " + werr;
        return true;
    }
    if (cur != seal.watermark)
    {
        // An unsupported/foreign writer consumed engine sequence numbers: the final
        // application-visible rows may look identical, but custody continuity does not.
        g_dagCustodyState = DAGCustodyState::SUSPENDED;
        if (error) *error = "custody watermark mismatch: LastSequence != sealed W";
        return true;
    }
    // R3.8 (restart seam): a restart may restore provenance admissibility ONLY when the
    // certificate, the journal/incarnations and the exact custody watermark all verify.
    // An unexplained hole, an invalid certificate or a broken journal is NOT "empty DAG
    // state", NOT "zero state" and NOT an objectively pruned row: it suspends coverage so
    // that live and restart interpretation of the same durable state agree.
    DAGCertVerifyResult resumed = DAGCertVerifyResult::NO_CERTIFICATE;
    std::string verr;
    if (!VerifyDAGProvenanceCoverage(&resumed, &verr))
    {
        g_dagCustodyState = DAGCustodyState::SUSPENDED;
        if (error) *error = "restored certification could not be evaluated: " + verr;
        return true;
    }
    if (resumed == DAGCertVerifyResult::NO_CERTIFICATE)
    {
        // Nothing was ever certified: cross-session coverage is unavailable. This is
        // availability loss (explicitly acceptable), never a false-positive prune.
        g_dagCustodyState = DAGCustodyState::UNAVAILABLE;
        if (error) *error = "no coverage certificate: cross-session provenance coverage is unavailable";
        return true;
    }
    if (resumed != DAGCertVerifyResult::OK)
    {
        g_dagCustodyState = DAGCustodyState::SUSPENDED;
        if (error) *error = "restored provenance certification rejected (result=" +
                            std::to_string((int)resumed) + ")" +
                            (verr.empty() ? std::string() : ": " + verr);
        return true;
    }
    g_dagCustodyState = DAGCustodyState::VERIFIED;
    return true;
}

bool CTxDB::ReadDAGChildCount(const uint256& parent, uint64_t* count, bool* present)
{
    if (!count || !present || ChildCountRevoked(GetInstance())) return false;
    *count = 0; *present = false;
    CDataStream key(SER_DISK, CLIENT_VERSION);
    key << make_pair(string("dagchildcount"), parent);
    std::string raw;
    bool deleted = false;
    const bool inBatch = activeBatch && ScanBatch(key, &raw, &deleted);
    if (deleted) return true;
    if (!inBatch) {
        const leveldb::Status status = GetInstance()->Get(leveldb::ReadOptions(), key.str(), &raw);
        if (status.IsNotFound()) return true;
        if (!status.ok()) { RevokeChildCount(GetInstance()); return false; }
    }
    *present = true;
    if (raw.size() == sizeof(uint64_t)) {
        try {
            CDataStream value(raw.data(), raw.data()+raw.size(), SER_DISK, CLIENT_VERSION);
            value >> *count;
            if (*count != 0 && value.empty()) return true;
        } catch (const std::exception&) {}
    }
    RevokeChildCount(GetInstance());
    return false;
}

bool CTxDB::IsDAGChildCountIndexHealthy(std::string* error)
{
    if (error) error->clear();
    if (ChildCountRevoked(GetInstance())) { if (error) *error="DAG child-count projection revoked/rebuild-required"; return false; }
    uint256 source;
    if (!ReadDAGSourceStateId(source)) { if (error) *error="DAG child-count index: source token unavailable"; return false; }
    const std::pair<std::string, uint8_t> key = make_pair(string("dagchildcountstate"), uint8_t(0));
    if (!Exists(key)) { if (error) *error="DAG child-count index: state marker missing"; return false; }
    std::pair<uint32_t,uint256> state;
    if (!Read(key,state)) { if (error) *error="DAG child-count index: corrupt state marker"; return false; }
    if (state.first != 1) { if (error) *error="DAG child-count index: unsupported state marker version"; return false; }
    if (state.second != source) { if (error) *error="DAG child-count index: state marker/source token mismatch"; return false; }
    return true;
}

bool CTxDB::EnsureDAGChildCountIndex(std::string* error)
{
    if (error) error->clear();
    if (activeBatch) { if (error) *error="child-count rebuild requires quiesced source without active transaction"; return false; }
    uint256 source;
    if (!ReadDAGSourceStateId(source))
    {
        if (error) *error = "DAG child-count index: source token unavailable";
        return false;
    }
    const std::pair<uint32_t, uint256> stateKey = make_pair((uint32_t)1, source);
    std::pair<uint32_t, uint256> state;
    if (!ChildCountRevoked(GetInstance()) && Read(make_pair(string("dagchildcountstate"), uint8_t(0)), state) && state == stateKey)
        return true;
    if (Exists(make_pair(string("dagchildcountstate"), uint8_t(0))) &&
        !Read(make_pair(string("dagchildcountstate"), uint8_t(0)), state))
    {
        if (error) *error = "DAG child-count index: corrupt state marker";
        return false;
    }

    if (!RevokeChildCount(GetInstance())) { if (error) *error="cannot persist child-count revocation"; return false; }
    // No marker is trusted during rebuild. Counts are a bounded disk projection;
    // a crash leaves no matching marker and the next startup rebuilds from the
    // canonical child->parents relation rather than trusting partial counts.
    if (!Erase(make_pair(string("dagchildcountstate"), uint8_t(0))))
    {
        if (error) *error = "DAG child-count index: cannot clear stale state";
        return false;
    }
    leveldb::DB* db = GetInstance();
    if (!db)
    {
        if (error) *error = "DAG child-count index: database unavailable";
        return false;
    }
    CDataStream countPrefixStream(SER_DISK, CLIENT_VERSION);
    countPrefixStream << string("dagchildcount");
    const std::string countPrefix = countPrefixStream.str();
    leveldb::Iterator* clear = db->NewIterator(leveldb::ReadOptions());
    clear->Seek(countPrefix);
    while (clear->Valid() && clear->key().ToString().compare(0, countPrefix.size(), countPrefix) == 0)
    {
        leveldb::Status s = db->Delete(leveldb::WriteOptions(), clear->key());
        if (!s.ok()) { delete clear; if (error) *error = s.ToString(); return false; }
        clear->Next();
    }
    if (!clear->status().ok()) { if (error) *error=clear->status().ToString(); delete clear; return false; }
    delete clear;

    CDataStream linksPrefixStream(SER_DISK, CLIENT_VERSION);
    linksPrefixStream << string("daglinks");
    const std::string linksPrefix = linksPrefixStream.str();
    leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
    it->Seek(linksPrefix);
    while (it->Valid() && it->key().ToString().compare(0, linksPrefix.size(), linksPrefix) == 0)
    {
        try {
            CDataStream value(SER_DISK, CLIENT_VERSION);
            value.write(it->value().data(), it->value().size());
            CBlockDAGData child;
            value >> child;
            std::set<uint256> unique(child.vDAGParents.begin(), child.vDAGParents.end());
            for (std::set<uint256>::const_iterator p = unique.begin(); p != unique.end(); ++p)
            {
                uint64_t count = 0; bool present = false;
                present = Exists(make_pair(string("dagchildcount"), *p));
                if ((present && (!Read(make_pair(string("dagchildcount"), *p), count) || count == 0)) || count == UINT64_MAX)
                    throw std::runtime_error("child-count read/overflow");
                if (!Write(make_pair(string("dagchildcount"), *p), count + 1))
                    throw std::runtime_error("child-count write");
            }
        } catch (const std::exception& e) {
            delete it; if (error) *error = std::string("DAG child-count index: ") + e.what(); return false;
        }
        it->Next();
    }
    if (!it->status().ok()) { std::string e = it->status().ToString(); delete it; if (error) *error = e; return false; }
    delete it;
    if (g_testFailDAGChildCountRebuild) { if (error) *error="injected rebuild failure before publication"; return false; }
    uint256 finalSource;
    if (!ReadDAGSourceStateId(finalSource) || finalSource != source)
    {
        if (error) *error = "DAG child-count index: source changed or state publication failed";
        return false;
    }
    CDataStream markerKey(SER_DISK, CLIENT_VERSION), markerValue(SER_DISK, CLIENT_VERSION);
    markerKey << make_pair(string("dagchildcountstate"), uint8_t(0));
    markerValue << make_pair(uint32_t(1), source);
    leveldb::WriteBatch publication;
    publication.Put(markerKey.str(), markerValue.str());
    publication.Delete(ChildCountRevocationKey());
    leveldb::WriteOptions durable; durable.sync = true;
    if (!db->Write(durable, &publication).ok()) { dagChildCountRevocationWriteFailed = true; if (error) *error="child-count certificate publication failed"; return false; }
    dagChildCountRevocationWriteFailed = false;
    return true;
}

bool CTxDB::RevokeDAGChildCountForTest()
{
    return RevokeChildCount(GetInstance());
}

bool CTxDB::IsDAGScoreAuthorityHealthy(std::string* error)
{
    return GetDAGScoreAuthorityStatus(error) == DAG_SCORE_AUTHORITY_HEALTHY;
}

CTxDB::DAGScoreAuthorityStatus CTxDB::GetDAGScoreAuthorityStatus(std::string* error)
{
    if (error) error->clear();
    if (ScoreAuthorityRevoked(GetInstance())) { if (error) *error="DAG score authority revoked/rebuild-required"; return DAG_SCORE_AUTHORITY_REVOKED; }
    uint256 source;
    if (!ReadDAGSourceStateId(source)) { if (error) *error="DAG score authority: source token unavailable"; return DAG_SCORE_AUTHORITY_UNAVAILABLE; }
    CDataStream keyStream(SER_DISK, CLIENT_VERSION);
    keyStream << make_pair(SCORE_STATE_KEY, uint8_t(0));
    std::string key = keyStream.str();
    std::string raw;
    leveldb::DB* db = GetInstance();
    if (!db) { if (error) *error="DAG score authority: database unavailable"; return DAG_SCORE_AUTHORITY_UNAVAILABLE; }
    const leveldb::Status st = db->Get(leveldb::ReadOptions(), key, &raw);
    if (st.IsNotFound()) { if (error) *error="DAG score authority: state marker missing"; return DAG_SCORE_AUTHORITY_UNCERTIFIED; }
    if (!st.ok()) { if (error) *error="DAG score authority: state marker read error"; return DAG_SCORE_AUTHORITY_UNAVAILABLE; }
    std::pair<uint32_t,uint256> state;
    try {
        CDataStream is(raw.data(), raw.data()+raw.size(), SER_DISK, CLIENT_VERSION);
        is >> state;
        if (!is.empty()) { if (error) *error="DAG score authority: trailing bytes in state marker"; return DAG_SCORE_AUTHORITY_CORRUPT; }
    } catch (const std::exception&) { if (error) *error="DAG score authority: corrupt state marker"; return DAG_SCORE_AUTHORITY_CORRUPT; }
    if (state.first != 1) { if (error) *error="DAG score authority: unsupported state marker version"; return DAG_SCORE_AUTHORITY_CORRUPT; }
    if (state.second != source) { if (error) *error="DAG score authority: state marker/source token mismatch"; return DAG_SCORE_AUTHORITY_CORRUPT; }
    return DAG_SCORE_AUTHORITY_HEALTHY;
}

bool CTxDB::PublishDAGScoreCertificateAtomic(std::string* error)
{
    if (error) error->clear();
    if (activeBatch) { if (error) *error="DAG score certificate publish requires quiesced source without active transaction"; return false; }
    leveldb::DB* db = GetInstance();
    if (!db) { if (error) *error="DAG score authority: database unavailable"; return false; }
    uint256 finalSource;
    if (!ReadDAGSourceStateId(finalSource)) { if (error) *error="DAG score authority: cannot read source for certification"; return false; }
    CDataStream markerKey(SER_DISK, CLIENT_VERSION), markerValue(SER_DISK, CLIENT_VERSION);
    markerKey << make_pair(SCORE_STATE_KEY, uint8_t(0));
    markerValue << make_pair((uint32_t)1, finalSource);
    leveldb::WriteBatch publication;
    publication.Put(markerKey.str(), markerValue.str());
    publication.Delete(ScoreAuthorityRevocationKey());
    leveldb::WriteOptions durable; durable.sync = true;
    if (!db->Write(durable, &publication).ok()) { dagScoreRevocationWriteFailed = true; if (error) *error="DAG score authority certificate publication failed"; return false; }
    dagScoreRevocationWriteFailed = false;
    return true;
}

bool CTxDB::RevokeDAGScoreAuthorityForTest()
{
    return RevokeScoreAuthority(GetInstance());
}

bool CTxDB::StageDAGLinkRawForTest(const uint256& hash, const CBlockDAGData& data, bool erase)
{
    if (!activeBatch) return false;
    if (erase) return Erase(make_pair(string("daglinks"), hash));
    return Write(make_pair(string("daglinks"), hash), data);
}

bool CTxDB::ReadDAGSourceStateId(uint256& out)
{
    return Read(make_pair(string("dagsourcestate"), uint8_t(0)), out);
}

bool CTxDB::HasDAGSourceStateId()
{
    return Exists(make_pair(string("dagsourcestate"), uint8_t(0)));
}

bool CTxDB::WriteDAGSourceStateId(const uint256& id)
{
    if (ChildCountRevoked(GetInstance())) return false;
    // Validate the OLD binding before staging the new source token. A
    // supported marker alone is not evidence that its projection is current.
    const std::pair<std::string, uint8_t> key = make_pair(string("dagchildcountstate"), uint8_t(0));
    const bool hasMarker = Exists(key);
    if (hasMarker) {
        std::string error;
        if (!IsDAGChildCountIndexHealthy(&error)) return false;
    }
    if (!Write(make_pair(string("dagsourcestate"), uint8_t(0)), id)) return false;
    // Tokenless legacy/bootstrap may advance without a projection marker;
    // only explicit rebuild can create its initial healthy certificate.
    if (!hasMarker) return true;
    return Write(key, make_pair((uint32_t)1, id));
}

bool CTxDB::StageDAGScoreCertificateInBatch(const uint256& source, std::string* error)
{
    if (error) error->clear();
    if (!activeBatch) { if (error) *error="S3 score-cert: no active transaction"; return false; }
    if (!Write(make_pair(SCORE_STATE_KEY, uint8_t(0)), make_pair((uint32_t)1, source)))
    { if (error) *error="S3 score-cert: marker stage failed"; return false; }
    // Raw key: ScoreAuthorityRevocationKey() is ALREADY encoded; routing it
    // through the templated Erase() would re-encode (length-prefix) and delete
    // a different key. Mirror the raw batch deletes used by the restore path
    // and by PublishDAGScoreCertificateAtomic.
    activeBatch->Delete(ScoreAuthorityRevocationKey());
    return true;
}

bool CTxDB::StageDAGChildCountCertificateInBatch(const uint256& source, std::string* error)
{
    if (error) error->clear();
    if (!activeBatch) { if (error) *error="S3 childcount-cert: no active transaction"; return false; }
    const std::pair<std::string, uint8_t> key = make_pair(string("dagchildcountstate"), uint8_t(0));
    if (!Write(key, make_pair((uint32_t)1, source)))
    { if (error) *error="S3 childcount-cert: marker stage failed"; return false; }
    // Raw key (same defect class as StageDAGScoreCertificateInBatch): the
    // pre-encoded revocation key must not be re-encoded through Erase().
    activeBatch->Delete(ChildCountRevocationKey());
    return true;
}

bool CTxDB::CaptureDAGScoreCertificateState(bool* markerPresent, std::string* markerRaw,
                                            bool* revoked, std::string* error)
{
    if (error) error->clear();
    if (!markerPresent || !markerRaw || !revoked) { if (error) *error="S3 capture: null output"; return false; }
    leveldb::DB* db = GetInstance();
    if (!db) { if (error) *error="S3 capture: database unavailable"; return false; }
    *markerPresent = false; markerRaw->clear(); *revoked = false;
    CDataStream markerKey(SER_DISK, CLIENT_VERSION);
    markerKey << make_pair(SCORE_STATE_KEY, uint8_t(0));
    std::string value;
    const leveldb::Status st = db->Get(leveldb::ReadOptions(), markerKey.str(), &value);
    if (st.IsNotFound()) { *markerPresent = false; }
    else if (!st.ok()) { if (error) *error="S3 capture: score marker read error"; return false; }
    else { *markerPresent = true; *markerRaw = value; }
    // Revocation presence mirrors ScoreAuthorityRevoked(): any read error is
    // unavailable, never healthy (captured as revoked, fail-closed).
    std::string revValue;
    const leveldb::Status rst = db->Get(leveldb::ReadOptions(), ScoreAuthorityRevocationKey(), &revValue);
    *revoked = !rst.IsNotFound();
    return true;
}

bool CTxDB::RestoreDAGScoreCertificateStateInBatch(bool markerPresent, const std::string& markerRaw,
                                                   bool revoked, std::string* error)
{
    if (error) error->clear();
    if (!activeBatch) { if (error) *error="S3 restore: no active transaction"; return false; }
    CDataStream markerKey(SER_DISK, CLIENT_VERSION);
    markerKey << make_pair(SCORE_STATE_KEY, uint8_t(0));
    if (markerPresent) activeBatch->Put(markerKey.str(), markerRaw);
    else activeBatch->Delete(markerKey.str());
    if (revoked) activeBatch->Put(ScoreAuthorityRevocationKey(), "rebuild-required");
    else activeBatch->Delete(ScoreAuthorityRevocationKey());
    return true;
}

bool CTxDB::MintDAGSourceStateId(uint256& out)
{
    if (g_testFailDAGSourceStateBootstrapMint ||
        RAND_bytes(reinterpret_cast<unsigned char*>(&out), sizeof(out)) != 1)
        return false;
    return out != uint256(0);
}

bool CTxDB::BootstrapDAGSourceStateId(std::string* error)
{
    if (error) error->clear();
    uint256 existing;
    if (ReadDAGSourceStateId(existing))
        return true; // Existing state identity is restart-idempotent.
    if (HasDAGSourceStateId())
    {
        if (error) *error = "DAG source-state token is corrupt or undecodable";
        return false;
    }

    uint256 minted;
    if (!MintDAGSourceStateId(minted))
    {
        if (error) *error = "DAG source-state token generation failed";
        return false;
    }
    if (g_testFailDAGSourceStateBootstrapTxnBegin || !TxnBegin())
    {
        if (error) *error = "DAG source-state bootstrap TxnBegin failed";
        return false;
    }
    if (!WriteDAGSourceStateId(minted))
    {
        TxnAbort();
        if (error) *error = "DAG source-state bootstrap token write failed";
        return false;
    }
    if (g_testFailDAGSourceStateBootstrapTxnCommit)
    {
        TxnAbort(); // deterministic pre-write failure: no storage effect.
        if (error) *error = "DAG source-state bootstrap TxnCommit failed";
        return false;
    }
    if (!TxnCommit())
    {
        if (error) *error = "DAG source-state bootstrap TxnCommit failed";
        return false;
    }
    uint256 readBack;
    if (!ReadDAGSourceStateId(readBack) || readBack != minted)
    {
        if (error) *error = "DAG source-state bootstrap read-back mismatch";
        return false;
    }
    return true;
}

// IDAG Phase 3: Epoch state persistence
bool CTxDB::WriteEpochState(int nEpoch, const CEpochState& state)
{
    return Write(make_pair(string("epochstate"), nEpoch), state);
}

bool CTxDB::IterateEpochStates(std::map<int, CEpochState>& mapOut)
{
    mapOut.clear();
    leveldb::DB* db = GetInstance();
    if (!db)
        return false;

    CDataStream ssPrefix(SER_DISK, CLIENT_VERSION);
    ssPrefix << string("epochstate");
    std::string strPrefix = ssPrefix.str();

    leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
    it->Seek(strPrefix);

    while (it->Valid())
    {
        std::string strKey = it->key().ToString();
        if (strKey.compare(0, strPrefix.size(), strPrefix) != 0)
            break;

        try {
            CDataStream ssKey(strKey.data(), strKey.data() + strKey.size(), SER_DISK, CLIENT_VERSION);
            std::pair<std::string, int> keyPair;
            ssKey >> keyPair;

            CDataStream ssValue(it->value().data(), it->value().data() + it->value().size(), SER_DISK, CLIENT_VERSION);
            CEpochState state;
            ssValue >> state;
            mapOut[keyPair.second] = state;
        }
        catch (const std::exception&)
        {
        }

        it->Next();
    }

    delete it;
    return true;
}

bool CTxDB::IterateCurveTreeEpochs(std::map<int, CCurveTree>& mapOut)
{
    mapOut.clear();
    leveldb::DB* db = GetInstance();
    if (!db)
        return false;

    CDataStream ssPrefix(SER_DISK, CLIENT_VERSION);
    ssPrefix << string("ce");
    std::string strPrefix = ssPrefix.str();

    leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
    it->Seek(strPrefix);

    while (it->Valid())
    {
        std::string strKey = it->key().ToString();
        if (strKey.compare(0, strPrefix.size(), strPrefix) != 0)
            break;

        try {
            CDataStream ssKey(strKey.data(), strKey.data() + strKey.size(), SER_DISK, CLIENT_VERSION);
            std::pair<std::string, int> keyPair;
            ssKey >> keyPair;

            CDataStream ssValue(it->value().data(), it->value().data() + it->value().size(), SER_DISK, CLIENT_VERSION);
            CCurveTree tree;
            ssValue >> tree;
            mapOut[keyPair.second] = tree;
        }
        catch (const std::exception&)
        {
        }

        it->Next();
    }

    delete it;
    return true;
}

bool CTxDB::WriteDAGCleanHeight(int nHeight)
{
    if (!Write(string("dagcleanheight"), nHeight)) return false;
    // R3 / C6 section 6: the clean height is part of the certified coverage state and
    // bounds the certified domain, so it re-publishes the certificate in the same batch.
    return RepublishDAGProvenanceFloorBinding();
}

bool CTxDB::ReadDAGCleanHeight(int& nHeight)
{
    return Read(string("dagcleanheight"), nHeight);
}

bool CTxDB::EraseDAGCleanHeight()
{
    return Erase(string("dagcleanheight"));
}

// F2 erase provenance: written ONLY by the prune/erase lifecycle
// (CDAGManager::PruneDAGData, in the same atomic batch as the erasures) and
// consumed ONLY as ERASE provenance (LegitimatePrunedBoundary). Distinct key,
// distinct meaning - never advanced by Shutdown().
bool CTxDB::WriteDAGPruneFloor(int nHeight)
{
    if (!Write(string("dagprunefloor"), nHeight)) return false;
    // R3 / C6 section 6: the floor is part of the certified coverage state, so a floor
    // change re-binds and re-publishes the certificate IN THE SAME BATCH (no-op when the
    // store is uncertified). Without this the certificate would go stale the moment a
    // prune advanced the floor.
    return RepublishDAGProvenanceFloorBinding();
}

bool CTxDB::ReadDAGPruneFloor(int& nHeight)
{
    return Read(string("dagprunefloor"), nHeight);
}

bool CTxDB::EraseDAGPruneFloor()
{
    return Erase(string("dagprunefloor"));
}

// F2-B1-R row-erase provenance (see header). Read is three-state: false means the
// store could not be read (callers fail closed); true with *present=false means
// there is no record (the absence is not attributable to a non-prune lifecycle).
bool CTxDB::WriteDAGRowErase(const uint256& hash, int origin)
{
    return Write(make_pair(string("dagrowerase"), hash), origin);
}

bool CTxDB::ReadDAGRowErase(const uint256& hash, int* origin, bool* present)
{
    if (!origin || !present) return false;
    *origin = -1; *present = false;
    const std::pair<std::string, uint256> key = make_pair(string("dagrowerase"), hash);
    if (!Read(key, *origin))
        return !Exists(key);   // present but unreadable/malformed -> fail closed
    *present = true;
    return true;
}

bool CTxDB::EraseDAGRowErase(const uint256& hash)
{
    return Erase(make_pair(string("dagrowerase"), hash));
}

bool CTxDB::WriteFinalityVote(const uint256& nullifier, const CFinalityVote& vote)
{
    return Write(make_pair(string("finalityvote"), nullifier), vote);
}

bool CTxDB::ReadFinalityVote(const uint256& nullifier, CFinalityVote& vote)
{
    return Read(make_pair(string("finalityvote"), nullifier), vote);
}

bool CTxDB::EraseFinalityVote(const uint256& nullifier)
{
    return Erase(make_pair(string("finalityvote"), nullifier));
}

bool CTxDB::IterateFinalityVotes(std::map<uint256, CFinalityVote>& mapOut)
{
    mapOut.clear();
    leveldb::DB* db = GetInstance();
    if (!db)
        return false;

    CDataStream ssPrefix(SER_DISK, CLIENT_VERSION);
    ssPrefix << string("finalityvote");
    std::string strPrefix = ssPrefix.str();

    leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
    it->Seek(strPrefix);

    while (it->Valid())
    {
        std::string strKey = it->key().ToString();
        if (strKey.compare(0, strPrefix.size(), strPrefix) != 0)
            break;

        try {
            CDataStream ssKey(strKey.data(), strKey.data() + strKey.size(), SER_DISK, CLIENT_VERSION);
            std::pair<std::string, uint256> keyPair;
            ssKey >> keyPair;

            CDataStream ssValue(it->value().data(), it->value().data() + it->value().size(), SER_DISK, CLIENT_VERSION);
            CFinalityVote vote;
            ssValue >> vote;
            mapOut[keyPair.second] = vote;
        }
        catch (const std::exception&)
        {
            // Skip malformed entries.
        }

        it->Next();
    }

    delete it;
    return true;
}

bool CTxDB::WriteFinalityTallyShare(const uint256& hashShare, const CFinalityTallyShare& share)
{
    return Write(make_pair(string("finalityshare"), hashShare), share);
}

bool CTxDB::ReadFinalityTallyShare(const uint256& hashShare, CFinalityTallyShare& share)
{
    return Read(make_pair(string("finalityshare"), hashShare), share);
}

bool CTxDB::EraseFinalityTallyShare(const uint256& hashShare)
{
    return Erase(make_pair(string("finalityshare"), hashShare));
}

bool CTxDB::IterateFinalityTallyShares(std::map<uint256, CFinalityTallyShare>& mapOut)
{
    mapOut.clear();
    leveldb::DB* db = GetInstance();
    if (!db)
        return false;

    CDataStream ssPrefix(SER_DISK, CLIENT_VERSION);
    ssPrefix << string("finalityshare");
    std::string strPrefix = ssPrefix.str();

    leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
    it->Seek(strPrefix);

    while (it->Valid())
    {
        std::string strKey = it->key().ToString();
        if (strKey.compare(0, strPrefix.size(), strPrefix) != 0)
            break;

        try {
            CDataStream ssKey(strKey.data(), strKey.data() + strKey.size(), SER_DISK, CLIENT_VERSION);
            std::pair<std::string, uint256> keyPair;
            ssKey >> keyPair;

            CDataStream ssValue(it->value().data(), it->value().data() + it->value().size(), SER_DISK, CLIENT_VERSION);
            CFinalityTallyShare share;
            ssValue >> share;
            mapOut[keyPair.second] = share;
        }
        catch (const std::exception&)
        {
            // Skip malformed entries.
        }

        it->Next();
    }

    delete it;
    return true;
}

bool CTxDB::WriteFinalityTallyCertificate(const uint256& hashCert, const CFinalityTallyCertificate& cert)
{
    return Write(make_pair(string("finalitycert"), hashCert), cert);
}

bool CTxDB::ReadFinalityTallyCertificate(const uint256& hashCert, CFinalityTallyCertificate& cert)
{
    return Read(make_pair(string("finalitycert"), hashCert), cert);
}

bool CTxDB::EraseFinalityTallyCertificate(const uint256& hashCert)
{
    return Erase(make_pair(string("finalitycert"), hashCert));
}

bool CTxDB::IterateFinalityTallyCertificates(std::map<uint256, CFinalityTallyCertificate>& mapOut)
{
    mapOut.clear();
    leveldb::DB* db = GetInstance();
    if (!db)
        return false;

    CDataStream ssPrefix(SER_DISK, CLIENT_VERSION);
    ssPrefix << string("finalitycert");
    std::string strPrefix = ssPrefix.str();

    leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
    it->Seek(strPrefix);

    while (it->Valid())
    {
        std::string strKey = it->key().ToString();
        if (strKey.compare(0, strPrefix.size(), strPrefix) != 0)
            break;

        try {
            CDataStream ssKey(strKey.data(), strKey.data() + strKey.size(), SER_DISK, CLIENT_VERSION);
            std::pair<std::string, uint256> keyPair;
            ssKey >> keyPair;

            CDataStream ssValue(it->value().data(), it->value().data() + it->value().size(), SER_DISK, CLIENT_VERSION);
            CFinalityTallyCertificate cert;
            ssValue >> cert;
            mapOut[keyPair.second] = cert;
        }
        catch (const std::exception&)
        {
            // Skip malformed entries.
        }

        it->Next();
    }

    delete it;
    return true;
}

bool CTxDB::IterateDAGLinks(std::map<uint256, CBlockDAGData>& mapOut)
{
    mapOut.clear();
    leveldb::DB* db = GetInstance();
    if (!db)
        return false;

    // Build the serialized prefix for "daglinks" key type
    CDataStream ssPrefix(SER_DISK, CLIENT_VERSION);
    ssPrefix << string("daglinks");
    std::string strPrefix = ssPrefix.str();

    leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
    it->Seek(strPrefix);

    while (it->Valid())
    {
        std::string strKey = it->key().ToString();
        if (strKey.compare(0, strPrefix.size(), strPrefix) != 0)
            break;

        try {
            CDataStream ssKey(strKey.data(), strKey.data() + strKey.size(), SER_DISK, CLIENT_VERSION);
            std::pair<std::string, uint256> keyPair;
            ssKey >> keyPair;

            CDataStream ssValue(it->value().data(), it->value().data() + it->value().size(), SER_DISK, CLIENT_VERSION);
            CBlockDAGData data;

            // Phase 4 compat: deserialize core fields first, then try nInferredK
            ssValue >> data.vDAGParents;
            ssValue >> data.vDAGChildren;
            ssValue >> data.fBlue;
            ssValue >> data.nDAGScore;
            ssValue >> data.nDAGOrder;

            // nInferredK may not exist in pre-Phase 4 entries
            if (ssValue.size() > 0)
            {
                try { ssValue >> data.nInferredK; }
                catch (const std::exception&) { data.nInferredK = -1; }
            }
            else
            {
                data.nInferredK = -1;
            }

            mapOut[keyPair.second] = data;
        }
        catch (const std::exception&)
        {
            // Skip malformed entries
        }

        it->Next();
    }

    delete it;
    return true;
}

// STRICT persisted daglinks enumeration for authoritative source construction.
// Rejects (returns false, populates *error) any malformed record instead of
// skipping it: malformed key, unexpected key-prefix field, malformed value,
// missing/trailing bytes, or an iterator/read error. A canonical daglinks
// record is exactly: key = pair<string("daglinks"), uint256>, value = the
// CBlockDAGData serialization (vDAGParents, vDAGChildren, fBlue, nDAGScore,
// nDAGOrder, then optional nInferredK for Phase-4/Phase-5 records). Duplicate
// canonical identity (same hash twice) is rejected. After reading every
// well-formed record the iterator status is checked and a read error yields
// false. This is the fail-closed drain for EnumerateAuthoritativeStagedScope:
// an incomplete/malformed retained canvas can never be silently certified.
bool CTxDB::IterateDAGLinksStrict(std::map<uint256, CBlockDAGData>& mapOut, std::string* error)
{
    mapOut.clear();
    if (error) error->clear();
    leveldb::DB* db = GetInstance();
    if (!db) { if (error) *error = "StrictDAGLinks: no DB instance"; return false; }

    CDataStream ssPrefix(SER_DISK, CLIENT_VERSION);
    ssPrefix << string("daglinks");
    std::string strPrefix = ssPrefix.str();

    leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
    bool ok = true;
    std::string failReason;
    it->Seek(strPrefix);
    while (it->Valid())
    {
        const std::string strKey = it->key().ToString();
        if (strKey.compare(0, strPrefix.size(), strPrefix) != 0)
            break; // normal end of the daglinks prefix range

        CDataStream ssKey(strKey.data(), strKey.data() + strKey.size(), SER_DISK, CLIENT_VERSION);
        std::pair<std::string, uint256> keyPair;
        try { ssKey >> keyPair; }
        catch (const std::exception& e) { failReason = std::string("malformed key: ") + e.what(); ok = false; break; }
        if (keyPair.first != "daglinks") { failReason = "malformed key: unexpected prefix field"; ok = false; break; }
        if (ssKey.size() != 0) { failReason = "malformed key: trailing bytes"; ok = false; break; }
        if (mapOut.count(keyPair.second)) { failReason = "duplicate canonical identity " + keyPair.second.GetHex(); ok = false; break; }

        CDataStream ssValue(it->value().data(), it->value().data() + it->value().size(), SER_DISK, CLIENT_VERSION);
        CBlockDAGData data;
        try
        {
            ssValue >> data.vDAGParents;
            ssValue >> data.vDAGChildren;
            ssValue >> data.fBlue;
            ssValue >> data.nDAGScore;
            ssValue >> data.nDAGOrder;
            if (ssValue.size() > 0)
            {
                ssValue >> data.nInferredK;
                if (ssValue.size() != 0) { failReason = "malformed value: trailing bytes after nInferredK"; ok = false; break; }
            }
            else
            {
                data.nInferredK = -1;
            }
        }
        catch (const std::exception& e) { failReason = std::string("malformed value for ") + keyPair.second.GetHex() + ": " + e.what(); ok = false; break; }

        mapOut[keyPair.second] = data;
        it->Next();
    }

    if (ok && !it->status().ok())
    {
        ok = false;
        failReason = "iterator/read error: " + it->status().ToString();
    }
    delete it;
    if (!ok)
    {
        mapOut.clear(); // no partial usable output on any malformed/read failure
        if (error) *error = "StrictDAGLinks: " + failReason;
        return false;
    }
    return true;
}
// S3 staged-view enumeration: scan the active WriteBatch for daglinks
// writes/tombstones (prefix "daglinks"), so the authoritative staged view can be
// built as (persisted daglinks + staged writes - staged tombstones). Fail closed
// on any batch scan error; report activeBatchOpen=false when no txn is open.
class CBatchDAGLinksScanner : public leveldb::WriteBatch::Handler {
public:
    // last-op-wins per daglinks key, matching keyed ScanBatch semantics:
    // a final Delete yields a tombstone; a final Put yields a write.
    std::map<uint256, bool> finalOp;          // hash -> true=delete, false=put
    std::map<uint256, CBlockDAGData> finalData; // hash -> last written record
    std::string daglinksPrefix;
    bool failed;
    CBatchDAGLinksScanner() : failed(false) {
        CDataStream p(SER_DISK, CLIENT_VERSION); p << std::string("daglinks");
        daglinksPrefix = p.str();
    }
    virtual void Put(const leveldb::Slice& key, const leveldb::Slice& value) {
        std::string k = key.ToString();
        if (k.compare(0, daglinksPrefix.size(), daglinksPrefix) != 0) return;
        try {
            CDataStream ssKey(k.data(), k.data()+k.size(), SER_DISK, CLIENT_VERSION);
            std::pair<std::string, uint256> keyPair; ssKey >> keyPair;
            if (keyPair.first != "daglinks") return;
            CDataStream ssValue(value.data(), value.data()+value.size(), SER_DISK, CLIENT_VERSION);
            CBlockDAGData data;
            ssValue >> data.vDAGParents; ssValue >> data.vDAGChildren;
            ssValue >> data.fBlue; ssValue >> data.nDAGScore; ssValue >> data.nDAGOrder;
            if (ssValue.size() > 0) { try { ssValue >> data.nInferredK; } catch (const std::exception&) { data.nInferredK = -1; } }
            else data.nInferredK = -1;
            finalOp[keyPair.second] = false;       // last op so far = put
            finalData[keyPair.second] = data;
        } catch (const std::exception&) { failed = true; }
    }
    virtual void Delete(const leveldb::Slice& key) {
        std::string k = key.ToString();
        if (k.compare(0, daglinksPrefix.size(), daglinksPrefix) != 0) return;
        try {
            CDataStream ssKey(k.data(), k.data()+k.size(), SER_DISK, CLIENT_VERSION);
            std::pair<std::string, uint256> keyPair; ssKey >> keyPair;
            if (keyPair.first != "daglinks") return;
            finalOp[keyPair.second] = true;        // last op so far = delete
            finalData.erase(keyPair.second);
        } catch (const std::exception&) { failed = true; }
    }
};
bool CTxDB::ScanBatchDAGLinks(std::map<uint256, CBlockDAGData>* stagedWrites,
                              std::set<uint256>* stagedTombstones,
                              bool* activeBatchOpen, std::string* error)
{
    if (stagedWrites) stagedWrites->clear();
    if (stagedTombstones) stagedTombstones->clear();
    if (activeBatchOpen) *activeBatchOpen = (activeBatch != NULL);
    if (!activeBatch) return true; // no transaction open: empty staged delta
    CBatchDAGLinksScanner scanner;
    const leveldb::Status status = activeBatch->Iterate(&scanner);
    if (!status.ok()) { if (error) *error = "S3 staged view: batch scan failed: " + status.ToString(); return false; }
    if (scanner.failed) { if (error) *error = "S3 staged view: malformed staged daglinks record"; return false; }
    for (std::map<uint256,bool>::const_iterator it=scanner.finalOp.begin(); it!=scanner.finalOp.end(); ++it){
        if (it->second) { if (stagedTombstones) stagedTombstones->insert(it->first); }
        else if (stagedWrites && scanner.finalData.count(it->first)) (*stagedWrites)[it->first]=scanner.finalData.at(it->first);
    }
    return true;
}

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

    // IDAG Phase 2+3: Load DAG links (ordering deferred to init.cpp for incremental support)
    g_dagManager.LoadDAGLinks(*this);
    {
        std::string dagStateError;
        if (!BootstrapDAGSourceStateId(&dagStateError))
            return error("CTxDB::LoadBlockIndex() : DAG source-state bootstrap failed: %s", dagStateError.c_str());
        if (!EnsureDAGChildCountIndex(&dagStateError))
            return error("CTxDB::LoadBlockIndex() : DAG child-count index failed: %s", dagStateError.c_str());
    }
    g_dagManager.LoadEpochStates(*this);
    g_finalityTracker.LoadVotes(*this);
    g_finalityTracker.LoadTallyShares(*this);
    g_finalityTracker.LoadTallyCertificates(*this);

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
