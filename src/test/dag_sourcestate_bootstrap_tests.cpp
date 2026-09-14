#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE DAG SourceState Bootstrap
#include <boost/test/unit_test.hpp>

#include "../txdb.h"
#include "../dag.h"
#include "../dag_tips_delta.h"
#include "../util.h"
#include "../wallet.h"
#include "../ui_interface.h"
#include "../checkpoints.h"
#include <boost/filesystem.hpp>
#include <leveldb/db.h>

CWallet* pwalletMain = NULL;
CClientUIInterface uiInterface;
bool fConfChange = false;
bool fEnforceCanonical = true;
bool fUseFastIndex = true;
unsigned int nDerivationMethodIndex = 0;
unsigned int nMinerSleep = 5000;
unsigned int nNodeLifespan = 7;
enum Checkpoints::CPMode CheckpointsMode = Checkpoints::STRICT;
extern bool fPrintToConsole;
extern void noui_connect();
extern bool g_testFailDAGSourceStateBootstrapMint;
extern bool g_testFailDAGSourceStateBootstrapTxnBegin;
extern bool g_testFailDAGSourceStateBootstrapTxnCommit;
void Shutdown(void*) { exit(0); }
void StartShutdown() { exit(0); }

namespace {
namespace fs = boost::filesystem;

struct IsolatedTxDB
{
    fs::path root;
    static fs::path ProcessRoot()
    {
        static const fs::path p = fs::temp_directory_path() /
            fs::unique_path("innova-dagsource-%%%%-%%%%-%%%%");
        return p;
    }
    IsolatedTxDB()
    {
        root = ProcessRoot();
        fs::create_directories(root);
        // GetDataDir is process-cached: install the process-unique root before
        // constructing any CTxDB, then reset only its disposable source DB.
        mapArgs["-datadir"] = root.string();
        CTxDB old;
        old.Close();
        fs::remove_all(root / "txleveldb");
        fPrintToDebugger = true;
        noui_connect();
    }
    ~IsolatedTxDB()
    {
        CTxDB db;
        db.Close();
    }
    void Close() { CTxDB db; db.Close(); }
    void Reopen() { CTxDB db("c"); (void)db; }
};

static void PutCorruptToken()
{
    CTxDB db;
    CDataStream key(SER_DISK, CLIENT_VERSION);
    key << make_pair(std::string("dagsourcestate"), uint8_t(0));
    BOOST_REQUIRE(db.GetInstance()->Put(leveldb::WriteOptions(), key.str(), "x").ok());
}

static void PutRepresentativeDaglink()
{
    CTxDB db;
    CBlockDAGData d;
    d.vDAGParents.push_back(uint256(9));
    BOOST_REQUIRE(db.TxnBegin());
    BOOST_REQUIRE(db.WriteDAGLinks(uint256(10), d));
    BOOST_REQUIRE(db.TxnCommit());
}
}

BOOST_AUTO_TEST_CASE(tokenless_legacy_mints_and_close_reopen_preserves)
{
    IsolatedTxDB fx;
    PutRepresentativeDaglink();
    CTxDB db;
    BOOST_CHECK(!db.HasDAGSourceStateId());
    uint256 absent;
    BOOST_CHECK(!db.ReadDAGSourceStateId(absent));
    std::string error;
    BOOST_REQUIRE(db.BootstrapDAGSourceStateId(&error));
    uint256 first;
    BOOST_REQUIRE(db.ReadDAGSourceStateId(first));
    BOOST_CHECK(first != uint256(0));
    std::map<uint256, CBlockDAGData> links;
    BOOST_REQUIRE(db.IterateDAGLinks(links));
    BOOST_REQUIRE(links.count(uint256(10)) == 1);
    BOOST_CHECK(links[uint256(10)].vDAGParents == std::vector<uint256>(1, uint256(9)));
    BOOST_CHECK(!GetDagTipDeltaState().active);
    fx.Close();
    fx.Reopen();
    CTxDB reopened;
    std::string again;
    BOOST_REQUIRE(reopened.BootstrapDAGSourceStateId(&again));
    uint256 second;
    BOOST_REQUIRE(reopened.ReadDAGSourceStateId(second));
    BOOST_CHECK(second == first);
}

BOOST_AUTO_TEST_CASE(empty_source_gets_persistent_token)
{
    IsolatedTxDB fx;
    CTxDB db;
    std::string error;
    BOOST_REQUIRE(db.BootstrapDAGSourceStateId(&error));
    uint256 first;
    BOOST_REQUIRE(db.ReadDAGSourceStateId(first));
    BOOST_CHECK(first != uint256(0));
    fx.Close(); fx.Reopen();
    CTxDB reopened; uint256 second;
    BOOST_REQUIRE(reopened.BootstrapDAGSourceStateId(&error));
    BOOST_REQUIRE(reopened.ReadDAGSourceStateId(second));
    BOOST_CHECK(second == first);
}

BOOST_AUTO_TEST_CASE(corrupt_token_fails_closed_and_is_not_replaced)
{
    IsolatedTxDB fx;
    PutRepresentativeDaglink();
    PutCorruptToken();
    CTxDB db;
    uint256 ignored;
    BOOST_CHECK(db.HasDAGSourceStateId());
    BOOST_CHECK(!db.ReadDAGSourceStateId(ignored));
    std::string error;
    BOOST_CHECK(!db.BootstrapDAGSourceStateId(&error));
    BOOST_CHECK(!error.empty());
    BOOST_CHECK(db.HasDAGSourceStateId());
    BOOST_CHECK(!db.ReadDAGSourceStateId(ignored));
    fx.Close(); fx.Reopen();
    CTxDB reopened;
    BOOST_CHECK(!reopened.ReadDAGSourceStateId(ignored));
}

BOOST_AUTO_TEST_CASE(legacy_loadblockindex_propagates_bootstrap_failure)
{
    IsolatedTxDB fx;
    g_testFailDAGSourceStateBootstrapMint = true;
    CTxDB db;
    BOOST_CHECK(!db.LoadBlockIndex());
    BOOST_CHECK(!db.HasDAGSourceStateId());
    g_testFailDAGSourceStateBootstrapMint = false;
    BOOST_REQUIRE(db.LoadBlockIndex());
    uint256 token;
    BOOST_REQUIRE(db.ReadDAGSourceStateId(token));
    BOOST_CHECK(token != uint256(0));
}

BOOST_AUTO_TEST_CASE(bootstrap_failure_seams_fail_closed_and_retry)
{
    bool* seams[] = { &g_testFailDAGSourceStateBootstrapMint,
                      &g_testFailDAGSourceStateBootstrapTxnBegin,
                      &g_testFailDAGSourceStateBootstrapTxnCommit };
    for (size_t i = 0; i < sizeof(seams)/sizeof(seams[0]); ++i)
    {
        IsolatedTxDB fx;
        PutRepresentativeDaglink();
        *seams[i] = true;
        CTxDB db;
        std::string error;
        BOOST_CHECK(!db.BootstrapDAGSourceStateId(&error));
        BOOST_CHECK(!error.empty());
        BOOST_CHECK(!db.HasDAGSourceStateId());
        BOOST_CHECK(!GetDagTipDeltaState().active);
        *seams[i] = false;
        BOOST_REQUIRE(db.BootstrapDAGSourceStateId(&error));
        uint256 token;
        BOOST_REQUIRE(db.ReadDAGSourceStateId(token));
        BOOST_CHECK(token != uint256(0));
        fx.Close(); fx.Reopen();
        CTxDB reopened; uint256 same;
        BOOST_REQUIRE(reopened.ReadDAGSourceStateId(same));
        BOOST_CHECK(same == token);
    }
    g_testFailDAGSourceStateBootstrapMint = false;
    g_testFailDAGSourceStateBootstrapTxnBegin = false;
    g_testFailDAGSourceStateBootstrapTxnCommit = false;
}
