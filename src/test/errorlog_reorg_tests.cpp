// P0 regression tests for:
//  (A) error()/errorN() must reach the durable debug.log unconditionally
//      (independent of -debug / console mode) — fixes log-loss where ERROR lines
//      appeared only in the terminal (stderr).
//  (B) Reorganize's by-value ancestor materialization helper for deep reorgs.
//
// These run against the shared authoritative-regtest TestingSetup fixture.
#include <boost/test/unit_test.hpp>
#include <fstream>
#include <sstream>

#include "main.h"
#include "util.h"
#include "blockindex_accessor.h"
#include "blockindex_authoritative_live.h"

extern CBlockIndex* MaterializeCBlockIndexByValue(const BlockIndexSnapshot& s, uint256* ownHash);

BOOST_AUTO_TEST_SUITE(errorlog_reorg_tests)

static std::string ReadDebugLog()
{
    boost::filesystem::path p = GetDataDir() / "debug.log";
    std::ifstream ifs(p.string().c_str());
    std::ostringstream oss;
    if (ifs) oss << ifs.rdbuf();
    return oss.str();
}

// 0-arg error() binds to the util.cpp variadic overload (historically wrote ONLY to
// stderr). The fix routes it through LogErrorAlways, so with -debug=0 it must still
// reach debug.log.
BOOST_AUTO_TEST_CASE(error_zero_arg_reaches_debuglog_without_debug)
{
    const bool savedDebug = fDebug;
    const bool savedConsole = fPrintToConsole;
    fDebug = false;          // simulate running without -debug
    fPrintToConsole = false; // and without -printtoconsole

    error("LOGTEST_ZEROARG_MSG");

    fDebug = savedDebug;
    fPrintToConsole = savedConsole;

    const std::string log = ReadDebugLog();
    BOOST_CHECK_MESSAGE(log.find("ERROR: LOGTEST_ZEROARG_MSG") != std::string::npos,
                        "0-arg error() must be present in debug.log without -debug");
}

// N-arg error() (tinyformat template) must also reach debug.log without -debug, and
// errorN() (used by DoS/score rejects) must too.
BOOST_AUTO_TEST_CASE(error_narg_and_errorn_reach_debuglog_without_debug)
{
    const bool savedDebug = fDebug;
    const bool savedConsole = fPrintToConsole;
    fDebug = false;
    fPrintToConsole = false;

    error("LOGTEST_FMT_MSG value=%d", 42);
    errorN(-101, "LOGTEST_ERRN_MSG");

    fDebug = savedDebug;
    fPrintToConsole = savedConsole;

    const std::string log = ReadDebugLog();
    BOOST_CHECK_MESSAGE(log.find("LOGTEST_FMT_MSG value=42") != std::string::npos,
                        "N-arg error() with format args must reach debug.log (value=42)");
    BOOST_CHECK_MESSAGE(log.find("ERROR: LOGTEST_ERRN_MSG") != std::string::npos,
                        "errorN() must reach debug.log");
}

// Console mode (-printtoconsole) routes ERROR to the terminal (stderr), NOT to the
// durable debug.log: the file is never duplicated, and normal file logging is the
// NO-terminal path. This matches the operator requirement (ERROR in debug.log but
// not the terminal; terminal only under explicit -printtoconsole or file fallback).
BOOST_AUTO_TEST_CASE(error_console_mode_goes_to_terminal_not_file)
{
    const bool savedConsole = fPrintToConsole;
    const bool savedDebug = fDebug;
    fPrintToConsole = true;
    fDebug = false;

    error("LOGTEST_CONSOLE_ROUTED_MSG");

    fPrintToConsole = savedConsole;
    fDebug = savedDebug;

    const std::string log = ReadDebugLog();
    BOOST_CHECK_MESSAGE(log.find("LOGTEST_CONSOLE_ROUTED_MSG") == std::string::npos,
        "console mode (-printtoconsole) must route ERROR to the terminal and NOT duplicate it into debug.log");
}

// Task A helper: a by-value snapshot materializes a correct full-topology CBlockIndex
// used to traverse ancestors below the residency floor during a deep reorg.
BOOST_AUTO_TEST_CASE(reorg_materialize_byvalue_index)
{
    BlockIndexSnapshot s;
    s.found = true;
    s.height = 123456;
    s.nFile = 7;
    s.nBlockPos = 999;
    // fixed pseudo-hashes
    s.hash.SetHex("4142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f60");
    s.hashPrev.SetHex("8182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9fa0");
    s.hashPrev = s.hashPrev; // keep distinct
    s.hashMerkleRoot = uint256();
    s.nVersion = 1;
    s.nTime = 1700000000;
    s.nBits = 0x1d00ffff;
    s.nNonce = 7;
    s.fProofOfStake = false;
    s.hasParent = true;
    s.hasStakeModifierTime = false;
    s.hasStakeModifierChecksum = false;

    uint256 ownHash;
    CBlockIndex* p = MaterializeCBlockIndexByValue(s, &ownHash);
    BOOST_REQUIRE(p != NULL);
    BOOST_CHECK(p->nHeight == 123456);
    BOOST_CHECK(p->nFile == 7);
    BOOST_CHECK(p->nBlockPos == 999);
    BOOST_CHECK(p->GetBlockHash() == s.hash);
    BOOST_CHECK(p->hashPrevStable == s.hashPrev);
    BOOST_CHECK(p->pprev == NULL); // linked by the walk
    delete p;
}

BOOST_AUTO_TEST_SUITE_END()