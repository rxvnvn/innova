// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2013 The NovaCoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "txdb.h"
#include "miner.h"
#include "kernel.h"
#include "collateralnode.h"
#include "dag.h"
#include "finality.h"

#include <chrono>
#include <limits>
#include <memory>

using namespace std;

//////////////////////////////////////////////////////////////////////////////
//
// BitcoinMiner
//

extern unsigned int nMinerSleep;

static bool TransactionSpendsAnyOutpoint(const CTransaction& tx,
                                         const std::set<COutPoint>& setOutpoints)
{
    if (setOutpoints.empty())
        return false;

    BOOST_FOREACH(const CTxIn& txin, tx.vin)
    {
        if (setOutpoints.count(txin.prevout))
            return true;
    }
    return false;
}

int static FormatHashBlocks(void* pbuffer, unsigned int len)
{
    unsigned char* pdata = (unsigned char*)pbuffer;
    unsigned int blocks = 1 + ((len + 8) / 64);
    unsigned char* pend = pdata + 64 * blocks;
    memset(pdata + len, 0, 64 * blocks - len);
    pdata[len] = 0x80;
    unsigned int bits = len * 8;
    pend[-1] = (bits >> 0) & 0xff;
    pend[-2] = (bits >> 8) & 0xff;
    pend[-3] = (bits >> 16) & 0xff;
    pend[-4] = (bits >> 24) & 0xff;
    return blocks;
}

static const unsigned int pSHA256InitState[8] =
{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

void SHA256Transform(void* pstate, void* pinput, const void* pinit)
{
    SHA256_CTX ctx;
    unsigned char data[64];

    SHA256_Init(&ctx);

    for (int i = 0; i < 16; i++)
        ((uint32_t*)data)[i] = ByteReverse(((uint32_t*)pinput)[i]);

    for (int i = 0; i < 8; i++)
        ctx.h[i] = ((uint32_t*)pinit)[i];

    SHA256_Update(&ctx, data, sizeof(data));
    for (int i = 0; i < 8; i++)
        ((uint32_t*)pstate)[i] = ctx.h[i];
}

// Some explaining would be appreciated
class COrphan
{
public:
    CTransaction* ptx;
    set<uint256> setDependsOn;
    double dPriority;
    double dFeePerKb;
    int64_t nFee;

    COrphan(CTransaction* ptxIn)
    {
        ptx = ptxIn;
        dPriority = dFeePerKb = 0;
        nFee = 0;
    }

    COrphan(double dPriority_, double dFeePerKb_, int64_t nFee_, CTransaction* ptxIn)
    {
        dPriority = dPriority_;
        dFeePerKb = dFeePerKb_;
        nFee = nFee_;
        ptx = ptxIn;
     }

    void print() const
    {
        printf("COrphan(hash=%s, dPriority=%.1f, dFeePerKb=%.1f)\n",
               ptx->GetHash().ToString().substr(0,10).c_str(), dPriority, dFeePerKb);
        BOOST_FOREACH(uint256 hash, setDependsOn)
            printf("   setDependsOn %s\n", hash.ToString().substr(0,10).c_str());
    }
};


uint64_t nLastBlockTx = 0;
uint64_t nLastBlockSize = 0;
int64_t nLastCoinStakeSearchInterval = 0;

// We want to sort transactions by priority and fee, so:
typedef boost::tuple<double, double, int64_t, CTransaction*> TxPriority;
class TxPriorityCompare
{
    bool byFee;
public:
    TxPriorityCompare(bool _byFee) : byFee(_byFee) { }
    bool operator()(const TxPriority& a, const TxPriority& b)
    {
        if (byFee)
        {
            if (a.get<1>() == b.get<1>())
                return a.get<0>() < b.get<0>();
            return a.get<1>() < b.get<1>();
        }
        else
        {
            if (a.get<0>() == b.get<0>())
                return a.get<1>() < b.get<1>();
            return a.get<0>() < b.get<0>();
        }
    }
};

// CreateNewBlock: create new block (without proof-of-work/proof-of-stake)
CBlock* CreateNewBlock(CWallet* pwallet, bool fProofOfStake, int64_t* pFees,
                       CReserveKey* pReserveKey)
{
    // Create new block
    auto_ptr<CBlock> pblock(new CBlock());
    if (!pblock.get())
        return NULL;

    CBlockIndex* pindexPrev;
    {
        LOCK2(cs_main, g_dagManager.cs_dag);
        pindexPrev = g_dagManager.SelectBestDAGTip();
        if (!pindexPrev)
            pindexPrev = pindexBest;
    }
    if (!pindexPrev)
    {
        printf("CreateNewBlock: ERROR: pindexPrev is NULL\n");
        return NULL;
    }

    int payments = 1;
    // Create coinbase tx
    CTransaction txNew;
    txNew.vin.resize(1);
    txNew.vin[0].prevout.SetNull();
    txNew.vout.resize(1);


    int nHeight = pindexPrev->nHeight+1; // height of new block
    if (fProofOfStake && nHeight >= FORK_HEIGHT_DAG)
    {
        if (fDebug && GetBoolArg("-printcoinstake"))
            printf("CreateNewBlock: refusing proof-of-stake block template at post-DAG height %d\n", nHeight);
        return NULL;
    }

    if (!fProofOfStake)
    {
        CReserveKey localReserveKey(pwallet);
        CReserveKey* reservekey = pReserveKey ? pReserveKey : &localReserveKey;
        CPubKey pubkey;
        if (!reservekey->GetReservedKey(pubkey))
            return NULL;
        txNew.vout[0].scriptPubKey.SetDestination(pubkey.GetID());
    }
    else
    {
        // Height first in coinbase required for block.version=2
        txNew.vin[0].scriptSig = (CScript() << nHeight) + COINBASE_FLAGS;
        if (txNew.vin[0].scriptSig.size() > 100)
        {
            printf("CreateNewBlock() : coinbase scriptSig too large (%d bytes)\n", (int)txNew.vin[0].scriptSig.size());
            return NULL;
        }

        txNew.vout[0].SetEmpty();
    }

    // IDAG Phase 2: Add DAG parent commitment to coinbase
    if (nHeight >= FORK_HEIGHT_DAG)
    {
        std::vector<uint256> vDAGParents;

        // Primary parent = pindexPrev
        if (pindexPrev->phashBlock)
            vDAGParents.push_back(pindexPrev->GetBlockHash());

        // Collect merge parents from DAG tips (cs_main for mapBlockIndex access)
        {
            LOCK2(cs_main, g_dagManager.cs_dag);
            std::vector<uint256> vTips = g_dagManager.GetDAGTips();

            std::vector<std::pair<uint256, uint256>> vTipScores;
            for (const uint256& hashTip : vTips)
            {
                if (pindexPrev->phashBlock && hashTip == pindexPrev->GetBlockHash())
                    continue; // skip primary parent
                std::map<uint256, CBlockIndex*>::iterator miTip = mapBlockIndex.find(hashTip);
                if (miTip == mapBlockIndex.end() || miTip->second == NULL)
                    continue;
                CBlockIndex* pTip = miTip->second;
                if (pTip != pindexBest && pTip->nChainTrust > nBestChainTrust)
                    continue;
                uint256 nScore = g_dagManager.ComputeDAGScore(pTip);
                vTipScores.push_back(std::make_pair(nScore, hashTip));
            }
            std::sort(vTipScores.begin(), vTipScores.end(),
                      [](const std::pair<uint256, uint256>& a, const std::pair<uint256, uint256>& b) {
                          if (a.first != b.first)
                              return a.first > b.first; // higher score first
                          return a.second < b.second;   // deterministic tiebreak
                      });

            for (const auto& pair : vTipScores)
            {
                if (vDAGParents.size() >= (unsigned int)MAX_DAG_PARENTS)
                    break;

                const uint256& hashTip = pair.second;

                // Merge parent must exist and be within DAG_MERGE_DEPTH
                std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashTip);
                if (mi == mapBlockIndex.end() || mi->second == NULL)
                    continue;
                CBlockIndex* pTip = mi->second;
                if (pTip != pindexBest && pTip->nChainTrust > nBestChainTrust)
                    continue;
                if (pTip->nHeight < pindexPrev->nHeight - DAG_MERGE_DEPTH)
                    continue;
                if (pTip->nHeight >= nHeight)
                    continue;

                vDAGParents.push_back(hashTip);
            }
        }

        if (!vDAGParents.empty())
        {
            CScript dagScript = BuildDAGParentScript(vDAGParents);
            if (dagScript.size() > 0)
            {
                CTxOut dagOut;
                dagOut.nValue = 0;
                dagOut.scriptPubKey = dagScript;
                txNew.vout.push_back(dagOut);
            }
        }
    }

    // Add our coinbase tx as first transaction
    pblock->vtx.push_back(txNew);

    // Largest block you're willing to create (adaptive post-DAG):
    unsigned int nAdaptiveLimit = GetAdaptiveBlockSizeLimit(pindexPrev);
    unsigned int nBlockMaxSize = GetArg("-blockmaxsize", nAdaptiveLimit / 2);
    // Limit to between 1K and the adaptive ceiling (underflow-safe)
    unsigned int nMaxAllowed = (nAdaptiveLimit > 1000) ? (nAdaptiveLimit - 1000) : 1000;
    nBlockMaxSize = std::max((unsigned int)1000, std::min(nMaxAllowed, nBlockMaxSize));

    // How much of the block should be dedicated to high-priority transactions,
    // included regardless of the fees they pay
    unsigned int nBlockPrioritySize = GetArg("-blockprioritysize", 27000);
    nBlockPrioritySize = std::min(nBlockMaxSize, nBlockPrioritySize);

    // Minimum block size you want to create; block will be filled with free transactions
    // until there are no more or the block reaches this size:
    unsigned int nBlockMinSize = GetArg("-blockminsize", 0);
    nBlockMinSize = std::min(nBlockMaxSize, nBlockMinSize);

    // start collateralnode payments
    bool bCollateralNodePayment = false;

	//Only if it isn't Proof of Stake?
	if (!fProofOfStake)
    {
		if (fTestNet) {
			if (nHeight >= BLOCK_START_COLLATERALNODE_PAYMENTS_TESTNET){
				bCollateralNodePayment = true;
			}
		} else {
			if (nHeight >= BLOCK_START_COLLATERALNODE_PAYMENTS && nHeight >= 2085000){
				bCollateralNodePayment = true;
			}
		}
        if(fDebug && fDebugCN) { printf("CreateNewBlock(): Collateralnode Payments : %i\n", bCollateralNodePayment); }
	}

    // Fee-per-kilobyte amount considered the same as "free"
    // Be careful setting this: if you set it to zero then
    // a transaction spammer can cheaply fill blocks using
    // 1-innovai-fee transactions. It should be set above the real
    // cost to you of processing a transaction.
    int64_t nMinTxFee = MIN_TX_FEE;
    if (mapArgs.count("-mintxfee"))
        ParseMoney(mapArgs["-mintxfee"], nMinTxFee);

    pblock->nBits = GetNextTargetRequired(pindexPrev, fProofOfStake);

    std::vector<CFinalityVote> vFinalityVotesForBlock;
    std::set<COutPoint> setFinalityStakeProofOutpoints;
    if (!fProofOfStake && nHeight >= FORK_HEIGHT_DAG)
    {
        vFinalityVotesForBlock = g_finalityTracker.GetPendingVotesForBlock(nHeight);
        BOOST_FOREACH(const CFinalityVote& vote, vFinalityVotesForBlock)
        {
            if (vote.IsPrivate())
                continue;
            BOOST_FOREACH(const COutPoint& proof, vote.vStakeProof)
                setFinalityStakeProofOutpoints.insert(proof);
        }
    }

    // Collect memory pool transactions into the block
    int64_t nFees = 0;
    {
        LOCK2(cs_main, mempool.cs);
        CTxDB txdb("r");

        if(bCollateralNodePayment) {
            bool hasPayment = true;
            //spork
            bool found = false;
            CScript payee;
            if(!collateralnodePayments.GetBlockPayee(pindexPrev->nHeight+1, payee)){
                found = false;
                {
                    LOCK(cs_collateralnodes);
                    if (vecCollateralnodes.size() > 0) {
                        GetCollateralnodeRanks(pindexBest);
                        BOOST_FOREACH(PAIRTYPE(int, CCollateralNode*)& s, vecCollateralnodeScores)
                        {
                            if (s.second->nBlockLastPaid < pindexBest->nHeight - 10) {
                                payee.SetDestination(s.second->pubkey.GetID());
                                found = true;
                                break;
                            }
                        }
                    }
                }
                if (found) {
                    if (fDebug && fDebugCN) printf("CreateNewBlock: Found a collateralnode to pay: %s\n",payee.ToString(true).c_str());
                } else {
                    printf("CreateNewBlock: Failed to detect collateralnode to pay\n");
                    // pay the burn address if it can't detect
                    if (fDebug) printf("CreateNewBlock(): Failed to detect collateralnode to pay, burning coins.");
                    std::string burnAddress;
                    if (fTestNet) burnAddress = "8TestXXXXXXXXXXXXXXXXXXXXXXXXbCvpq";
                    else burnAddress = "INNXXXXXXXXXXXXXXXXXXXXXXXXXZeeDTw";
                    CBitcoinAddress burnAddr;
                    burnAddr.SetString(burnAddress);
                    payee = GetScriptForDestination(burnAddr.Get());
                }
            }

            if(hasPayment){
                payments = txNew.vout.size() + 1;
                if (fDebug && fDebugNet) printf("CreateNewBlock(): Payment Size: %i\n", payments);
                pblock->vtx[0].vout.resize(payments);

                pblock->vtx[0].vout[payments-1].scriptPubKey = payee;
                pblock->vtx[0].vout[payments-1].nValue = 0;

                CTxDestination address1;
                ExtractDestination(payee, address1);
                CBitcoinAddress address2(address1);

                if (fDebug && fDebugCN) printf("CreateNewBlock(): Collateralnode payment to %s\n", address2.ToString().c_str());
            }
        }

        // Priority order to process transactions
        list<COrphan> vOrphan; // list memory doesn't move
        map<uint256, vector<COrphan*> > mapDependers;

        // IDAG Phase 2: Collect txids and spent inputs from DAG sibling
        // blocks to avoid duplicates and fee accounting drift. ConnectBlock
        // skips transactions whose inputs were already spent by earlier DAG
        // siblings, so CreateNewBlock must exclude them before adding their
        // fees to the coinbase value.
        std::set<uint256> setDAGSiblingTxids;
        std::set<COutPoint> setDAGSiblingSpentOutpoints;
        if (nHeight >= FORK_HEIGHT_DAG && pindexPrev->phashBlock)
        {
            std::set<uint256> siblings = g_dagManager.GetDAGSiblingBlocks(pindexPrev->GetBlockHash());
            CBlockDAGData parentDagData;
            if (g_dagManager.GetDAGData(pindexPrev->GetBlockHash(), parentDagData))
            {
                BOOST_FOREACH(const uint256& hashChild, parentDagData.vDAGChildren)
                    siblings.insert(hashChild);
            }

            BOOST_FOREACH(const uint256& hashSib, siblings)
            {
                std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashSib);
                if (mi == mapBlockIndex.end())
                    continue;
                CBlock sibBlock;
                if (!sibBlock.ReadFromDisk(mi->second))
                    continue;
                for (const CTransaction& sibTx : sibBlock.vtx)
                {
                    if (sibTx.IsCoinBase() || sibTx.IsCoinStake())
                        continue;
                    setDAGSiblingTxids.insert(sibTx.GetHash());
                    BOOST_FOREACH(const CTxIn& txin, sibTx.vin)
                        setDAGSiblingSpentOutpoints.insert(txin.prevout);
                }
            }
        }

        // This vector will be sorted into a priority queue:
        vector<TxPriority> vecPriority;
        vecPriority.reserve(mempool.mapTx.size());
        for (map<uint256, CTransaction>::iterator mi = mempool.mapTx.begin(); mi != mempool.mapTx.end(); ++mi)
        {
            CTransaction& tx = (*mi).second;
            if (tx.IsCoinBase() || tx.IsCoinStake() || !tx.IsFinal())
                continue;

            // IDAG: Skip transactions already in DAG sibling blocks
            if (!setDAGSiblingTxids.empty() && setDAGSiblingTxids.count(tx.GetHash()))
                continue;
            if (TransactionSpendsAnyOutpoint(tx, setDAGSiblingSpentOutpoints))
                continue;

            // Transparent finality votes use existing UTXOs as stake proofs.
            // Consensus rejects blocks that both commit such a vote and spend
            // the proof UTXO, so reserve those outpoints while building the
            // candidate block.
            if (TransactionSpendsAnyOutpoint(tx, setFinalityStakeProofOutpoints))
                continue;

            COrphan* porphan = NULL;
            double dPriority = 0;
            int64_t nTotalIn = 0;
            bool fMissingInputs = false;
            BOOST_FOREACH(const CTxIn& txin, tx.vin)
            {
                if (tx.nVersion == ANON_TXN_VERSION
                    && txin.IsAnonInput()) // anon inputs are verified later in CheckAnonInputs()
                    continue;
                // Read prev transaction
                CTransaction txPrev;
                CTxIndex txindex;
                if (!txPrev.ReadFromDisk(txdb, txin.prevout, txindex))
                {
                    // This should never happen; all transactions in the memory
                    // pool should connect to either transactions in the chain
                    // or other transactions in the memory pool.
                    if (!mempool.mapTx.count(txin.prevout.hash))
                    {
                        printf("ERROR: mempool transaction missing input\n");
                        fMissingInputs = true;
                        if (porphan)
                            vOrphan.pop_back();
                        break;
                    }

                    // Has to wait for dependencies
                    if (!porphan)
                    {
                        // Use list for automatic deletion
                        vOrphan.push_back(COrphan(&tx));
                        porphan = &vOrphan.back();
                    }
                    mapDependers[txin.prevout.hash].push_back(porphan);
                    porphan->setDependsOn.insert(txin.prevout.hash);
                    nTotalIn += mempool.mapTx[txin.prevout.hash].vout[txin.prevout.n].nValue;
                    continue;
                }
                int64_t nValueIn = txPrev.vout[txin.prevout.n].nValue;
                nTotalIn += nValueIn;

                int nConf = txindex.GetDepthInMainChain();
                dPriority += (double)nValueIn * nConf;
            };

            if (tx.nVersion == ANON_TXN_VERSION)
            {
                int64_t nSumAnon;
                bool fInvalid;
                if (!tx.CheckAnonInputs(txdb, nSumAnon, fInvalid, false))
                {
                    if (fInvalid)
                        printf("CreateNewBlock() : CheckAnonInputs found invalid tx %s\n", tx.GetHash().ToString().substr(0,10).c_str());
                    fMissingInputs = true;
                    continue;
                };

                nTotalIn += nSumAnon;
            };

            if (fMissingInputs)
                continue;

            // Priority is sum(valuein * age) / txsize
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            dPriority /= nTxSize;

            // This is a more accurate fee-per-kilobyte than is used by the client code, because the
            // client code rounds up the size to the nearest 1K. That's good, because it gives an
            // incentive to create smaller transactions.
            int64_t nFee = nTotalIn-tx.GetValueOut();
            if (tx.IsShielded() && tx.nValueBalance != 0)
            {
                if ((tx.nValueBalance > 0 && nFee > std::numeric_limits<int64_t>::max() - tx.nValueBalance) ||
                    (tx.nValueBalance < 0 && nFee < std::numeric_limits<int64_t>::min() - tx.nValueBalance))
                {
                    printf("CreateNewBlock: fee overflow with shielded value balance, skipping tx\n");
                    continue;
                }
                nFee += tx.nValueBalance;
            }
            double dFeePerKb =  double(nFee) / (double(nTxSize)/1000.0);

            if (porphan)
            {
                porphan->dPriority = dPriority;
                porphan->dFeePerKb = dFeePerKb;
            }
            else
                vecPriority.push_back(TxPriority(dPriority, dFeePerKb, nFee, &(*mi).second));
        }

        // Collect transactions into block
        map<uint256, CTxIndex> mapTestPool;
        uint64_t nBlockSize = 1000;
        uint64_t nBlockTx = 0;
        int nBlockSigOps = 100;
        bool fSortedByFee = (nBlockPrioritySize <= 0);

        TxPriorityCompare comparer(fSortedByFee);
        std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);

        while (!vecPriority.empty())
        {
            // Take highest priority transaction off the priority queue:
            double dPriority = vecPriority.front().get<0>();
            double dFeePerKb = vecPriority.front().get<1>();
            int64_t nFee = vecPriority.front().get<2>();
            CTransaction& tx = *(vecPriority.front().get<3>());

            std::pop_heap(vecPriority.begin(), vecPriority.end(), comparer);
            vecPriority.pop_back();

            // Size limits
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            if (nBlockSize + nTxSize >= nBlockMaxSize)
                continue;

            // Legacy limits on sigOps:
            unsigned int nTxSigOps = tx.GetLegacySigOpCount();
            if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
                continue;

            // Timestamp limit
            if (tx.nTime > GetAdjustedTime() || (fProofOfStake && tx.nTime > pblock->vtx[0].nTime))
                continue;

            // Transaction fee
            int64_t nMinFee = tx.GetMinFee(nBlockSize, GMF_BLOCK); // will get GMF_ANON if tx.nVersion == ANON_TXN_VERSION

            // Skip free transactions if we're past the minimum block size:
            if (fSortedByFee && (dFeePerKb < nMinTxFee) && (nBlockSize + nTxSize >= nBlockMinSize))
                continue;

            // Prioritize by fee once past the priority size or we run out of high-priority
            // transactions:
            if (!fSortedByFee &&
                ((nBlockSize + nTxSize >= nBlockPrioritySize) || (dPriority < COIN * 144 / 250)))
            {
                fSortedByFee = true;
                comparer = TxPriorityCompare(fSortedByFee);
                std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);
            }

            // Connecting shouldn't fail due to dependency on other memory pool transactions
            // because we're already processing them in order of dependency
            map<uint256, CTxIndex> mapTestPoolTmp(mapTestPool);
            MapPrevTx mapInputs;
            bool fInvalid;
            if (!tx.FetchInputs(txdb, mapTestPoolTmp, false, true, mapInputs, fInvalid))
                continue;

            // -- Avoid calling CheckAnonInputs twice, use nFee from vecPriority
            //int64_t nTxFees = tx.GetValueIn(mapInputs)-tx.GetValueOut();
            if (nFee == 0) // tx came from COrphan
            {
                int64_t nTxFees = tx.GetValueIn(mapInputs)-tx.GetValueOut();

                if (tx.nVersion == ANON_TXN_VERSION)
                {
                    int64_t nSumAnon;
                    bool fInvalid;
                    if (!tx.CheckAnonInputs(txdb, nSumAnon, fInvalid, false))
                    {
                        if (fInvalid)
                            printf("CreateNewBlock() : CheckAnonInputs found invalid tx %s\n", tx.GetHash().ToString().substr(0,10).c_str());
                        continue;
                    };

                    nTxFees += nSumAnon;
                };
                if (tx.IsShielded() && tx.nValueBalance != 0)
                    nTxFees += tx.nValueBalance;
                nFee = nTxFees;
            };
            // TODO: must this be done twice!?
            // Need to look at COrphan
            if (nFee < nMinFee)
                continue;

            nTxSigOps += tx.GetP2SHSigOpCount(mapInputs);
            if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
                continue;

            if (!tx.ConnectInputs(txdb, mapInputs, mapTestPoolTmp, CDiskTxPos(1,1,1), pindexPrev, false, true, MANDATORY_SCRIPT_VERIFY_FLAGS))
                continue;
            mapTestPoolTmp[tx.GetHash()] = CTxIndex(CDiskTxPos(1,1,1), tx.vout.size());
            swap(mapTestPool, mapTestPoolTmp);

            // Added
            pblock->vtx.push_back(tx);
            nBlockSize += nTxSize;
            ++nBlockTx;
            nBlockSigOps += nTxSigOps;
            //nFees += nTxFees;
            nFees += nFee;

            if (fDebug && GetBoolArg("-printpriority"))
            {
                printf("priority %.1f feeperkb %.1f txid %s\n",
                       dPriority, dFeePerKb, tx.GetHash().ToString().c_str());
            }

            // Add transactions that depend on this one to the priority queue
            uint256 hash = tx.GetHash();
            if (mapDependers.count(hash))
            {
                BOOST_FOREACH(COrphan* porphan, mapDependers[hash])
                {
                    if (!porphan->setDependsOn.empty())
                    {
                        porphan->setDependsOn.erase(hash);
                        if (porphan->setDependsOn.empty())
                        {
                            vecPriority.push_back(TxPriority(porphan->dPriority, porphan->dFeePerKb, porphan->nFee, porphan->ptx));
                            std::push_heap(vecPriority.begin(), vecPriority.end(), comparer);
                        }
                    }
                }
            }
        }

        int64_t nFinalityRewardTotal = 0;
        if (!fProofOfStake && nHeight >= FORK_HEIGHT_DAG)
        {
            for (const CFinalityVote& vote : vFinalityVotesForBlock)
            {
                if (nFinalityRewardTotal > MAX_MONEY - vote.nReward)
                    break;

                CScript voteScript = BuildFinalityVoteScript(vote);
                unsigned int nVoteCommitSize = ::GetSerializeSize(voteScript, SER_NETWORK, PROTOCOL_VERSION);
                if (nBlockSize + nVoteCommitSize + 64 >= nBlockMaxSize)
                    break;

                CTxOut voteOut;
                voteOut.nValue = 0;
                voteOut.scriptPubKey = voteScript;
                pblock->vtx[0].vout.push_back(voteOut);

                CPubKey pubkey(vote.vchPubKey);
                if (!pubkey.IsValid())
                    continue;

                CTxOut rewardOut;
                rewardOut.nValue = vote.nReward;
                rewardOut.scriptPubKey = GetScriptForDestination(pubkey.GetID());
                pblock->vtx[0].vout.push_back(rewardOut);

                nFinalityRewardTotal += vote.nReward;
                nBlockSize += nVoteCommitSize + 64;
            }

            std::vector<CFinalityTallyShare> vFinalityShares = g_finalityTracker.GetPendingTallySharesForBlock(nHeight);
            for (const CFinalityTallyShare& share : vFinalityShares)
            {
                CScript shareScript = BuildFinalityTallyShareScript(share);
                unsigned int nShareCommitSize = ::GetSerializeSize(shareScript, SER_NETWORK, PROTOCOL_VERSION);
                if (nBlockSize + nShareCommitSize + 16 >= nBlockMaxSize)
                    break;

                CTxOut shareOut;
                shareOut.nValue = 0;
                shareOut.scriptPubKey = shareScript;
                pblock->vtx[0].vout.push_back(shareOut);
                nBlockSize += nShareCommitSize + 16;
            }

            std::vector<CFinalityTallyCertificate> vFinalityCerts = g_finalityTracker.GetPendingTallyCertificatesForBlock(nHeight);
            for (const CFinalityTallyCertificate& cert : vFinalityCerts)
            {
                CScript certScript = BuildFinalityTallyCertificateScript(cert);
                unsigned int nCertCommitSize = ::GetSerializeSize(certScript, SER_NETWORK, PROTOCOL_VERSION);
                if (nBlockSize + nCertCommitSize + 16 >= nBlockMaxSize)
                    break;

                CTxOut certOut;
                certOut.nValue = 0;
                certOut.scriptPubKey = certScript;
                pblock->vtx[0].vout.push_back(certOut);
                nBlockSize += nCertCommitSize + 16;
            }
        }

        nLastBlockTx = nBlockTx;
        nLastBlockSize = nBlockSize;

        int nRewardHeight = nHeight;
        if (nHeight < FORK_HEIGHT_TIGHTER_DRIFT && nHeight > 0)
            nRewardHeight = nHeight - 1;
        int64_t blockValue = GetProofOfWorkReward(nRewardHeight, nFees);
        if (!MoneyRange(blockValue))
        {
            printf("CreateNewBlock: ERROR: blockValue %" PRId64 " out of MoneyRange (nHeight=%d, nFees=%" PRId64 ")\n", blockValue, nHeight, nFees);
            return NULL;
        }
        int64_t collateralnodePayment = GetCollateralnodePayment(pindexPrev->nHeight+1, blockValue);

        //create collateralnode payment
        if(payments > 1){
            if (collateralnodePayment > blockValue)
            {
                printf("CreateNewBlock: WARNING: collateralnodePayment %" PRId64 " > blockValue %" PRId64 ", clamping\n",
                       collateralnodePayment, blockValue);
                collateralnodePayment = blockValue;
            }
            pblock->vtx[0].vout[payments-1].nValue = collateralnodePayment;
            blockValue -= collateralnodePayment;
        }

        if (fDebug && GetBoolArg("-printpriority"))
            printf("CreateNewBlock(): total size %" PRIu64"\n", nBlockSize);

        if (!fProofOfStake){
            pblock->vtx[0].vout[0].nValue = blockValue;
        }

        if (pFees)
            *pFees = nFees;

        // Fill in header
        pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
        pblock->nTime          = max(pindexPrev->GetPastTimeLimit()+1, pblock->GetMaxTransactionTime());
        pblock->nTime          = max(pblock->GetBlockTime(), PastDrift(pindexPrev->GetBlockTime(), pindexPrev->nHeight + 1));
        if (!fProofOfStake)
            pblock->UpdateTime(pindexPrev);
        pblock->nNonce         = 0;
    }

    return pblock.release();
}


void IncrementExtraNonce(CBlock* pblock, CBlockIndex* pindexPrev, unsigned int& nExtraNonce)
{
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock)
    {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }
    ++nExtraNonce;

    SetExtraNonce(pblock, pindexPrev, nExtraNonce);
}

void SetExtraNonce(CBlock* pblock, CBlockIndex* pindexPrev, unsigned int nExtraNonce)
{
    unsigned int nHeight = pindexPrev->nHeight+1; // Height first in coinbase required for block.version=2
    pblock->vtx[0].vin[0].scriptSig = (CScript() << nHeight << CBigNum(nExtraNonce)) + COINBASE_FLAGS;
    if (pblock->vtx[0].vin[0].scriptSig.size() > 100)
    {
        printf("IncrementExtraNonce() : coinbase scriptSig too large (%d bytes)\n", (int)pblock->vtx[0].vin[0].scriptSig.size());
        return;
    }

    pblock->hashMerkleRoot = pblock->BuildMerkleTree();
}


void FormatHashBuffers(CBlock* pblock, char* pmidstate, char* pdata, char* phash1)
{
    //
    // Pre-build hash buffers
    //
    struct
    {
        struct unnamed2
        {
            int nVersion;
            uint256 hashPrevBlock;
            uint256 hashMerkleRoot;
            unsigned int nTime;
            unsigned int nBits;
            unsigned int nNonce;
        }
        block;
        unsigned char pchPadding0[64];
        uint256 hash1;
        unsigned char pchPadding1[64];
    }
    tmp;
    memset(&tmp, 0, sizeof(tmp));

    tmp.block.nVersion       = pblock->nVersion;
    tmp.block.hashPrevBlock  = pblock->hashPrevBlock;
    tmp.block.hashMerkleRoot = pblock->hashMerkleRoot;
    tmp.block.nTime          = pblock->nTime;
    tmp.block.nBits          = pblock->nBits;
    tmp.block.nNonce         = pblock->nNonce;

    FormatHashBlocks(&tmp.block, sizeof(tmp.block));
    FormatHashBlocks(&tmp.hash1, sizeof(tmp.hash1));

    // Byte swap all the input buffer
    for (unsigned int i = 0; i < sizeof(tmp)/4; i++)
        ((unsigned int*)&tmp)[i] = ByteReverse(((unsigned int*)&tmp)[i]);

    // Precalc the first half of the first hash, which stays constant
    SHA256Transform(pmidstate, &tmp.block, pSHA256InitState);

    memcpy(pdata, &tmp.block, 128);
    memcpy(phash1, &tmp.hash1, 64);
}


bool CheckWork(CBlock* pblock, CWallet& wallet, CReserveKey& reservekey)
{
    uint256 hashBlock = pblock->GetHash();
    uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();

    if(!pblock->IsProofOfWork())
        return error("CheckWork() : %s is not a proof-of-work block", hashBlock.GetHex().c_str());

    if (hashBlock > hashTarget)
        return error("CheckWork() : proof-of-work not meeting target");

    //// debug print
    printf("CheckWork() : new proof-of-work block found  \n  hash: %s  \ntarget: %s\n", hashBlock.GetHex().c_str(), hashTarget.GetHex().c_str());
    pblock->print();
    printf("generated %s\n", FormatMoney(pblock->vtx[0].vout[0].nValue).c_str());

    // Found a solution
    {
        LOCK(cs_main);
        if (pblock->hashPrevBlock != hashBestChain)
        {
            reservekey.ReturnKey();
            return error("CheckWork() : generated block is stale");
        }

        // Track how many getdata requests this block gets
        {
            LOCK(wallet.cs_wallet);
            wallet.mapRequestCount[hashBlock] = 0;
        }

        // Process this block the same as if we had received it from another node
        if (!ProcessBlock(NULL, pblock))
        {
            reservekey.ReturnKey();
            return error("CheckWork() : ProcessBlock, block not accepted");
        }

        // Consume the reserved coinbase key only after full validation accepts
        // the block. A rejected submission returns it to the keypool above.
        {
            LOCK(wallet.cs_wallet);
            reservekey.KeepKey();
        }
    }

    return true;
}

bool CheckStake(CBlock* pblock, CWallet& wallet)
{
    uint256 proofHash = 0, hashTarget = 0;
    uint256 hashBlock = pblock->GetHash();

    if(!pblock->IsProofOfStake())
        return error("CheckStake() : %s is not a proof-of-stake block", hashBlock.GetHex().c_str());
    {
        LOCK(cs_main);
        if (pindexBest && pindexBest->nHeight + 1 >= FORK_HEIGHT_DAG)
            return error("CheckStake() : proof-of-stake block production disabled after DAG fork");
    }

   // verify hash target and signature of coinstake tx -
    //if (!CheckProofOfStake(mapBlockIndex[pblock->hashPrevBlock], pblock->vtx[1], pblock->nBits, proofHash, hashTarget))
	if (!CheckProofOfStake(pblock->vtx[1], pblock->nBits, proofHash, hashTarget))
        return error("CheckStake() : proof-of-stake checking failed");

    //// debug print
    printf("CheckStake() : new proof-of-stake block found  \n  hash: %s \nproofhash: %s  \ntarget: %s\n", hashBlock.GetHex().c_str(), proofHash.GetHex().c_str(), hashTarget.GetHex().c_str());
    pblock->print();
    printf("out %s\n", FormatMoney(pblock->vtx[1].GetValueOut()).c_str());

    // Found a solution
    {
        LOCK(cs_main);
        if (pblock->hashPrevBlock != hashBestChain)
            return error("CheckStake() : generated block is stale");

        // Track how many getdata requests this block gets
        {
            LOCK(wallet.cs_wallet);
            wallet.mapRequestCount[hashBlock] = 0;
        }

        // Process this block the same as if we had received it from another node
        if (!ProcessBlock(NULL, pblock))
            return error("CheckStake() : ProcessBlock, block not accepted");
    }

    return true;
}

void StakeMiner(CWallet *pwallet)
{
    SetThreadPriority(THREAD_PRIORITY_LOWEST);

    // Make this thread recognisable as the mining thread
    RenameThread("innova-miner");

    bool fTryToSync = true;
    int64_t nTimeLastStake = 0;
    int nLastFinalityEpochVoted = -1;

    while (true)
    {
        if (fShutdown)
            return;

        while (pwallet->IsLocked())
        {
            nLastCoinStakeSearchInterval = 0;
            MilliSleep(2000);
            if (fShutdown)
                return;
        }

        bool fWaitForSync;
        {
            LOCK(cs_vNodes);
            fWaitForSync = (!fRegTest && vNodes.empty()) || (!fRegTest && IsInitialBlockDownload() && !fHybridSPV);
        }
        while (fWaitForSync)
        {
            nLastCoinStakeSearchInterval = 0;
            fTryToSync = true;
            if (fDebug  && GetBoolArg("-printcoinstake"))
                printf("StakeMiner() IsInitialBlockDownload\n");
            MilliSleep(2000);
            if (fShutdown)
                return;
            {
                LOCK(cs_vNodes);
                fWaitForSync = vNodes.empty() || (IsInitialBlockDownload() && !fHybridSPV);
            }
        }

        if (fTryToSync)
        {
            fTryToSync = false;
            bool fTooFewNodes;
            {
                LOCK(cs_vNodes);
                fTooFewNodes = vNodes.size() < 3;
            }
            if (fTooFewNodes || nBestHeight < GetNumBlocksOfPeers())
            {
                if (fDebug  && GetBoolArg("-printcoinstake"))
                    printf("StakeMiner() vNodes.size() < 3 || nBestHeight < GetNumBlocksOfPeers()\n");
                vnThreadsRunning[THREAD_STAKE_MINER]--;
                MilliSleep(5000);
                vnThreadsRunning[THREAD_STAKE_MINER]++;
				if (fShutdown)
                    return;
            }
        }

        if (!fRegTest && !fHybridSPV && nBestHeight < GetNumBlocksOfPeers()-1)
        {
            if (fDebug  && GetBoolArg("-printcoinstake"))
                printf("StakeMiner() nBestHeight < GetNumBlocksOfPeers()\n");
            MilliSleep(nMinerSleep * 4);
            continue;
        };

        // Pause staking while chain is stale to yield cs_main for sync.
        // Staking during active sync causes cs_main contention that starves
        // ThreadMessageHandler, preventing block/inv processing.
        bool fChainStale = false;
        {
            LOCK(cs_main);
            fChainStale = !fRegTest && !fTestNet && pindexBest &&
                          pindexBest->GetBlockTime() < GetTime() - 300;
        }
        if (fChainStale)
        {
            if (fDebug && GetBoolArg("-printcoinstake"))
                printf("StakeMiner() chain stale, pausing for sync\n");
            MilliSleep(5000);
            continue;
        }

        // IDAG: after the DAG fork, stakers no longer create blocks. The
        // staking thread only produces transparent finality votes.
        bool fPostDAGFinalityMode = false;
        bool fShouldProduceFinalityVote = false;
        int nFinalityEpoch = -1;
        {
            LOCK(cs_main);
            if (pindexBest && pindexBest->nHeight >= FORK_HEIGHT_DAG)
            {
                fPostDAGFinalityMode = true;
                nFinalityEpoch = GetEpochForHeight(pindexBest->nHeight);
                int nEpochBoundary = GetEpochBoundaryHeight(nFinalityEpoch, pindexBest->nHeight);
                int nEpochProgress = pindexBest->nHeight - nEpochBoundary;
                fShouldProduceFinalityVote = (nEpochProgress < FINALITY_VOTE_WINDOW &&
                                              nFinalityEpoch != nLastFinalityEpochVoted);
            }
        }
        if (fPostDAGFinalityMode)
        {
            ProcessFinalityTallyCommittee();
            if (fShouldProduceFinalityVote && ProduceFinalityVote())
                nLastFinalityEpochVoted = nFinalityEpoch;
            MilliSleep(5000);
            continue;
        }

        // Post-DAG: reduce stake interval to match nMaxStakeSearchInterval (2s)
        // This ensures no timestamp slots are skipped between staking attempts
        int64_t nEffectiveStakeInterval = nMinStakeInterval;
        {
            LOCK(cs_main);
            if (pindexBest && pindexBest->nHeight >= FORK_HEIGHT_DAG)
                nEffectiveStakeInterval = std::min(nEffectiveStakeInterval, (int64_t)2);
        }
        if (nEffectiveStakeInterval > 0 && nTimeLastStake + nEffectiveStakeInterval > GetTime())
        {
            if (fDebug && GetBoolArg("-printcoinstake"))
                printf("StakeMiner() Rate limited to 1 / %d seconds.\n", (int)nEffectiveStakeInterval);
            MilliSleep(nEffectiveStakeInterval * 1000);
            continue;
        };

        if (vecCollateralnodes.size() == 0 && !fTestNet && !fRegTest)
        {
            if (fDebug && GetBoolArg("-printcoinstake")) printf("StakeMiner() waiting for CN list.");
            vnThreadsRunning[THREAD_STAKE_MINER]--;
            MilliSleep(10000);
            vnThreadsRunning[THREAD_STAKE_MINER]++;
            continue;
        }

        //
        // Create new block
        //
        int64_t nFees;
        if (fDebug && GetBoolArg("-printcoinstake")) printf ("creating block. ");
        auto_ptr<CBlock> pblock(CreateNewBlock(pwallet, true, &nFees));
        if (!pblock.get())
        {
            printf("StakeMiner: CreateNewBlock failed, retrying...\n");
            MilliSleep(5000);
            continue;
        }

        if (fDebug && GetBoolArg("-printcoinstake")) printf ("signing block. ");
        // Trying to sign a block
        if (pblock->SignBlock(*pwallet, nFees))
        {
            if (fDebug && GetBoolArg("-printcoinstake")) printf ("checking stake. ");
            bool staked;
            SetThreadPriority(THREAD_PRIORITY_NORMAL);
            staked = CheckStake(pblock.get(), *pwallet);
            if (staked && fDebug && GetBoolArg("-printcoinstake")) printf ("stake is good. \n");
            SetThreadPriority(THREAD_PRIORITY_LOWEST);
			if (fShutdown)
                return;
            MilliSleep(nMinerSleep);
            if (staked) {
                nTimeLastStake = GetAdjustedTime();
                MilliSleep(nMinerSleep*3); // sleep for a while after successfully staking
            }
            else if (fDebug && GetBoolArg("-printcoinstake")) printf ("stake is bad. \n");
        }
        else
        {
            if (fDebug && GetBoolArg("-printcoinstake")) printf ("failed to sign.\n");
			if (fShutdown)
                return;
            MilliSleep(nMinerSleep);
        }
    }
}

namespace
{
CPUMiningWorkIdentity CaptureCurrentCPUMiningWorkIdentity()
{
    CPUMiningWorkIdentity identity;
    LOCK(cs_main);

    identity.hashBestChain = hashBestChain;
    CBlockIndex* pindexParent = g_dagManager.SelectBestDAGTip();
    if (!pindexParent)
        pindexParent = pindexBest;
    if (pindexParent)
    {
        identity.hashPrimaryParent = pindexParent->GetBlockHash();
        identity.nHeight = pindexParent->nHeight + 1;
    }
    if (identity.nHeight >= FORK_HEIGHT_DAG)
    {
        identity.vDAGTips = g_dagManager.GetDAGTips();
        std::sort(identity.vDAGTips.begin(), identity.vDAGTips.end());
    }
    identity.nTransactionsUpdated = mempool.GetTransactionsUpdated();
    return identity;
}

bool IsCPUMiningCollateralStateReady()
{
    int nNextHeight = 0;
    {
        LOCK2(cs_main, g_dagManager.cs_dag);
        CBlockIndex* pindexParent = g_dagManager.SelectBestDAGTip();
        if (!pindexParent)
            pindexParent = pindexBest;
        if (!pindexParent)
            return false;
        nNextHeight = pindexParent->nHeight + 1;
    }

    bool fPaymentsRequired = fTestNet
                                 ? nNextHeight >= BLOCK_START_COLLATERALNODE_PAYMENTS_TESTNET
                                 : nNextHeight >= BLOCK_START_COLLATERALNODE_PAYMENTS &&
                                       nNextHeight >= 2085000;
    int nEnforcementHeight = fTestNet
                                 ? MN_ENFORCEMENT_ACTIVE_HEIGHT_TESTNET
                                 : MN_ENFORCEMENT_ACTIVE_HEIGHT;
    if (!fPaymentsRequired || nNextHeight < nEnforcementHeight)
        return true;

    LOCK(cs_collateralnodes);
    return !vecCollateralnodes.empty();
}

bool IsCPUMiningWorkCurrent(const CPUMiningWorkIdentity& identity, bool fCheckMempool)
{
    return CPUMiningWorkIdentityMatches(identity,
                                        CaptureCurrentCPUMiningWorkIdentity(),
                                        fCheckMempool);
}

void RunCPUMinerWorker(CCPUMinerController& controller, CWallet* pwallet,
                       unsigned int nWorkerId, unsigned int nThreads,
                       uint64_t nSessionId)
{
    SetThreadPriority(THREAD_PRIORITY_LOWEST);
    RenameThread("innova-cpuminer");

    uint64_t nExtraNonceStart = nSessionId * CCPUMinerController::MAX_THREADS + nWorkerId + 1;
    unsigned int nExtraNonce = static_cast<unsigned int>(nExtraNonceStart);
    if (nExtraNonce == 0)
        nExtraNonce = nWorkerId + 1;

    while (!controller.ShouldStop(nSessionId))
    {
        bool fNoNodes;
        {
            LOCK(cs_vNodes);
            fNoNodes = vNodes.empty();
        }

        if (!fRegTest && (fNoNodes || IsInitialBlockDownload()))
        {
            controller.WaitForStop(nSessionId, 1000);
            continue;
        }

        bool fChainStale = false;
        {
            LOCK(cs_main);
            fChainStale = !fRegTest && !fTestNet && pindexBest && pindexBest->nHeight > 10 &&
                          pindexBest->GetBlockTime() < GetTime() - 300;
        }
        if (fChainStale)
        {
            controller.WaitForStop(nSessionId, 5000);
            continue;
        }

        if (!IsCPUMiningCollateralStateReady())
        {
            if (fDebug)
                printf("CPUMiner: waiting for Collateral Node state\n");
            controller.WaitForStop(nSessionId, 5000);
            continue;
        }

        CPUMiningWorkIdentity identityBefore = CaptureCurrentCPUMiningWorkIdentity();
        CReserveKey reservekey(pwallet);
        std::unique_ptr<CBlock> pblock(CreateNewBlock(pwallet, false, NULL, &reservekey));
        if (!pblock.get())
        {
            reservekey.ReturnKey();
            printf("CPUMiner: CreateNewBlock failed, retrying...\n");
            controller.WaitForStop(nSessionId, 1000);
            continue;
        }

        CPUMiningWorkIdentity identityAfter = CaptureCurrentCPUMiningWorkIdentity();
        if (!CPUMiningWorkIdentityMatches(identityBefore, identityAfter, true) ||
            !CPUMiningBlockMatchesWorkIdentity(*pblock, identityAfter))
        {
            reservekey.ReturnKey();
            controller.WaitForStop(nSessionId, 50);
            continue;
        }

        CPUMiningWorkIdentity workIdentity = identityAfter;
        uint256 hashTarget;
        CBlockIndex* pindexBlockPrev = NULL;
        bool fPrepared = false;
        {
            LOCK(cs_main);
            std::map<uint256, CBlockIndex*>::iterator miPrev = mapBlockIndex.find(pblock->hashPrevBlock);
            if (miPrev != mapBlockIndex.end() && miPrev->second)
            {
                pindexBlockPrev = miPrev->second;
                if (pindexBlockPrev->nHeight + 1 == workIdentity.nHeight)
                {
                    CBigNum bnTarget;
                    SetExtraNonce(pblock.get(), pindexBlockPrev, nExtraNonce);
                    bnTarget.SetCompact(pblock->nBits);
                    hashTarget = bnTarget.getuint256();
                    fPrepared = true;
                }
            }
        }

        uint64_t nNextExtraNonce = static_cast<uint64_t>(nExtraNonce) + nThreads;
        nExtraNonce = nNextExtraNonce > std::numeric_limits<unsigned int>::max()
                          ? nWorkerId + 1
                          : static_cast<unsigned int>(nNextExtraNonce);

        if (!fPrepared || !IsCPUMiningWorkCurrent(workIdentity, false))
        {
            reservekey.ReturnKey();
            controller.WaitForStop(nSessionId, 50);
            continue;
        }

        if (fDebug)
        {
            std::string strDAGTips;
            for (const uint256& hashTip : workIdentity.vDAGTips)
            {
                if (!strDAGTips.empty())
                    strDAGTips += ",";
                strDAGTips += hashTip.ToString();
            }
            printf("CPUMiner[%u]: Work identity best=%s parent=%s dag-tips=[%s]\n",
                   nWorkerId, workIdentity.hashBestChain.ToString().c_str(),
                   workIdentity.hashPrimaryParent.ToString().c_str(),
                   strDAGTips.c_str());
        }

        printf("CPUMiner[%u]: Mining block at height %d, target bits=0x%08x\n",
               nWorkerId, workIdentity.nHeight, pblock->nBits);

        int64_t nStart = GetTime();
        uint64_t nHashesDone = 0;
        bool fBlockFound = false;
        while (!controller.ShouldStop(nSessionId))
        {
            uint256 hash = pblock->GetPoWHash();
            if (hash <= hashTarget)
            {
                fBlockFound = true;
                break;
            }

            ++nHashesDone;
            ++pblock->nNonce;
            if (pblock->nNonce == 0)
                ++pblock->nTime;

            if ((nHashesDone % 500000) == 0)
            {
                int64_t nElapsed = GetTime() - nStart;
                if (nElapsed > 0)
                    printf("CPUMiner[%u]: %.0f H/s (height %d, %llu hashes)\n",
                           nWorkerId, (double)nHashesDone / nElapsed,
                           workIdentity.nHeight, (unsigned long long)nHashesDone);
            }

            if ((nHashesDone % 4096) == 0)
            {
                // Keep the timestamp current without changing the coinbase or
                // merkle root. GetNextTargetRequired is timestamp-independent.
                pblock->UpdateTime(pindexBlockPrev);
                bool fCheckMempool = GetTime() - nStart >= 5;
                if (!IsCPUMiningWorkCurrent(workIdentity, fCheckMempool))
                    break;
            }
        }

        bool fAccepted = false;
        if (fBlockFound && !controller.ShouldStop(nSessionId) &&
            IsCPUMiningCollateralStateReady())
        {
            uint256 hashBlock = pblock->GetHash();
            LOCK(cs_main);
            if (!controller.ShouldStop(nSessionId) &&
                IsCPUMiningWorkCurrent(workIdentity, false) &&
                IsCPUMiningCollateralStateReady())
            {
                printf("CPUMiner[%u]: Found block at height %d, nonce=%u\n",
                       nWorkerId, workIdentity.nHeight, pblock->nNonce);
                fAccepted = ProcessBlock(NULL, pblock.get()) && mapBlockIndex.count(hashBlock) != 0;
            }
        }

        if (fAccepted)
        {
            {
                LOCK(pwallet->cs_wallet);
                reservekey.KeepKey();
            }
            printf("CPUMiner[%u]: Block accepted at height %d\n",
                   nWorkerId, workIdentity.nHeight);
        }
        else
        {
            reservekey.ReturnKey();
            if (fBlockFound)
                printf("CPUMiner[%u]: Found block was stale or rejected\n", nWorkerId);
        }

        controller.WaitForStop(nSessionId, fBlockFound ? 100 : 50);
    }
}

std::thread CreateCPUMinerThread(const std::function<void()>& function)
{
    return std::thread(function);
}
}

bool CPUMiningWorkIdentityMatches(const CPUMiningWorkIdentity& a,
                                  const CPUMiningWorkIdentity& b,
                                  bool fCheckMempool)
{
    if (a.hashBestChain != b.hashBestChain ||
        a.hashPrimaryParent != b.hashPrimaryParent ||
        a.vDAGTips != b.vDAGTips)
        return false;
    return !fCheckMempool || a.nTransactionsUpdated == b.nTransactionsUpdated;
}

bool CPUMiningBlockMatchesWorkIdentity(const CBlock& block,
                                       const CPUMiningWorkIdentity& identity)
{
    if (block.hashPrevBlock != identity.hashPrimaryParent)
        return false;
    if (identity.nHeight < FORK_HEIGHT_DAG)
        return true;
    if (block.vtx.empty())
        return false;

    std::vector<uint256> vParents;
    for (const CTxOut& output : block.vtx[0].vout)
    {
        vParents = ExtractDAGParents(output.scriptPubKey);
        if (!vParents.empty())
            break;
    }
    if (vParents.empty() || vParents[0] != identity.hashPrimaryParent)
        return false;

    for (size_t i = 1; i < vParents.size(); ++i)
        if (std::find(identity.vDAGTips.begin(), identity.vDAGTips.end(), vParents[i]) ==
            identity.vDAGTips.end())
            return false;
    return true;
}

const int CCPUMinerController::MAX_THREADS;

CCPUMinerController::CCPUMinerController(const WorkerFunction& worker,
                                         const ThreadFactory& threadFactory)
    : m_worker(worker ? worker : WorkerFunction(RunCPUMinerWorker)),
      m_threadFactory(threadFactory ? threadFactory : ThreadFactory(CreateCPUMinerThread)),
      m_state(STOPPED),
      m_stopRequested(true),
      m_shutdownRequested(false),
      m_workerFailed(false),
      m_startReleased(false),
      m_requestedThreads(0),
      m_activeThreads(0),
      m_sessionId(0)
{
}

CCPUMinerController::~CCPUMinerController()
{
    Shutdown();
}

void CCPUMinerController::SetState(State state)
{
    m_state.store(state, std::memory_order_release);
    m_stateChanged.notify_all();
}

bool CCPUMinerController::Start(CWallet* pwallet, int nThreads, std::string& strError)
{
    if (!pwallet)
    {
        strError = "CPU mining requires a wallet";
        return false;
    }
    if (nThreads < 1 || nThreads > MAX_THREADS)
    {
        strError = "CPU mining thread count is out of range";
        return false;
    }

    std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
    if (m_shutdownRequested.load(std::memory_order_acquire))
    {
        strError = "CPU miner is shutting down";
        return false;
    }

    Status status = GetStatus();
    if (status.running && status.requestedThreads == nThreads &&
        status.activeThreads == nThreads && m_threads.size() == static_cast<size_t>(nThreads))
        return true;

    StopAndJoinLocked(STOPPED);

    uint64_t nSessionId;
    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        m_stopRequested.store(false, std::memory_order_release);
        m_workerFailed.store(false, std::memory_order_release);
        m_startReleased.store(false, std::memory_order_release);
        m_requestedThreads.store(nThreads, std::memory_order_release);
        nSessionId = m_sessionId.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    SetState(STARTING);

    try
    {
        m_threads.reserve(nThreads);
        for (int i = 0; i < nThreads; ++i)
        {
            std::function<void()> function = std::bind(&CCPUMinerController::WorkerEntry,
                                                       this, pwallet,
                                                       static_cast<unsigned int>(i),
                                                       static_cast<unsigned int>(nThreads),
                                                       nSessionId);
            m_threads.push_back(m_threadFactory(function));
        }
    }
    catch (std::exception& e)
    {
        strError = std::string("Failed to create CPU mining thread: ") + e.what();
        StopAndJoinLocked(STOPPED);
        return false;
    }
    catch (...)
    {
        strError = "Failed to create CPU mining thread";
        StopAndJoinLocked(STOPPED);
        return false;
    }

    bool fStartupFailed;
    {
        std::unique_lock<std::mutex> stateLock(m_stateMutex);
        m_stateChanged.wait(stateLock, [this, nThreads]() {
            return m_activeThreads.load(std::memory_order_acquire) == nThreads ||
                   m_workerFailed.load(std::memory_order_acquire);
        });
        fStartupFailed = m_workerFailed.load(std::memory_order_acquire);
        if (!fStartupFailed)
        {
            m_startReleased.store(true, std::memory_order_release);
            SetState(RUNNING);
        }
    }

    if (fStartupFailed)
    {
        strError = "CPU mining worker failed during startup";
        StopAndJoinLocked(STOPPED);
        return false;
    }

    m_stateChanged.notify_all();
    return true;
}

void CCPUMinerController::StopAndJoinLocked(State finalState)
{
    if (!m_threads.empty() || m_activeThreads.load(std::memory_order_acquire) > 0)
        SetState(finalState == SHUTTING_DOWN ? SHUTTING_DOWN : STOPPING);

    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        m_stopRequested.store(true, std::memory_order_release);
        m_startReleased.store(true, std::memory_order_release);
    }
    m_stopChanged.notify_all();
    m_stateChanged.notify_all();

    for (std::thread& thread : m_threads)
    {
        if (thread.joinable())
            thread.join();
    }
    m_threads.clear();

    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        m_activeThreads.store(0, std::memory_order_release);
        m_requestedThreads.store(0, std::memory_order_release);
        m_workerFailed.store(false, std::memory_order_release);
    }
    SetState(finalState);
}

void CCPUMinerController::Stop()
{
    std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
    StopAndJoinLocked(m_shutdownRequested.load(std::memory_order_acquire)
                          ? SHUTTING_DOWN : STOPPED);
}

void CCPUMinerController::Shutdown()
{
    std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        m_shutdownRequested.store(true, std::memory_order_release);
    }
    StopAndJoinLocked(SHUTTING_DOWN);
}

CCPUMinerController::Status CCPUMinerController::GetStatus() const
{
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    Status status;
    status.state = static_cast<State>(m_state.load(std::memory_order_acquire));
    status.activeThreads = m_activeThreads.load(std::memory_order_acquire);
    status.requestedThreads = m_requestedThreads.load(std::memory_order_acquire);
    status.shutdown = m_shutdownRequested.load(std::memory_order_acquire);
    status.stopping = status.state == STOPPING || status.state == SHUTTING_DOWN;
    status.running = status.state == RUNNING && status.activeThreads > 0 &&
                     !status.shutdown &&
                     !m_stopRequested.load(std::memory_order_acquire);
    status.sessionId = m_sessionId.load(std::memory_order_acquire);
    return status;
}

bool CCPUMinerController::ShouldStop(uint64_t nSessionId) const
{
    return m_stopRequested.load(std::memory_order_acquire) ||
           m_shutdownRequested.load(std::memory_order_acquire) ||
           m_sessionId.load(std::memory_order_acquire) != nSessionId;
}

bool CCPUMinerController::WaitForStop(uint64_t nSessionId, int nMilliseconds)
{
    if (ShouldStop(nSessionId))
        return true;
    std::unique_lock<std::mutex> stateLock(m_stateMutex);
    m_stopChanged.wait_for(stateLock, std::chrono::milliseconds(nMilliseconds),
                           [this, nSessionId]() { return ShouldStop(nSessionId); });
    return ShouldStop(nSessionId);
}

void CCPUMinerController::WorkerEntry(CWallet* pwallet, unsigned int nWorkerId,
                                      unsigned int nThreads, uint64_t nSessionId)
{
    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        m_activeThreads.fetch_add(1, std::memory_order_acq_rel);
    }
    m_stateChanged.notify_all();

    try
    {
        std::unique_lock<std::mutex> stateLock(m_stateMutex);
        m_stateChanged.wait(stateLock, [this, nSessionId]() {
            return m_startReleased.load(std::memory_order_acquire) || ShouldStop(nSessionId);
        });
        stateLock.unlock();

        if (!ShouldStop(nSessionId))
            m_worker(*this, pwallet, nWorkerId, nThreads, nSessionId);

        if (!ShouldStop(nSessionId))
            MarkWorkerFailed();
    }
    catch (std::exception& e)
    {
        MarkWorkerFailed();
        PrintException(&e, "CPUMiner worker");
    }
    catch (...)
    {
        MarkWorkerFailed();
        PrintException(NULL, "CPUMiner worker");
    }

    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        m_activeThreads.fetch_sub(1, std::memory_order_acq_rel);
    }
    m_stopChanged.notify_all();
    m_stateChanged.notify_all();
}

void CCPUMinerController::MarkWorkerFailed()
{
    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        m_workerFailed.store(true, std::memory_order_release);
        m_stopRequested.store(true, std::memory_order_release);
        if (!m_shutdownRequested.load(std::memory_order_acquire))
            m_state.store(STOPPING, std::memory_order_release);
    }
    m_stopChanged.notify_all();
    m_stateChanged.notify_all();
}

CCPUMinerController& GetCPUMinerController()
{
    static CCPUMinerController controller;
    return controller;
}

void ShutdownCPUMining()
{
    GetCPUMinerController().Shutdown();
}
