// Differential parity for the SelectCoins IsFromMe memoization.
//
// The optimization replaces a per-coin (per pass, per tier) IsFromMe() call with one cached
// answer per transaction. To prove the selected set, the value and the failure behaviour are
// unchanged, the same wallet fixture and the same RNG seed are fed to:
//   * the reference path  -> SelectCoinsMinConf(..., pIsFromMeCache = NULL)  (old behaviour)
//   * the optimized path  -> SelectCoinsMinConf(..., &cache)
// and the results are compared exactly.
//
// Cases: mixed "ours"/"foreign" transactions (so both nConfMine and nConfTheirs tiers run),
// coins filtered by depth and by timestamp, exact-value match, denominated amounts (tryDenom),
// ordinary subset-sum above the target, and insufficient funds.

#include <boost/test/unit_test.hpp>

#include <map>
#include <set>
#include <vector>

#include "base58.h"
#include "key.h"
#include "main.h"
#include "script.h"
#include "util.h"
#include "wallet.h"

BOOST_AUTO_TEST_SUITE(selectcoins_parity_tests)

// Restores the production hook no matter how the case ends.
struct HookGuard
{
    HookGuard() {}
    ~HookGuard() { pfnSelectCoinsRandBytes = GetRandBytes; }
};

namespace { static const int64_t CENT_7 = 7 * CENT; static const int64_t CENT_10 = 10 * CENT; }

namespace
{
struct CoinSpec
{
    int64_t nValue;
    unsigned int nDepth;
    int64_t nOffsetFromTip;   // tx timestamp relative to the spend time (negative = older)
    bool fFromMe;             // whether this transaction spends our own output
    bool fSpendable;
};

// Builds a wallet with two keys and a set of CWalletTx paying to key1, some of which spend an
// output of ours (fFromMe = true) and some of which spend a foreign outpoint (fFromMe = false).
class WalletFixture
{
public:
    CWallet wallet;
    CKey keyPay;
    CKey keySrc;
    CScript scriptPay;
    CScript scriptSrc;
    std::vector<CWalletTx*> txs;
    std::map<const CWalletTx*, int> indexOf;

    WalletFixture()
    {
        keyPay.MakeNewKey(true);
        keySrc.MakeNewKey(true);
        {
            LOCK(wallet.cs_wallet);
            BOOST_REQUIRE(wallet.AddKey(keyPay));
            BOOST_REQUIRE(wallet.AddKey(keySrc));
        }
        scriptPay.SetDestination(keyPay.GetPubKey().GetID());
        scriptSrc.SetDestination(keySrc.GetPubKey().GetID());

        // One "source" output of ours, used as the input of every fFromMe transaction.
        CTransaction source;
        source.nTime = 1500000000;
        source.vin.push_back(CTxIn(COutPoint(uint256(0xaaaa), 0)));
        source.vout.push_back(CTxOut(100 * COIN, scriptSrc));
        CWalletTx wtxSource(&wallet, source);
        wallet.mapWallet[wtxSource.GetHash()] = wtxSource;
        sourceHash = wtxSource.GetHash();
    }

    ~WalletFixture()
    {
        for (size_t i = 0; i < txs.size(); ++i)
            delete txs[i];
    }

    uint256 sourceHash;

    CWalletTx* MakeCoin(const CoinSpec& spec, unsigned int salt)
    {
        CTransaction tx;
        tx.nTime = 1500000000 + spec.nOffsetFromTip;
        if (spec.fFromMe)
            tx.vin.push_back(CTxIn(COutPoint(sourceHash, 0)));
        else
            tx.vin.push_back(CTxIn(COutPoint(uint256(0xbbbb + salt), 0)));
        tx.vout.push_back(CTxOut(spec.nValue, scriptPay));
        CWalletTx* wtx = new CWalletTx(&wallet, tx);
        wallet.mapWallet[wtx->GetHash()] = *wtx;
        txs.push_back(wtx);
        return wtx;
    }

    std::vector<COutput> BuildPool(const std::vector<CoinSpec>& specs)
    {
        std::vector<COutput> vCoins;
        for (size_t i = 0; i < specs.size(); ++i)
        {
            CWalletTx* wtx = MakeCoin(specs[i], (unsigned int)i);
            indexOf[wtx] = (int)i;
            COutput out(wtx, 0, specs[i].nDepth, true);
            out.fSpendable = specs[i].fSpendable;
            vCoins.push_back(out);
        }
        return vCoins;
    }
};

struct Result
{
    bool fOk;
    std::set<int> indices;   // coin indices, fixture independent
    int64_t nValue;

    bool operator==(const Result& o) const
    {
        return fOk == o.fOk && nValue == o.nValue && indices == o.indices;
    }
};

// Runs the three confirmation tiers exactly as CWallet::SelectCoins does, once with the memo
// disabled (reference) and once enabled (optimized), both from the same RNG seed.
// Deterministic replacement for GetRandBytes so both paths draw the same stream.
uint32_t g_det_state = 0x12345678u;
void DeterministicRandBytes(unsigned char* buf, int num)
{
    for (int i = 0; i < num; ++i) {
        g_det_state = g_det_state * 1664525u + 1013904223u;
        buf[i] = (unsigned char)(g_det_state >> 24);
    }
}
void ResetDeterministicRand() { g_det_state = 0x12345678u; }

Result RunSelection(CWallet& wallet, int64_t nTarget, unsigned int nSpendTime,
                    const std::vector<COutput>& vCoins, bool fUseCache,
                    const std::map<const CWalletTx*, int>& indexOf)
{
    Result r;
    r.fOk = false;
    r.nValue = 0;

    pfnSelectCoinsRandBytes = DeterministicRandBytes;
    std::map<const CWalletTx*, bool>* pCache = NULL;
    std::map<const CWalletTx*, bool> cache;
    if (fUseCache)
        pCache = &cache;

    static const int tiers[3][2] = {{1, 10}, {1, 1}, {0, 1}};
    std::set<std::pair<const CWalletTx*, unsigned int> > setCoins;
    for (int t = 0; t < 3; ++t)
    {
        setCoins.clear();
        r.nValue = 0;
        r.fOk = wallet.SelectCoinsMinConf(nTarget, nSpendTime, tiers[t][0], tiers[t][1],
                                          vCoins, setCoins, r.nValue, pCache);
        if (r.fOk)
            break;
    }
    if (r.fOk)
        for (std::set<std::pair<const CWalletTx*, unsigned int> >::const_iterator it = setCoins.begin();
             it != setCoins.end(); ++it)
            r.indices.insert(indexOf.find(it->first)->second);
    return r;
}

void Compare(const char* name, int64_t nTarget, unsigned int nSpendTime,
             const std::vector<CoinSpec>& specs)
{
    // Two independent wallets with identical coin specs: running both paths on ONE fixture lets
    // the first run warm wallet-side caches and would compare state, not algorithms.
    HookGuard guard;
    (void)guard;
    Result ref, opt;
    {
        WalletFixture fx;
        std::vector<COutput> vCoins = fx.BuildPool(specs);
        ResetDeterministicRand();
        ref = RunSelection(fx.wallet, nTarget, nSpendTime, vCoins, false, fx.indexOf);
    }
    {
        WalletFixture fx;
        std::vector<COutput> vCoins = fx.BuildPool(specs);
        ResetDeterministicRand();
        opt = RunSelection(fx.wallet, nTarget, nSpendTime, vCoins, true, fx.indexOf);
    }
    BOOST_TEST_MESSAGE(name << ": ok=" << ref.fOk << " value=" << ref.nValue
                            << " coins=" << ref.indices.size());
    BOOST_REQUIRE_MESSAGE(ref == opt, "selection differs between reference and memoized path: " << name);
}
} // namespace

BOOST_AUTO_TEST_CASE(same_selection_for_mixed_wallet)
{
    std::vector<CoinSpec> specs;
    // 60 ours (high conf), 60 foreign (1 conf), a few immature, one unspendable, one future-dated.
    for (int i = 0; i < 60; ++i)
    {
        CoinSpec s = {CENT_7, 100, -5000, true, true};
        specs.push_back(s);
    }
    for (int i = 0; i < 60; ++i)
    {
        CoinSpec s = {CENT_7, 1, -5000, false, true};
        specs.push_back(s);
    }
    for (int i = 0; i < 5; ++i)
    {
        CoinSpec s = {CENT_7, 0, -5000, false, true};   // immature: filtered by conf tier
        specs.push_back(s);
    }
    CoinSpec unspendable = {1 * COIN, 100, -5000, true, false};
    specs.push_back(unspendable);
    CoinSpec future = {5 * COIN, 100, +500, true, true};      // nTime > nSpendTime: filtered
    specs.push_back(future);

    Compare("mixed_wallet_target_4in", 4 * COIN, 1500000500, specs);
}

BOOST_AUTO_TEST_CASE(same_selection_exact_match)
{
    std::vector<CoinSpec> specs;
    for (int i = 0; i < 30; ++i)
    {
        CoinSpec s = {CENT_7, 100, -5000, true, true};
        specs.push_back(s);
    }
    CoinSpec exact = {4 * COIN, 100, -5000, true, true};
    specs.push_back(exact);
    Compare("exact_match", 4 * COIN, 1500000500, specs);
}

BOOST_AUTO_TEST_CASE(same_selection_single_larger_coin)
{
    std::vector<CoinSpec> specs;
    for (int i = 0; i < 10; ++i)
    {
        CoinSpec s = {CENT_7, 100, -5000, true, true};
        specs.push_back(s);
    }
    CoinSpec large = {100 * COIN, 100, -5000, true, true};
    specs.push_back(large);
    Compare("single_larger_coin", 40 * COIN, 1500000500, specs);
}

BOOST_AUTO_TEST_CASE(same_failure_when_insufficient)
{
    HookGuard guard; (void)guard;
    std::vector<CoinSpec> specs;
    for (int i = 0; i < 20; ++i)
    {
        CoinSpec s = {CENT_7, 100, -5000, true, true};
        specs.push_back(s);
    }
    Result ref, opt;
    {
        WalletFixture fx;
        std::vector<COutput> vCoins = fx.BuildPool(specs);
        ResetDeterministicRand();
        ref = RunSelection(fx.wallet, 1000 * COIN, 1500000500, vCoins, false, fx.indexOf);
    }
    {
        WalletFixture fx;
        std::vector<COutput> vCoins = fx.BuildPool(specs);
        ResetDeterministicRand();
        opt = RunSelection(fx.wallet, 1000 * COIN, 1500000500, vCoins, true, fx.indexOf);
    }
    BOOST_REQUIRE(!ref.fOk);
    BOOST_REQUIRE(ref == opt);
    BOOST_REQUIRE(ref.indices.empty());
}

BOOST_AUTO_TEST_CASE(same_selection_denominated_amounts)
{
    HookGuard guard; (void)guard;
    // tryDenom == 0 rejects denominated values; both paths must pick the same side of the branch.
    WalletFixture fx;
    std::vector<CoinSpec> specs;
    for (int i = 0; i < 40; ++i)
    {
        CoinSpec s = {CENT_10, 100, -5000, true, true};
        specs.push_back(s);
    }
    for (int i = 0; i < 40; ++i)
    {
        CoinSpec s = {CENT_7, 100, -5000, true, true};
        specs.push_back(s);
    }
    Compare("denominated_amounts", 3 * COIN, 1500000500, specs);
}

BOOST_AUTO_TEST_CASE(same_seed_reproduces_same_selection)
{
    // The sampler spends one seed draw and expands it with a cheap PRNG instead of calling
    // RAND_bytes() for every bit; a fixed seed must therefore reproduce the same selection.
    HookGuard guard; (void)guard;
    std::vector<CoinSpec> specs;
    for (int i = 0; i < 200; ++i)
    {
        CoinSpec s = {CENT_7, 100, -5000, true, true};
        specs.push_back(s);
    }
    for (int i = 0; i < 200; ++i)
    {
        CoinSpec s = {CENT_7, 1, -5000, false, true};
        specs.push_back(s);
    }
    Result a, b;
    {
        WalletFixture fx;
        std::vector<COutput> vCoins = fx.BuildPool(specs);
        ResetDeterministicRand();
        a = RunSelection(fx.wallet, 5 * COIN, 1500000500, vCoins, true, fx.indexOf);
    }
    {
        WalletFixture fx;
        std::vector<COutput> vCoins = fx.BuildPool(specs);
        ResetDeterministicRand();
        b = RunSelection(fx.wallet, 5 * COIN, 1500000500, vCoins, true, fx.indexOf);
    }
    BOOST_REQUIRE(a.fOk);
    BOOST_REQUIRE_MESSAGE(a == b, "same seed produced different selections");
    BOOST_REQUIRE_MESSAGE(a.nValue >= 5 * COIN, "selection does not cover the target");
    BOOST_REQUIRE_MESSAGE(!a.indices.empty(), "empty selection for a covered target");
}

BOOST_AUTO_TEST_SUITE_END()
