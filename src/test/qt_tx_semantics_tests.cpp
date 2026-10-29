// Regression tests for the transaction-lookup / mempool API contract used by the
// Qt GUI after the CTxDB removal.
//
// Context: the GUI (src/qt/multisigdialog.cpp, src/qt/transactiondesc.cpp) used to
// read previous transactions through CTxDB::ReadDiskTx/FetchInputs. That index no
// longer exists; the replacement is GetTransaction(hash, tx, hashBlock) for
// read-only lookups and AcceptToMemoryPool() for submission. These tests pin the
// two properties the GUI relies on:
//
//   1. GetTransaction() is READ-ONLY: an unknown hash returns false and changes
//      neither the mempool nor the best height (a transaction-description view
//      must never alter node state).
//   2. AcceptToMemoryPool(..., fOnlyCheckWithoutAdding=true) never inserts the
//      transaction (the "check" path cannot leak a transaction into the mempool).
//
// They do not exercise the Qt widgets themselves (no GUI harness in this tree);
// they pin the underlying contract the widgets call.

#include <boost/test/unit_test.hpp>

#include "main.h"
#include <limits>

BOOST_AUTO_TEST_SUITE(qt_tx_semantics_tests)

// 1. Read-only lookup of an unknown transaction.
BOOST_AUTO_TEST_CASE(gettransaction_unknown_hash_is_read_only)
{
    const size_t mempoolBefore = mempool.size();
    const int heightBefore = pindexBest ? pindexBest->nHeight : -1;

    uint256 unknown = uint256(0xDEADBEEF);
    CTransaction tx;
    uint256 hashBlock = 0;

    const bool found = GetTransaction(unknown, tx, hashBlock);

    BOOST_CHECK_MESSAGE(!found, "an unknown txid must not resolve");
    BOOST_CHECK_MESSAGE(mempool.size() == mempoolBefore,
                        "GetTransaction() must not add or remove mempool entries");
    BOOST_CHECK_MESSAGE((pindexBest ? pindexBest->nHeight : -1) == heightBefore,
                        "GetTransaction() must not change the best height");
    BOOST_CHECK_MESSAGE(hashBlock == 0,
                        "an unresolved transaction must not report a containing block");
}

// 2. The check-only mempool path must not insert anything.
BOOST_AUTO_TEST_CASE(check_only_accept_does_not_add_to_mempool)
{
    CTransaction tx;
    tx.nVersion = 1;
    tx.nTime = (unsigned int)GetTime();
    tx.nLockTime = 0;
    tx.vin.resize(1);
    tx.vout.resize(1);
    // Deliberately unknown prevout: the transaction cannot be accepted, and with
    // fOnlyCheckWithoutAdding it must not be added even if it were acceptable.
    tx.vin[0].prevout = COutPoint(uint256(0xFEEDFACE), 0);
    tx.vin[0].nSequence = std::numeric_limits<unsigned int>::max();
    tx.vout[0].nValue = 0;
    tx.vout[0].scriptPubKey.clear();

    const size_t mempoolBefore = mempool.size();

    bool fMissingInputs = false;
    const bool accepted = tx.AcceptToMemoryPool(/*fCheckInputs=*/true, &fMissingInputs,
                                               /*fOnlyCheckWithoutAdding=*/true);

    BOOST_CHECK_MESSAGE(!accepted, "a transaction with an unknown prevout cannot be accepted");
    BOOST_CHECK_MESSAGE(mempool.size() == mempoolBefore,
                        "fOnlyCheckWithoutAdding must not insert into the mempool");
    BOOST_CHECK_MESSAGE(!mempool.exists(tx.GetHash()),
                        "a check-only submission must not be visible in the mempool");
}

BOOST_AUTO_TEST_SUITE_END()
