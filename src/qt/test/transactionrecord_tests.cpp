#include "transactionrecord_tests.h"
#include "../transactionrecord.h"
#include "../wallet.h"
#include "../main.h"

#include <QList>

void TransactionRecordTests::showTransactionAcceptsCoinbase()
{
    // Build a minimal coinbase CWalletTx. IsCoinBase() requires
    // vin.size()==1 and vin[0].prevout.IsNull().
    CTransaction tx;
    tx.vin.push_back(CTxIn(COutPoint(0, (unsigned int)-1), CScript(), 0));
    tx.vout.push_back(CTxOut(1000, CScript()));
    tx.nVersion = 1;

    // CWalletTx wrapping the coinbase transaction, no wallet bound.
    CWalletTx wtx(NULL, tx);

    // At this point:
    //   - wtx.IsCoinBase()   == true
    //   - wtx.IsInMainChain() == false (hashBlock==0, nIndex==-1)
    //
    // The original IsInMainChain gate would have returned false here,
    // causing the model to skip insertion. After the fix, showTransaction
    // ignores the coinbase/IsInMainChain check and returns true.
    QVERIFY(TransactionRecord::showTransaction(wtx) == true);
}

void TransactionRecordTests::showTransactionAcceptsNonCoinbase()
{
    // A regular (non-coinbase) transaction — two inputs, one output.
    CTransaction tx;
    tx.vin.push_back(CTxIn(COutPoint(uint256("aa"), 0), CScript(), 0));
    tx.vin.push_back(CTxIn(COutPoint(uint256("bb"), 1), CScript(), 0));
    tx.vout.push_back(CTxOut(2000, CScript()));
    tx.nVersion = 1;

    CWalletTx wtx(NULL, tx);

    // showTransaction has always returned true for non-coinbase transactions.
    QVERIFY(TransactionRecord::showTransaction(wtx) == true);
}
