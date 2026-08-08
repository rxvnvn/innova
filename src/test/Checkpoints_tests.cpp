//
// Unit tests for block-chain checkpoints
//
#include <boost/foreach.hpp>
#include <boost/test/unit_test.hpp>

#include "../checkpoints.h"
#include "../kernel.h"
#include "../util.h"

using namespace std;

BOOST_AUTO_TEST_SUITE(Checkpoints_tests)

BOOST_AUTO_TEST_CASE(regtest_ignores_mainnet_hardened_checkpoint)
{
    // Regression test for the regtest checkpoint bug: the mainnet hardened
    // checkpoint at height 2000 was being applied during regtest because
    // CheckHardened() did not exclude fRegTest, so a regtest chain could not
    // grow past height 1999 (mining loop rejected every block at height 2000).
    const uint256 wrong("0x0000000000000000000000000000000000000000000000000000000000000001");

    // Sanity: 2000 IS a mainnet hardened checkpoint, so a wrong hash must be
    // rejected on mainnet. This also pins mainnet semantics against regress.
    {
        bool fRegSaved = fRegTest;
        bool fTestSaved = fTestNet;
        fRegTest = false;
        fTestNet = false;
        BOOST_CHECK(!Checkpoints::CheckHardened(2000, wrong));
        fRegTest = fRegSaved;
        fTestNet = fTestSaved;
    }

    // The fix: regtest must bypass hardened checkpoints entirely.
    {
        bool fRegSaved = fRegTest;
        bool fTestSaved = fTestNet;
        fRegTest = true;
        fTestNet = false;
        BOOST_CHECK(Checkpoints::CheckHardened(2000, wrong));
        BOOST_CHECK(Checkpoints::CheckHardened(5000, wrong));
        fRegTest = fRegSaved;
        fTestNet = fTestSaved;
    }
}

BOOST_AUTO_TEST_CASE(mainnet_hardened_checkpoint_semantics_unchanged)
{
    // Verify mainnet hardened-checkpoint behavior is untouched by the regtest
    // fix: the stored checkpoint hash at a known height must pass and any
    // other hash must fail.
    const uint256 h2000 = Checkpoints::mapCheckpoints[2000];
    const uint256 wrong("0x0000000000000000000000000000000000000000000000000000000000000001");
    BOOST_CHECK(h2000 != 0);
    BOOST_CHECK(h2000 != wrong);

    bool fRegSaved = fRegTest;
    bool fTestSaved = fTestNet;
    fRegTest = false;
    fTestNet = false;

    BOOST_CHECK(Checkpoints::CheckHardened(2000, h2000));
    BOOST_CHECK(!Checkpoints::CheckHardened(2000, wrong));
    // A height that is not a checkpoint must accept any hash.
    BOOST_CHECK(Checkpoints::CheckHardened(2001, wrong));

    fRegTest = fRegSaved;
    fTestNet = fTestSaved;
}

BOOST_AUTO_TEST_CASE(testnet_uses_own_checkpoint_map)
{
    // Testnet has no checkpoints beyond genesis; any non-genesis height must
    // accept any hash regardless of the mainnet table.
    const uint256 h0 = Checkpoints::mapCheckpointsTestnet[0];
    const uint256 wrong("0x0000000000000000000000000000000000000000000000000000000000000001");
    BOOST_CHECK(h0 != 0);

    bool fRegSaved = fRegTest;
    bool fTestSaved = fTestNet;
    fRegTest = false;
    fTestNet = true;

    BOOST_CHECK(Checkpoints::CheckHardened(0, h0));
    BOOST_CHECK(!Checkpoints::CheckHardened(0, wrong));
    BOOST_CHECK(Checkpoints::CheckHardened(2000, wrong));

    fRegTest = fRegSaved;
    fTestNet = fTestSaved;
}

BOOST_AUTO_TEST_CASE(regtest_ignores_stake_modifier_checkpoint)
{
    const unsigned int wrong = 0x12345678;
    bool fRegSaved = fRegTest;
    bool fTestSaved = fTestNet;

    fRegTest = false;
    fTestNet = false;
    BOOST_CHECK(!CheckStakeModifierCheckpoints(100000, wrong));

    fRegTest = true;
    fTestNet = false;
    BOOST_CHECK(CheckStakeModifierCheckpoints(100000, wrong));

    fRegTest = fRegSaved;
    fTestNet = fTestSaved;
}

BOOST_AUTO_TEST_SUITE_END()
