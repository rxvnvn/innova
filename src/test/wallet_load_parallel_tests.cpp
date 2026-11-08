// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license.

// WALLET-LOAD PARALLELISM (perf/startup-phase-timing) — equivalence and cost.
//
// A legacy wallet (records written WITHOUT the pubkey/privkey accelerator hash,
// which is what production wallets of this age contain) costs one EC
// re-derivation+verification per key at load. PrepareKeyRecord does that work
// statelessly; the parallel path spreads it over a bounded batch and applies
// records strictly in cursor order.
//
// This test builds such a wallet through the REAL WriteKey path (which writes
// the legacy, hash-less record), loads it twice — -walletloadthreads=1 vs 8 —
// and asserts (a) the loaded key set is IDENTICAL, (b) metadata/birthday/keypool
// state is identical, (c) both loads are read-only w.r.t. the wallet file, and
// (d) the parallel load is not slower.

#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include "../walletdb.h"
#include "../wallet.h"
#include "../key.h"
#include "../util.h"

#include <boost/filesystem.hpp>

#include <set>
#include <string>
#include <vector>

namespace {

// Digest of the wallet file's bytes: proves a load does not rewrite it.
static std::string FileDigest(const std::string& strFile)
{
    FILE* f = fopen(strFile.c_str(), "rb");
    BOOST_REQUIRE(f != NULL);
    std::vector<unsigned char> vch;
    unsigned char buf[65536];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        vch.insert(vch.end(), buf, buf + n);
    fclose(f);
    return Hash(vch.begin(), vch.end()).GetHex();
}

static void MakeLegacyWalletWithKeys(const std::string& strFile, int nKeys)
{
    CWalletDB db(strFile);
    for (int i = 0; i < nKeys; ++i)
    {
        CKey key;
        key.MakeNewKey(true);
        CKeyMetadata meta;
        meta.nVersion = 1;
        meta.nCreateTime = 1300000000 + i;
        // WriteKey writes the LEGACY record layout ("key" => privkey only):
        // ReadKeyValue then finds no accelerator hash and re-derives the pubkey
        // with EC — exactly the production wallet shape under measurement.
        BOOST_REQUIRE(db.WriteKey(key.GetPubKey(), key.GetPrivKey(), meta));
    }
    db.Flush();
}

struct LoadResult
{
    std::set<CKeyID> keys;
    size_t nMetadata;
    size_t nKeyPool;
    int64_t nTimeFirstKey;
    int64_t nElapsedMs;
};

static LoadResult LoadOnce(const std::string& strFile, int nThreads)
{
    mapArgs["-walletloadthreads"] = strprintf("%d", nThreads);
    CWallet wallet(strFile);
    const int64_t nStart = GetTimeMillis();
    const DBErrors err = wallet.LoadWallet(/*fFirstRun=*/false);
    LoadResult r;
    r.nElapsedMs = GetTimeMillis() - nStart;
    wallet.GetKeys(r.keys);
    r.nMetadata = wallet.mapKeyMetadata.size();
    r.nKeyPool = wallet.setKeyPool.size();
    r.nTimeFirstKey = wallet.nTimeFirstKey;
    BOOST_REQUIRE_EQUAL((int)err, (int)DB_LOAD_OK);
    return r;
}

} // namespace

BOOST_AUTO_TEST_SUITE(wallet_load_parallel_tests)

BOOST_AUTO_TEST_CASE(legacy_wallet_parallel_prep_equals_sequential_and_is_not_slower)
{
    const std::string strFile = "wallet-load-parallel-test.dat";
    boost::filesystem::remove(strFile); // clean file each run (BDB may keep it open)

    const int nKeys = 4000;
    MakeLegacyWalletWithKeys(strFile, nKeys);

    // Fingerprint the file: loading a healthy wallet must never rewrite it.
    const boost::uintmax_t nSizeBefore = boost::filesystem::file_size(strFile);
    const std::string strHashBefore = FileDigest(strFile);

    const LoadResult seq = LoadOnce(strFile, 1);
    const LoadResult par = LoadOnce(strFile, 8);

    // (a) IDENTICAL loaded key set (not merely the same count).
    BOOST_CHECK_EQUAL(seq.keys.size(), par.keys.size());
    BOOST_CHECK_EQUAL(seq.keys.size(), (size_t)nKeys);
    BOOST_CHECK(seq.keys == par.keys);

    // (b) identical remaining load state.
    BOOST_CHECK_EQUAL(seq.nMetadata, par.nMetadata);
    BOOST_CHECK_EQUAL(seq.nKeyPool, par.nKeyPool);
    BOOST_CHECK_EQUAL(seq.nTimeFirstKey, par.nTimeFirstKey);

    // (c) read-only w.r.t. the wallet file.
    BOOST_CHECK_EQUAL(boost::filesystem::file_size(strFile), nSizeBefore);
    BOOST_CHECK_EQUAL(FileDigest(strFile), strHashBefore);

    // (d) cost: informational, plus a guard against a pathological regression.
    printf("WALLET_LOAD_TIMING keys=%d sequential=%lldms parallel8=%lldms speedup=%.2fx\n",
           nKeys, (long long)seq.nElapsedMs, (long long)par.nElapsedMs,
           (seq.nElapsedMs > 0 && par.nElapsedMs > 0)
               ? (double)seq.nElapsedMs / (double)par.nElapsedMs : 0.0);
    BOOST_CHECK_MESSAGE(par.nElapsedMs <= seq.nElapsedMs,
                        "parallel wallet load must not be slower than sequential");
}

BOOST_AUTO_TEST_SUITE_END()
