// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TXDB_BDB_H
#define BITCOIN_TXDB_BDB_H


#ifndef INNOVA_SYNC_CHECKPOINT_LOAD_STATUS_DEFINED
#define INNOVA_SYNC_CHECKPOINT_LOAD_STATUS_DEFINED
// V2-R2B (FIX A): typed result of reading the persisted synchronized checkpoint
// (see txdb-leveldb.h for the semantics; identical in both impls).
enum SyncCheckpointLoadStatus
{
    SYNC_CHECKPOINT_ABSENT  = 0,
    SYNC_CHECKPOINT_LOADED  = 1,
    SYNC_CHECKPOINT_CORRUPT = 2,
};
#endif

/** Access to the transaction database (blkindex.dat) */
class CTxDB : public CDB
{
public:
    CTxDB(const char* pszMode="r+") : CDB("blkindex.dat", pszMode) { }
private:
    CTxDB(const CTxDB&);
    void operator=(const CTxDB&);
public:

    bool ReadTxIndex(uint256 hash, CTxIndex& txindex);
    bool UpdateTxIndex(uint256 hash, const CTxIndex& txindex);
    bool AddTxIndex(const CTransaction& tx, const CDiskTxPos& pos, int nHeight);
    bool EraseTxIndex(const CTransaction& tx);
    bool ContainsTx(uint256 hash);
    bool ReadDiskTx(uint256 hash, CTransaction& tx, CTxIndex& txindex);
    bool ReadDiskTx(uint256 hash, CTransaction& tx);
    bool ReadDiskTx(COutPoint outpoint, CTransaction& tx, CTxIndex& txindex);
    bool ReadDiskTx(COutPoint outpoint, CTransaction& tx);
    bool WriteBlockIndex(const CDiskBlockIndex& blockindex);
    bool ReadHashBestChain(uint256& hashBestChain);
    bool WriteHashBestChain(uint256 hashBestChain);
    bool ReadBestInvalidTrust(CBigNum& bnBestInvalidTrust);
    bool WriteBestInvalidTrust(CBigNum bnBestInvalidTrust);
    bool ReadInvalidBlockSet(std::set<uint256>& setInvalidBlockHash);
    bool WriteInvalidBlockSet(const std::set<uint256>& setInvalidBlockHash);
    bool ReadSyncCheckpoint(uint256& hashCheckpoint);
    bool WriteSyncCheckpoint(uint256 hashCheckpoint);
    // V2-R2B (FIX A): see txdb-leveldb.h — typed result distinguishing an ABSENT
    // persisted synchronized checkpoint (ppcoin "none") from a CORRUPT one.
    SyncCheckpointLoadStatus LoadSyncCheckpoint(uint256& hashCheckpoint);
    bool ReadCheckpointPubKey(std::string& strPubKey);
    bool WriteCheckpointPubKey(const std::string& strPubKey);
    bool ReadModifierUpgradeTime(unsigned int& nUpgradeTime);
    bool WriteModifierUpgradeTime(const unsigned int& nUpgradeTime);
    bool LoadBlockIndex();
private:
    bool LoadBlockIndexGuts();
};

#endif
