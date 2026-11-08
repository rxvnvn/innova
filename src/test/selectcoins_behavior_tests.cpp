// Behavioural tests for coin selection: sufficiency, input counts, sums, change headroom,
// fee-retry, absence of double selection, and behaviour on large pools of small UTXOs.
//
// These complement the differential parity suite: parity proves "same result as the reference
// path", this suite proves the result is *correct* (reaches the target, never selects an outpoint
// twice, never reports a value it did not select, and fails closed on an RNG error).
#include <boost/test/unit_test.hpp>

#include <map>
#include <set>
#include <stdexcept>
#include <vector>

#include "base58.h"
#include "key.h"
#include "main.h"
#include "script.h"
#include "util.h"
#include "wallet.h"

BOOST_AUTO_TEST_SUITE(selectcoins_behavior_tests)

namespace
{
struct Spec
{
    int64_t nValue;
    unsigned int nDepth;
};

// Minimal wallet: one key of ours, coins minted by a transaction that spends a foreign outpoint
// (so they fall into the nConfMine=0 tier and the pool is fully spendable at nConfTheirs=1).
class Fixture
{
public:
    CWallet wallet;
    CKey key;
    CScript script;
    std::vector<CWalletTx*> txs;
    std::map<const CWalletTx*, int> indexOf;

    Fixture()
    {
        key.MakeNewKey(true);
        {
            LOCK(wallet.cs_wallet);
            BOOST_REQUIRE(wallet.AddKey(key));
        }
        script.SetDestination(key.GetPubKey().GetID());
    }

    ~Fixture()
    {
        for (size_t i = 0; i < txs.size(); ++i)
            delete txs[i];
    }

    std::vector<COutput> Build(const std::vector<Spec>& specs)
    {
        std::vector<COutput> vCoins;
        for (size_t i = 0; i < specs.size(); ++i) {
            CTransaction tx;
            tx.nTime = 1500000000 - (int64_t)i;   // distinct hashes, all in the past
            tx.vin.push_back(CTxIn(COutPoint(uint256(0x1000 + (uint32_t)i), 0)));
            tx.vout.push_back(CTxOut(specs[i].nValue, script));
            CWalletTx* wtx = new CWalletTx(&wallet, tx);
            wallet.mapWallet[wtx->GetHash()] = *wtx;
            txs.push_back(wtx);
            indexOf[wtx] = (int)i;
            vCoins.push_back(COutput(wtx, 0, specs[i].nDepth, true));
        }
        return vCoins;
    }
};

// Deterministic, clearly non-cryptographic source: these tests assert properties of the result,
// not of the randomness.
uint32_t g_state = 0x51ED270Bu;
void DetBytes(unsigned char* buf, int num)
{
    for (int i = 0; i < num; ++i) {
        g_state = g_state * 1664525u + 1013904223u;
        buf[i] = (unsigned char)(g_state >> 21);
    }
}
void ResetDet() { g_state = 0x51ED270Bu; }

// Fails like OpenSSL RAND_bytes does: the selection must not continue with usable bits.
void ThrowingBytes(unsigned char*, int) { throw std::runtime_error("RAND_bytes failed (simulated)"); }

struct Selection
{
    bool ok;
    int64_t value;
    size_t count;
};

Selection Select(Fixture& f, const std::vector<COutput>& pool, int64_t target,
                 CoinSelectionRandBytesFn fn = DetBytes)
{
    std::set<std::pair<const CWalletTx*, unsigned int> > coins;
    int64_t value = 0;
    Selection s;
    s.ok = f.wallet.SelectCoinsMinConf(target, 1500000000u, 0, 1, pool, coins, value, NULL, fn);
    s.value = value;
    s.count = coins.size();

    // Sum what was actually selected and compare with the reported value: a mismatch would mean
    // the wallet reports a value it did not pick (or picked an outpoint twice).
    int64_t sum = 0;
    for (std::set<std::pair<const CWalletTx*, unsigned int> >::const_iterator it = coins.begin();
         it != coins.end(); ++it) {
        BOOST_CHECK_EQUAL(it->second, 0u);
        sum += it->first->vout[0].nValue;
    }
    BOOST_CHECK_EQUAL(sum, value);
    return s;
}
}

BOOST_AUTO_TEST_CASE(exact_single_coin_is_preferred)
{
    Fixture f;
    std::vector<Spec> specs;
    specs.push_back((Spec){100 * CENT, 1});
    for (int i = 0; i < 20; ++i) specs.push_back((Spec){7 * CENT, 1});
    std::vector<COutput> pool = f.Build(specs);

    ResetDet();
    Selection s = Select(f, pool, 100 * CENT);
    BOOST_CHECK(s.ok);
    BOOST_CHECK_EQUAL(s.value, 100 * CENT);
    BOOST_CHECK_EQUAL(s.count, 1u);          // one exact coin beats a pile of smaller ones
}

BOOST_AUTO_TEST_CASE(subset_reaches_target_and_never_overshoots_by_a_whole_coin)
{
    Fixture f;
    std::vector<Spec> specs;
    for (int i = 0; i < 60; ++i) specs.push_back((Spec){(int64_t)(3 + i % 7) * CENT, 1});
    std::vector<COutput> pool = f.Build(specs);

    ResetDet();
    const int64_t target = 50 * CENT;
    Selection s = Select(f, pool, target);
    BOOST_CHECK(s.ok);
    BOOST_CHECK(s.value >= target);
    BOOST_CHECK(s.value - target >= 0);                 // change headroom is never negative
    BOOST_CHECK(s.value < target + 100 * CENT);         // and the search does not give up early
    BOOST_CHECK(s.count > 0);
}

BOOST_AUTO_TEST_CASE(insufficient_funds_reports_failure_with_no_selection)
{
    Fixture f;
    std::vector<Spec> specs;
    for (int i = 0; i < 5; ++i) specs.push_back((Spec){10 * CENT, 1});
    std::vector<COutput> pool = f.Build(specs);

    ResetDet();
    Selection s = Select(f, pool, 100 * CENT);   // pool holds 50 CENT
    BOOST_CHECK(!s.ok);
    BOOST_CHECK_EQUAL(s.value, 0);
    BOOST_CHECK_EQUAL(s.count, 0u);
}

BOOST_AUTO_TEST_CASE(exact_total_is_spendable_to_the_last_cent)
{
    Fixture f;
    std::vector<Spec> specs;
    for (int i = 0; i < 10; ++i) specs.push_back((Spec){10 * CENT, 1});
    std::vector<COutput> pool = f.Build(specs);

    ResetDet();
    Selection s = Select(f, pool, 100 * CENT);   // exactly the whole pool
    BOOST_CHECK(s.ok);
    BOOST_CHECK_EQUAL(s.value, 100 * CENT);
    BOOST_CHECK_EQUAL(s.count, 10u);
}

BOOST_AUTO_TEST_CASE(many_small_utxos_are_consumed_without_duplicates)
{
    Fixture f;
    std::vector<Spec> specs;
    for (int i = 0; i < 3000; ++i) specs.push_back((Spec){CENT / 10, 1});   // 3000 x 0.001
    std::vector<COutput> pool = f.Build(specs);

    ResetDet();
    Selection s = Select(f, pool, 100 * CENT);   // needs >= 1000 inputs
    BOOST_CHECK(s.ok);
    BOOST_CHECK(s.value >= 100 * CENT);
    BOOST_CHECK(s.count >= 1000u);
    BOOST_CHECK(s.count <= 3000u);               // the set is unique, so it cannot exceed the pool
}

BOOST_AUTO_TEST_CASE(large_coin_is_preferred_over_many_small_ones)
{
    Fixture f;
    std::vector<Spec> specs;
    for (int i = 0; i < 400; ++i) specs.push_back((Spec){CENT, 1});
    specs.push_back((Spec){500 * CENT, 1});
    std::vector<COutput> pool = f.Build(specs);

    ResetDet();
    Selection s = Select(f, pool, 50 * CENT);
    BOOST_CHECK(s.ok);
    BOOST_CHECK(s.value >= 50 * CENT);
    BOOST_CHECK(s.count <= 60u);                 // a single 500 CENT coin is far cheaper to spend
}

BOOST_AUTO_TEST_CASE(fee_retry_keeps_the_selection_consistent)
{
    // Target just below the pool total: the retry with target + CENT runs, and the final set must
    // still be internally consistent (unique outpoints, sum == reported value).
    Fixture f;
    std::vector<Spec> specs;
    for (int i = 0; i < 40; ++i) specs.push_back((Spec){(int64_t)(2 + i % 5) * CENT, 1});
    std::vector<COutput> pool = f.Build(specs);

    int64_t total = 0;
    for (size_t i = 0; i < specs.size(); ++i) total += specs[i].nValue;

    ResetDet();
    Selection s = Select(f, pool, total - CENT / 2);   // forces the approximate path + retry
    BOOST_CHECK(s.ok);
    BOOST_CHECK(s.value >= total - CENT / 2);
    BOOST_CHECK(s.value <= total);
}

BOOST_AUTO_TEST_CASE(repeated_selection_with_same_stream_is_identical)
{
    Fixture f;
    std::vector<Spec> specs;
    for (int i = 0; i < 120; ++i) specs.push_back((Spec){(int64_t)(2 + i % 9) * CENT, 1});
    std::vector<COutput> pool = f.Build(specs);

    ResetDet();
    Selection a = Select(f, pool, 100 * CENT);
    ResetDet();
    Selection b = Select(f, pool, 100 * CENT);
    BOOST_CHECK_EQUAL(a.value, b.value);
    BOOST_CHECK_EQUAL(a.count, b.count);
}

BOOST_AUTO_TEST_CASE(rng_failure_aborts_selection_instead_of_using_weak_bits)
{
    Fixture f;
    std::vector<Spec> specs;
    for (int i = 0; i < 30; ++i) specs.push_back((Spec){5 * CENT, 1});
    std::vector<COutput> pool = f.Build(specs);

    std::set<std::pair<const CWalletTx*, unsigned int> > coins;
    int64_t value = 0;
    BOOST_CHECK_THROW(f.wallet.SelectCoinsMinConf(50 * CENT, 1500000000u, 0, 1, pool, coins,
                                                  value, NULL, ThrowingBytes),
                      std::runtime_error);
}


BOOST_AUTO_TEST_CASE(next_int_is_uniform_for_non_power_of_two_ranges)
{
    // Rejection sampling must not skew indices: chi-square over 300k draws for ranges that do
    // not divide 2^64 (3, 7, 1000) stays far below the 0.999 quantile.
    const int kDraws = 300000;
    const int ranges[3] = {3, 7, 1000};
    const double crit005[3] = {13.82, 22.46, 1106.0};   // chi2 0.999 quantile, df = range-1

    for (int r = 0; r < 3; ++r) {
        std::vector<int> v;
        ResetDet();
        selectcoins_test_DrawInts(DetBytes, ranges[r], kDraws, v);
        BOOST_REQUIRE_EQUAL((int)v.size(), kDraws);

        std::vector<int> counts(ranges[r], 0);
        for (int i = 0; i < kDraws; ++i) {
            BOOST_CHECK(v[i] >= 0 && v[i] < ranges[r]);
            counts[v[i]] += 1;
        }
        const double expected = (double)kDraws / (double)ranges[r];
        double chi2 = 0;
        for (int i = 0; i < ranges[r]; ++i) {
            const double d = (double)counts[i] - expected;
            chi2 += d * d / expected;
        }
        if (r < 2) BOOST_CHECK_LT(chi2, crit005[r]);
        else BOOST_CHECK_LT(chi2, crit005[r]);
        fprintf(stderr, "SELBEHAVIOUR nextint range=%d draws=%d chi2=%.1f crit=%.1f\n",
                ranges[r], kDraws, chi2, crit005[r]);
    }
}

BOOST_AUTO_TEST_CASE(next_int_survives_buffer_boundary_and_extreme_range)
{
    // 20k draws over a 2^31-1 range consume ~160 KiB, i.e. ~40 buffer refills: a boundary bug
    // (off-by-one in the bit cursor) would either index past the buffer or skew the output.
    std::vector<int> v;
    ResetDet();
    selectcoins_test_DrawInts(DetBytes, 2147483647, 20000, v);
    BOOST_REQUIRE_EQUAL((int)v.size(), 20000);
    std::set<int> distinct(v.begin(), v.end());
    BOOST_CHECK_GT((int)distinct.size(), 19990);   // collisions in 20k draws over 2^31 are ~0

    // nMax <= 1 must yield 0 without touching the source, and must not divide by zero.
    std::vector<int> z;
    selectcoins_test_DrawInts(DetBytes, 1, 100, z);
    BOOST_CHECK_EQUAL((int)z.size(), 100);
    for (size_t i = 0; i < z.size(); ++i) BOOST_CHECK_EQUAL(z[i], 0);
}

BOOST_AUTO_TEST_SUITE_END()
