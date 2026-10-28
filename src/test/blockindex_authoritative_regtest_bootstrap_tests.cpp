// REGRESSION — the TestingSetup global fixture (test_innova.cpp) establishes a
// valid authoritative regtest baseline on a FRESH temp datadir by following the
// exact production startup sequence (init.cpp): classify block-index ownership
// -> EMPTY_NEW native genesis bootstrap (PrepareNativeBlockIndexGeneration,
// which builds+validates+publishes+SELECTS the first V2 generation,
// fail-closed) -> real InitBlockIndexAuthoritative via LoadBlockIndex(true).
//
// Before the fixture fix, LoadBlockIndex(true) returned false with
// pindexBest/pindexGenesisBlock == NULL on an empty datadir (BlockIndexStartup
// Bootstrap had no published V2 generation to open), and every suite that
// dereferences the tip crashed.
#include <boost/test/unit_test.hpp>

#include "main.h"
#include "blockindex_authoritative_startup.h"
#include "blockindex_startup_ownership.h"
#include "blockindex_accessor.h"

BOOST_AUTO_TEST_SUITE(blockindex_authoritative_regtest_bootstrap_tests)

BOOST_AUTO_TEST_CASE(fixture_asserts_authoritative_regtest_baseline)
{
    // 1. Tip / genesis / height populated by the global fixture.
    BOOST_REQUIRE_MESSAGE(pindexBest != NULL,
                          "TestingSetup must leave pindexBest non-NULL on a fresh regtest datadir");
    BOOST_REQUIRE_MESSAGE(pindexGenesisBlock != NULL,
                          "TestingSetup must leave pindexGenesisBlock non-NULL on a fresh regtest datadir");
    BOOST_REQUIRE_MESSAGE(pindexBest->nHeight >= 0,
                          "TestingSetup must leave a valid best height");

    // 2. The tip chain IS the regtest genesis chain (byte-identical genesis:
    //    produced through the shared genesis primitive, not hand-rolled).
    BOOST_REQUIRE_MESSAGE(pindexBest->GetBlockHash() == GetGenesisBlockHash(),
                          "regtest tip must be the canonical regtest genesis block");
    BOOST_REQUIRE_MESSAGE(pindexGenesisBlock->GetBlockHash() == GetGenesisBlockHash(),
                          "regtest genesis pointer must carry the canonical regtest genesis hash");
    BOOST_REQUIRE_EQUAL(pindexBest->nHeight, 0);

    // 3. BI-V2 authority is genuinely open/authoritative via the REAL API:
    //    the authoritative startup flag is armed and a by-height resolve of
    //    height 0 through the authoritative resolver returns the genesis hash.
    BOOST_REQUIRE_MESSAGE(g_fAuthoritativeStartup,
                          "authoritative V2 startup must be active after TestingSetup");

    BlockIndexSnapshot snap;
    std::string err;
    const AuthoritativeBlockResolutionResult r =
        AuthoritativeGetActiveSnapshotByHeightR(0, &snap, &err);
    BOOST_REQUIRE_MESSAGE(r == AUTHORITATIVE_BLOCK_FOUND, err);
    BOOST_REQUIRE(snap.found);
    BOOST_REQUIRE_MESSAGE(snap.hash == GetGenesisBlockHash(),
                          "authoritative by-height(0) resolve must return the regtest genesis hash");
}

BOOST_AUTO_TEST_SUITE_END()
