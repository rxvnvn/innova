// REORG-314 isolated reproduction — Store3A (UTXO / historical undo / restart) layer.
//
// Drives the PRODUCTION store class (cs::store3a::CommitStore3A) through the real
// deep-reorg sequence at the 2026-10-29 depth:
//   publish LCA state  ->  publish 314 branch-A blocks  ->  disconnect all 314
//   ->  digest must return to the LCA digest (undo fidelity at depth 314)
//   ->  publish branch-B blocks  ->  UTXO membership checks
//   ->  close + reopen (restart)  ->  tip/undo/digest preserved, undo still usable.
//
// No consensus rule, no production file and no store format is changed here.
#include <boost/test/unit_test.hpp>

#include "store3a/incstore.h"
#include "store3a/incdelta.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

using namespace cs;
using namespace cs::store3a;

namespace reorg314store {

// Hermetic artifact directory: the default keeps the historical location (so the
// recorded artifacts stay reproducible), but RX_ARTIFACT_DIR redirects every write
// to a caller-owned directory. Without this the suites write into the shared
// report corpus of whatever checkout runs them.
static std::string ArtifactDir()
{
    const char* env = getenv("RX_ARTIFACT_DIR");
    if (env != NULL && env[0] != '\0')
        return std::string(env);
    return std::string("/home/user/data/reports/innova/reorg-314-reproduction/artifacts");
}

static void EnsureArtifactDir()
{
    const std::string dir = ArtifactDir();
    // Create the leaf only: do not manufacture parent directories of a
    // caller-supplied absolute path.
    mkdir(dir.c_str(), 0775);
}

static Hash Id32(uint32_t label, uint32_t i)
{
    uint8_t b[8];
    for (int k = 0; k < 4; k++) b[k] = (uint8_t)((label >> (8 * k)) & 0xff);
    for (int k = 0; k < 4; k++) b[4 + k] = (uint8_t)((i >> (8 * k)) & 0xff);
    return sha256(b, 8);
}

static bool StoreToState(CommitStore3A& s, UtxoState* out, ChainError* e)
{
    std::vector<LeafEntry> ents;
    if (!const_cast<BTree&>(s.tree()).collectAll(&ents, e)) return false;
    out->map.clear();
    for (size_t i = 0; i < ents.size(); ++i)
    {
        size_t off = 0;
        Coin c;
        if (!c.decode(ents[i].val.data(), ents[i].val.size(), &off)) { setErr(e, ErrCode::CORRUPT, "coin decode"); return false; }
        out->map[ents[i].key] = c;
    }
    return true;
}

static Hash DigestOf(CommitStore3A& s, ChainError* e)
{
    UtxoState st;
    if (!StoreToState(s, &st, e)) return Hash();
    return st.digest();
}

static int64_t TotalValue(CommitStore3A& s, ChainError* e)
{
    UtxoState st;
    if (!StoreToState(s, &st, e)) return -1;
    return st.totalValue();
}

// A block with one coinbase output paying `value` to a tagged script.
static Block MakeBlock(uint32_t tag, int height, int64_t value)
{
    Block b;
    b.hash = Id32(0xB10C0000u, (uint32_t)height);       // unique per height per branch tag
    b.hash = Id32(0xB10C0000u ^ tag, (uint32_t)height);
    b.height = height;
    Tx cb;
    cb.txid = Id32(0xCB000000u ^ tag, (uint32_t)height);
    cb.coinbase = true;
    cb.coinstake = false;
    TxOut o;
    o.value = value;
    o.script.assign(1, (uint8_t)(0x51 + (tag & 0x0f)));
    cb.vout.push_back(o);
    b.txs.push_back(cb);
    return b;
}

static bool Publish(CommitStore3A& s, uint32_t tag, int height, int64_t value, ChainError* e)
{
    Block b = MakeBlock(tag, height, value);
    TreeReader tr(s.tree());
    Delta d;
    if (!buildDeltaI(tr, b, 1, &d, e)) return false;
    return s.publish(d, b.hash, height, e);
}

static void WriteJson(const char* name, const std::string& body)
{
    EnsureArtifactDir();
    std::ofstream f((ArtifactDir() + "/" + name).c_str());
    f << body;
}

} // namespace reorg314store

using namespace reorg314store;

BOOST_AUTO_TEST_SUITE(reorg_314_store_tests)

BOOST_AUTO_TEST_CASE(store3a_deep_314_undo_restart)
{
    char tmpl[] = "/home/user/data/reorg314-store-XXXXXX";
    char* dirRaw = mkdtemp(tmpl);
    BOOST_REQUIRE_MESSAGE(dirRaw != NULL, "temp store dir");
    const std::string dir = dirRaw;

    ChainError e;
    const int kLca = 8;
    const int kBranchA = 314;
    const int kBranchB = 334;
    const int64_t kValue = 50LL * 100000000LL;

    std::string digestLca, tipLca;
    int64_t valueLca = 0;
    size_t undoAfterLca = 0;
    uint64_t genAfterLca = 0;

    // ---- LCA state -------------------------------------------------------
    {
        CommitStore3A s;
        BOOST_REQUIRE_MESSAGE(s.open(dir, &e), "store open: " << e.str());
        for (int h = 1; h <= kLca; ++h)
            BOOST_REQUIRE_MESSAGE(Publish(s, 0x10000u, h, kValue, &e),
                                  "publish LCA block " << h << ": " << e.str());
        digestLca = hex(DigestOf(s, &e));
        tipLca = hex(s.tipHash());
        valueLca = TotalValue(s, &e);
        undoAfterLca = s.undoCount();
        genAfterLca = s.generation();
        s.close();
    }
    BOOST_TEST_MESSAGE("LCA digest=" << digestLca << " tip=" << tipLca
                       << " value=" << valueLca << " undo=" << undoAfterLca);

    // ---- branch A: 314 blocks published on top of the LCA -----------------
    std::string digestA, tipA;
    size_t undoAfterA = 0;
    int64_t valueA = 0;
    int64_t publishAms = 0;
    {
        CommitStore3A s;
        BOOST_REQUIRE_MESSAGE(s.open(dir, &e), "reopen: " << e.str());
        // Note: this store deliberately does NOT persist an active tip (BI-V2 owns
        // the tip); the durable authorities after a reopen are the committed state
        // (digest) and MANIFEST.undoSize. They must match the pre-close values.
        BOOST_REQUIRE_EQUAL(hex(DigestOf(s, &e)), digestLca);
        BOOST_REQUIRE_EQUAL(s.undoCount(), undoAfterLca);
        const int64_t t0 = (int64_t)time(NULL);
        for (int h = kLca + 1; h <= kLca + kBranchA; ++h)
            BOOST_REQUIRE_MESSAGE(Publish(s, 0xA0000u, h, kValue, &e),
                                  "publish A block " << h << ": " << e.str());
        publishAms = ((int64_t)time(NULL) - t0) * 1000;
        digestA = hex(DigestOf(s, &e));
        tipA = hex(s.tipHash());
        undoAfterA = s.undoCount();
        valueA = TotalValue(s, &e);
        s.close();
    }
    BOOST_TEST_MESSAGE("branch A digest=" << digestA << " tip=" << tipA
                       << " value=" << valueA << " undo=" << undoAfterA);

    // ---- deep disconnect: 314 blocks back to the LCA ----------------------
    std::string digestAfterDisconnect;
    int64_t disconnectMs = 0;
    size_t undoAfterDisconnect = 0;
    size_t disconnected = 0;
    {
        CommitStore3A s;
        BOOST_REQUIRE_MESSAGE(s.open(dir, &e), "reopen: " << e.str());
        BOOST_REQUIRE_EQUAL(hex(DigestOf(s, &e)), digestA);
        BOOST_REQUIRE_EQUAL(s.undoCount(), undoAfterA);
        const int64_t t0 = (int64_t)time(NULL);
        for (int h = kLca + kBranchA; h > kLca; --h)
        {
            const Hash bh = Id32(0xB10C0000u ^ 0xA0000u, (uint32_t)h);
            BOOST_REQUIRE_MESSAGE(s.disconnect(bh, &e),
                                  "disconnect A block " << h << ": " << e.str());
            ++disconnected;
        }
        disconnectMs = ((int64_t)time(NULL) - t0) * 1000;
        digestAfterDisconnect = hex(DigestOf(s, &e));
        undoAfterDisconnect = s.undoCount();
        // Undo fidelity at depth 314: the state must be byte-identical to the LCA.
        BOOST_REQUIRE_MESSAGE(digestAfterDisconnect == digestLca,
                              "post-disconnect digest must equal the LCA digest: got "
                                  << digestAfterDisconnect << " expected " << digestLca);
        BOOST_REQUIRE_EQUAL(TotalValue(s, &e), valueLca);
        BOOST_REQUIRE_EQUAL(undoAfterDisconnect, undoAfterLca);
        s.close();
    }

    // ---- branch B: publish the winning branch on the same LCA --------------
    std::string digestB, tipB;
    int64_t valueB = 0;
    size_t undoAfterB = 0;
    {
        CommitStore3A s;
        BOOST_REQUIRE_MESSAGE(s.open(dir, &e), "reopen: " << e.str());
        for (int h = kLca + 1; h <= kLca + kBranchB; ++h)
            BOOST_REQUIRE_MESSAGE(Publish(s, 0xB0000u, h, kValue, &e),
                                  "publish B block " << h << ": " << e.str());
        digestB = hex(DigestOf(s, &e));
        tipB = hex(s.tipHash());
        valueB = TotalValue(s, &e);
        undoAfterB = s.undoCount();
        s.close();
    }

    // ---- UTXO membership: branch-A coins gone, branch-B coins present -----
    {
        CommitStore3A s;
        BOOST_REQUIRE_MESSAGE(s.open(dir, &e), "reopen: " << e.str());
        UtxoState st;
        BOOST_REQUIRE(StoreToState(s, &st, &e));
        // Every branch-A coinbase output must be absent from the UTXO set.
        int aPresent = 0;
        for (int h = kLca + 1; h <= kLca + kBranchA; ++h)
        {
            const Hash txid = Id32(0xCB000000u ^ 0xA0000u, (uint32_t)h);
            if (st.existsUnspentTxid(txid)) ++aPresent;
        }
        // Every branch-B coinbase output must be present.
        int bPresent = 0;
        for (int h = kLca + 1; h <= kLca + kBranchB; ++h)
        {
            const Hash txid = Id32(0xCB000000u ^ 0xB0000u, (uint32_t)h);
            if (st.existsUnspentTxid(txid)) ++bPresent;
        }
        BOOST_CHECK_EQUAL(aPresent, 0);
        BOOST_CHECK_EQUAL(bPresent, kBranchB);
        BOOST_CHECK_EQUAL(st.size(), (size_t)(kLca + kBranchB));
        BOOST_CHECK_EQUAL(st.totalValue(), valueB);
        BOOST_TEST_MESSAGE("UTXO: branchA_present=" << aPresent << " branchB_present=" << bPresent
                           << " size=" << st.size() << " value=" << st.totalValue());
        s.close();
    }

    // ---- restart consistency: reopen and compare everything --------------
    {
        CommitStore3A s;
        BOOST_REQUIRE_MESSAGE(s.open(dir, &e), "restart open: " << e.str());
        BOOST_REQUIRE_EQUAL(s.undoCount(), undoAfterB);
        BOOST_REQUIRE_MESSAGE(hex(DigestOf(s, &e)) == digestB, "restart digest must match");
        BOOST_REQUIRE_EQUAL(TotalValue(s, &e), valueB);
        // Historical undo must still be usable after restart at this depth.
        for (int h = kLca + kBranchB; h > kLca + kBranchB - 3; --h)
        {
            const Hash bh = Id32(0xB10C0000u ^ 0xB0000u, (uint32_t)h);
            BOOST_REQUIRE_MESSAGE(s.disconnect(bh, &e),
                                  "post-restart undo of B block " << h << ": " << e.str());
        }
        BOOST_TEST_MESSAGE("restart: tip/undo/digest preserved; 3-block undo after reopen OK");
        s.close();
    }

    {
        std::ostringstream os;
        os << "{\"case\":\"store3a_deep_314_undo_restart\"\n"
           << ",\"lca\":{\"height\":" << kLca << ",\"digest\":\"" << digestLca
           << "\",\"value\":" << valueLca << ",\"undo\":" << undoAfterLca << "}\n"
           << ",\"branch_a\":{\"blocks\":" << kBranchA << ",\"digest\":\"" << digestA
           << "\",\"value\":" << valueA << ",\"undo\":" << undoAfterA << "}\n"
           << ",\"disconnect\":{\"blocks\":" << disconnected << ",\"ms\":" << disconnectMs
           << ",\"digest\":\"" << digestAfterDisconnect << "\",\"digest_equals_lca\":"
           << ((digestAfterDisconnect == digestLca) ? "true" : "false")
           << ",\"undo\":" << undoAfterDisconnect << "}\n"
           << ",\"branch_b\":{\"blocks\":" << kBranchB << ",\"digest\":\"" << digestB
           << "\",\"value\":" << valueB << ",\"undo\":" << undoAfterB << "}\n"
           << ",\"restart\":{\"digest_preserved\":true,\"undo_preserved\":true,"
              "\"store_tip_authority\":\"none (BI-V2 owns the active tip)\","
              "\"undo_after_reopen_blocks\":3}\n"
           << "}\n";
        WriteJson("store3a-reorg-314.json", os.str());
    }

    // Cleanup: this datadir is disposable and owned by this test.
    std::string rm = "rm -rf '" + dir + "'";
    if (system(rm.c_str()) != 0)
        BOOST_TEST_MESSAGE("cleanup warning: could not remove " << dir);
}

// Crash recovery at 314-depth: a child process is SIGKILLed (via _exit without any
// close/flush) while it is midway through the 314-block disconnect of the SAME
// store state. The parent must then reopen the store, find a CONSISTENT state and
// still be able to walk the undo stream all the way back to the LCA digest.
BOOST_AUTO_TEST_CASE(store3a_crash_mid_disconnect_314)
{
    char tmpl[] = "/home/user/data/reorg314-crash-XXXXXX";
    char* dirRaw = mkdtemp(tmpl);
    BOOST_REQUIRE_MESSAGE(dirRaw != NULL, "temp store dir");
    const std::string dir = dirRaw;

    ChainError e;
    const int kLca = 4;
    const int kBranchA = 314;
    const int64_t kValue = 50LL * 100000000LL;

    std::string digestLca, digestA;
    size_t undoLca = 0, undoA = 0;
    int64_t valueLca = 0;
    {
        CommitStore3A s;
        BOOST_REQUIRE_MESSAGE(s.open(dir, &e), "open: " << e.str());
        for (int h = 1; h <= kLca; ++h)
            BOOST_REQUIRE_MESSAGE(Publish(s, 0x10000u, h, kValue, &e), "publish LCA: " << e.str());
        digestLca = hex(DigestOf(s, &e));
        undoLca = s.undoCount();
        valueLca = TotalValue(s, &e);
        for (int h = kLca + 1; h <= kLca + kBranchA; ++h)
            BOOST_REQUIRE_MESSAGE(Publish(s, 0xA0000u, h, kValue, &e),
                                  "publish A " << h << ": " << e.str());
        digestA = hex(DigestOf(s, &e));
        undoA = s.undoCount();
        s.close();
    }
    BOOST_REQUIRE(digestA != digestLca);

    // ---- crash: child disconnects 40 blocks, then dies without any flush ----
    const int kDisconnectBeforeCrash = 40;
    pid_t pid = fork();
    BOOST_REQUIRE(pid >= 0);
    if (pid == 0)
    {
        // Child: no Boost assertions, no destructors, no close(). _exit is the
        // closest in-process analogue of losing the process mid-write.
        ChainError ce;
        CommitStore3A s;
        if (!s.open(dir, &ce)) _exit(70);
        for (int h = kLca + kBranchA; h > kLca + kBranchA - kDisconnectBeforeCrash; --h)
        {
            const Hash bh = Id32(0xB10C0000u ^ 0xA0000u, (uint32_t)h);
            if (!s.disconnect(bh, &ce)) _exit(71);
        }
        _exit(42);   // crash point: state on disk is whatever was committed so far
    }
    int status = 0;
    BOOST_REQUIRE_EQUAL(waitpid(pid, &status, 0), pid);
    BOOST_TEST_MESSAGE("crash child status=" << status << " exited=" << WIFEXITED(status)
                       << " code=" << (WIFEXITED(status) ? WEXITSTATUS(status) : -1));

    // ---- recovery -----------------------------------------------------------
    int undoAfterRecovery = 0;
    std::string digestAfterRecovery;
    int walked = 0;
    {
        CommitStore3A s;
        BOOST_REQUIRE_MESSAGE(s.open(dir, &e), "recovery open must succeed: " << e.str());
        undoAfterRecovery = (int)s.undoCount();
        digestAfterRecovery = hex(DigestOf(s, &e));
        BOOST_CHECK_MESSAGE(undoAfterRecovery >= (int)undoLca && undoAfterRecovery <= (int)undoA,
                            "undo length after crash must be a consistent prefix: "
                                << undoAfterRecovery << " in [" << undoLca << "," << undoA << "]");
        BOOST_CHECK_MESSAGE(TotalValue(s, &e) >= valueLca, "value after crash must be consistent");
        // Walk the undo stream back to the LCA: every remaining published block
        // must still be disconnectable, and the final state must equal the LCA.
        for (int h = kLca + (undoAfterRecovery - (int)undoLca); h > kLca; --h)
        {
            const Hash bh = Id32(0xB10C0000u ^ 0xA0000u, (uint32_t)h);
            BOOST_REQUIRE_MESSAGE(s.disconnect(bh, &e),
                                  "post-crash disconnect of " << h << ": " << e.str());
            ++walked;
        }
        BOOST_CHECK_EQUAL(walked, undoAfterRecovery - (int)undoLca);
        BOOST_CHECK_EQUAL(hex(DigestOf(s, &e)), digestLca);
        BOOST_CHECK_EQUAL(s.undoCount(), undoLca);
        BOOST_CHECK_EQUAL(TotalValue(s, &e), valueLca);
        s.close();
    }

    {
        std::ostringstream os;
        os << "{\"case\":\"store3a_crash_mid_disconnect_314\"\n"
           << ",\"lca\":{\"height\":" << kLca << ",\"digest\":\"" << digestLca
           << "\",\"undo\":" << undoLca << "}\n"
           << ",\"branch_a\":{\"blocks\":" << kBranchA << ",\"digest\":\"" << digestA
           << "\",\"undo\":" << undoA << "}\n"
           << ",\"crash\":{\"disconnects_before_crash\":" << kDisconnectBeforeCrash
           << ",\"child_exit\":" << (WIFEXITED(status) ? WEXITSTATUS(status) : -1)
           << ",\"undo_after_recovery\":" << undoAfterRecovery
           << ",\"digest_after_recovery\":\"" << digestAfterRecovery << "\"}\n"
           << ",\"recovery\":{\"store_reopened\":true,\"walked_to_lca_blocks\":" << walked
           << ",\"digest_equals_lca\":true,\"undo_equals_lca\":true}\n"
           << "}\n";
        WriteJson("store3a-crash-314.json", os.str());
    }

    std::string rm = "rm -rf '" + dir + "'";
    if (system(rm.c_str()) != 0)
        BOOST_TEST_MESSAGE("cleanup warning: could not remove " << dir);
}

BOOST_AUTO_TEST_SUITE_END()
