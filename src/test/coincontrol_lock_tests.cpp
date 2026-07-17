#include <boost/test/unit_test.hpp>
#include <set>

#include "coincontrol.h"
#include "main.h"
#include "wallet.h"

BOOST_AUTO_TEST_SUITE(coincontrol_lock_tests)

namespace
{
class ScopedMerkleRoot
{
public:
    ScopedMerkleRoot(CBlockIndex* indexIn, const uint256& root)
        : index(indexIn), originalRoot(indexIn->hashMerkleRoot)
    {
        LOCK(cs_main);
        index->hashMerkleRoot = root;
    }

    ~ScopedMerkleRoot()
    {
        LOCK(cs_main);
        index->hashMerkleRoot = originalRoot;
    }

private:
    CBlockIndex* index;
    uint256 originalRoot;
};

std::set<COutPoint> GetOutpoints(const std::vector<COutput>& coins)
{
    std::set<COutPoint> outpoints;
    for (std::vector<COutput>::const_iterator it = coins.begin(); it != coins.end(); ++it)
        outpoints.insert(COutPoint(it->tx->GetHash(), it->i));
    return outpoints;
}
}

BOOST_AUTO_TEST_CASE(create_transaction_failure_rolls_back_reserved_coins)
{
    BOOST_REQUIRE(pindexBest != NULL);

    CWallet testWallet;
    CKey key;
    key.MakeNewKey(true);
    {
        LOCK(testWallet.cs_wallet);
        BOOST_REQUIRE(testWallet.AddKey(key));
    }

    CScript scriptPubKey;
    scriptPubKey.SetDestination(key.GetPubKey().GetID());

    static const unsigned int INPUT_COUNT = 711;
    CBlock block;
    for (unsigned int i = 0; i <= INPUT_COUNT; ++i)
    {
        CTransaction tx;
        tx.nTime = pindexBest->GetBlockTime();
        tx.vin.push_back(CTxIn(COutPoint(uint256(i + 1), 0)));
        tx.vout.push_back(CTxOut(2 * COIN + i, scriptPubKey));
        block.vtx.push_back(tx);
    }
    block.hashMerkleRoot = block.BuildMerkleTree();
    ScopedMerkleRoot merkleRoot(pindexBest, block.hashMerkleRoot);

    CCoinControl oversizedCoinControl;
    std::vector<COutPoint> selectedOutpoints;
    for (unsigned int i = 0; i <= INPUT_COUNT; ++i)
    {
        CWalletTx wtx(&testWallet, block.vtx[i]);
        wtx.hashBlock = pindexBest->GetBlockHash();
        wtx.nIndex = i;
        wtx.vMerkleBranch = block.GetMerkleBranch(i);

        const uint256 hash = wtx.GetHash();
        testWallet.mapWallet[hash] = wtx;
        testWallet.mapWallet[hash].BindWallet(&testWallet);

        COutPoint outpoint(hash, 0);
        selectedOutpoints.push_back(outpoint);
        if (i < INPUT_COUNT)
            oversizedCoinControl.Select(outpoint);
    }

    COutPoint preexistingLock = selectedOutpoints[INPUT_COUNT];
    {
        LOCK(testWallet.cs_wallet);
        testWallet.LockCoin(preexistingLock);
    }

    std::vector<COutput> availableBefore;
    testWallet.AvailableCoins(availableBefore, true, &oversizedCoinControl);
    BOOST_REQUIRE_EQUAL(INPUT_COUNT, availableBefore.size());

    std::vector<std::pair<CScript, int64_t> > recipients;
    recipients.push_back(std::make_pair(scriptPubKey, COIN));
    CWalletTx oversizedTransaction(&testWallet);
    CReserveKey oversizedReserveKey(&testWallet);
    int64_t feeRequired = 0;
    int32_t changePos = -1;
    std::string failReason;

    BOOST_CHECK(!testWallet.CreateTransaction(recipients, oversizedTransaction,
                                               oversizedReserveKey, feeRequired,
                                               changePos, &oversizedCoinControl,
                                               &scriptPubKey, &failReason));
    BOOST_CHECK_EQUAL(failReason, _("Transaction too large"));
    BOOST_CHECK(oversizedTransaction.vReservedCoins.empty());

    std::vector<COutput> availableAfter;
    testWallet.AvailableCoins(availableAfter, true, &oversizedCoinControl);
    BOOST_CHECK(GetOutpoints(availableBefore) == GetOutpoints(availableAfter));
    {
        LOCK(testWallet.cs_wallet);
        BOOST_CHECK(testWallet.IsLockedCoin(preexistingLock.hash, preexistingLock.n));
        BOOST_CHECK_EQUAL(1U, testWallet.setLockedCoins.size());
    }

    CCoinControl smallCoinControl;
    smallCoinControl.Select(selectedOutpoints[0]);
    smallCoinControl.Select(selectedOutpoints[1]);

    CWalletTx smallTransaction(&testWallet);
    CReserveKey smallReserveKey(&testWallet);
    feeRequired = 0;
    changePos = -1;
    failReason.clear();

    BOOST_REQUIRE(testWallet.CreateTransaction(recipients, smallTransaction,
                                               smallReserveKey, feeRequired,
                                               changePos, &smallCoinControl,
                                               &scriptPubKey, &failReason));
    BOOST_CHECK_EQUAL(2U, smallTransaction.vReservedCoins.size());
    {
        LOCK(testWallet.cs_wallet);
        BOOST_CHECK_EQUAL(2U, testWallet.UnlockReservedCoins(smallTransaction));
        BOOST_CHECK(testWallet.IsLockedCoin(preexistingLock.hash, preexistingLock.n));
        BOOST_CHECK_EQUAL(1U, testWallet.setLockedCoins.size());
        testWallet.UnlockCoin(preexistingLock);
    }
}

BOOST_AUTO_TEST_SUITE_END()
