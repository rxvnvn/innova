#ifndef TRANSACTIONRECORD_TESTS_H
#define TRANSACTIONRECORD_TESTS_H

#include <QTest>
#include <QObject>

class TransactionRecordTests : public QObject
{
    Q_OBJECT

private slots:
    void showTransactionAcceptsCoinbase();
    void showTransactionAcceptsNonCoinbase();
};

#endif // TRANSACTIONRECORD_TESTS_H
