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
#include "txdb.h"
#include "util.h"
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
    if (!Write(make_pair(string("daglinks"), hash), data)) return false;
    delta.Finish();
    return true;
}

bool CTxDB::EraseDAGLinks(const uint256& hash)
{
    if (!activeBatch || ChildCountRevoked(GetInstance())) return false;
    CBlockDAGData old;
    if (!ReadDAGLinks(hash, old))
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
    delta.Finish();
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
    if (error) error->clear();
    if (ScoreAuthorityRevoked(GetInstance())) { if (error) *error="DAG score authority revoked/rebuild-required"; return false; }
    uint256 source;
    if (!ReadDAGSourceStateId(source)) { if (error) *error="DAG score authority: source token unavailable"; return false; }
    CDataStream keyStream(SER_DISK, CLIENT_VERSION);
    keyStream << make_pair(SCORE_STATE_KEY, uint8_t(0));
    std::string key = keyStream.str();
    std::string raw;
    leveldb::DB* db = GetInstance();
    if (!db) { if (error) *error="DAG score authority: database unavailable"; return false; }
    const leveldb::Status st = db->Get(leveldb::ReadOptions(), key, &raw);
    if (st.IsNotFound()) { if (error) *error="DAG score authority: state marker missing"; return false; }
    if (!st.ok()) { if (error) *error="DAG score authority: state marker read error"; return false; }
    std::pair<uint32_t,uint256> state;
    try {
        CDataStream is(raw.data(), raw.data()+raw.size(), SER_DISK, CLIENT_VERSION);
        is >> state;
        if (!is.empty()) { if (error) *error="DAG score authority: trailing bytes in state marker"; return false; }
    } catch (const std::exception&) { if (error) *error="DAG score authority: corrupt state marker"; return false; }
    if (state.first != 1) { if (error) *error="DAG score authority: unsupported state marker version"; return false; }
    if (state.second != source) { if (error) *error="DAG score authority: state marker/source token mismatch"; return false; }
    return true;
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
    return Write(string("dagcleanheight"), nHeight);
}

bool CTxDB::ReadDAGCleanHeight(int& nHeight)
{
    return Read(string("dagcleanheight"), nHeight);
}

bool CTxDB::EraseDAGCleanHeight()
{
    return Erase(string("dagcleanheight"));
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
