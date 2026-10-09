// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "main.h"
#include "innovarpc.h"
#include "init.h"
#include "txdb.h"
#include "bootstrap.h"
#include "blockindex_authoritative_startup.h"
#include "blockindex_manager.h"
#include "collateralnode.h"
#include "activecollateralnode.h"
#include "db.h"
#include "base58.h"
#include "net.h"
#include <errno.h>
#include <fstream>
#include <chrono>

static int64_t RPCPerfTimeMicros()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
#define RPCPERF_LOG(...) do { try { printf(__VA_ARGS__); } catch (...) {} } while (0)

#include <boost/filesystem.hpp>

using namespace json_spirit;
using namespace std;

extern void TxToJSON(const CTransaction& tx, const uint256 hashBlock, json_spirit::Object& entry);
extern enum Checkpoints::CPMode CheckpointsMode;
extern void spj(const CScript& scriptPubKey, Object& out, bool fIncludeHex);

double BitsToDouble(unsigned int nBits)
{
    // Floating point number that is a multiple of the minimum difficulty,
    // minimum difficulty = 1.0.
    int nShift = (nBits >> 24) & 0xff;

    double dDiff = (double)0x0000ffff / (double)(nBits & 0x00ffffff);

    while (nShift < 29)
    {
        dDiff *= 256.0;
        nShift++;
    };

    while (nShift > 29)
    {
        dDiff /= 256.0;
        nShift--;
    };

    return dDiff;
};

double GetDifficulty(const CBlockIndex* blockindex)
{
    if (blockindex == NULL)
    {
        if (pindexBest == NULL)
            return 1.0;
        else
            blockindex = GetLastBlockIndex(pindexBest, false);
    };

    return BitsToDouble(blockindex->nBits);
}

// Stage D: authoritative difficulty read group. When the by-value Block Index
// Manager can serve (authoritative mode / bound reader), derive BOTH difficulty
// figures from manager-backed by-value history — the nearest PoW and nearest PoS
// block at or before the active tip — using the SAME BitsToDouble formula. In
// legacy mode, or if the authority cannot answer, it declines so the caller uses
// the existing CBlockIndex path; output/error semantics are unchanged.
bool ManagerDifficultyPair(double* dPoW, double* dPoS)
{
    BlockIndexManager& mgr = GetBlockIndexManager();
    std::string e;
    if (!mgr.IsAvailable(&e))
        return false;
    uint256 tip;
    if (mgr.BestTipHash(&tip, &e) != BLOCK_INDEX_MANAGER_OK)
        return false;
    BlockIndexSnapshot powBlock, posBlock;
    if (mgr.GetLastBlockIndexByProofType(tip, false, &powBlock, &e) != BLOCK_INDEX_MANAGER_OK)
        return false;
    if (mgr.GetLastBlockIndexByProofType(tip, true, &posBlock, &e) != BLOCK_INDEX_MANAGER_OK)
        return false;
    *dPoW = BitsToDouble(powBlock.nBits);
    *dPoS = BitsToDouble(posBlock.nBits);
    return true;
}

// R1 slice-1 (best-tip pointer decoupling): resolve the ACTIVE TIP's scalar
// metadata BY VALUE through the authoritative Block Index Manager, with NO
// dependency on the historical CBlockIndex object `pindexBest`.
//
// Returns true when the manager (authoritative mode, or a bound V2 reader in
// tests) resolved the committed tip. Returns false ONLY when no by-value
// authority is available; in that case an authoritative caller MUST fail
// closed and must NEVER fall back to `pindexBest`, while a non-authoritative
// caller may consult the legacy globals (legacy-only behavior retained).
static bool BestTipSnapshotByValue(BlockIndexSnapshot* out, std::string* error)
{
    return GetBlockIndexManager().GetTip(out, error) == BLOCK_INDEX_MANAGER_OK;
}

double GetPoWMHashPS()
{
    const bool fRPCPerfTrace = GetBoolArg("-rpcperftrace", false);
    const int64_t nStartTime = fRPCPerfTrace ? RPCPerfTimeMicros() : 0;
    int nPoWInterval = 72;
    int nPoWBlocksToCheck = 100000; // Only look at last 100000 blocks max
    int64_t nTargetSpacingWorkMin = 30, nTargetSpacingWork = 30;

    CBlockIndex* pindex = pindexBest;
    CBlockIndex* pindexPrevWork = NULL;
    int nBlocksChecked = 0;
    int nPoWBlocksFound = 0;

    while (pindex && nBlocksChecked < nPoWBlocksToCheck && nPoWBlocksFound < nPoWInterval)
    {
        if (pindex->IsProofOfWork())
        {
            if (pindexPrevWork)
            {
                int64_t nActualSpacingWork = pindexPrevWork->GetBlockTime() - pindex->GetBlockTime();
                if (nActualSpacingWork > 0)
                {
                    nTargetSpacingWork = ((nPoWInterval - 1) * nTargetSpacingWork + nActualSpacingWork + nActualSpacingWork) / (nPoWInterval + 1);
                    nTargetSpacingWork = max(nTargetSpacingWork, nTargetSpacingWorkMin);
                }
            }
            pindexPrevWork = pindex;
            nPoWBlocksFound++;
        }

        pindex = pindex->pprev;
        nBlocksChecked++;
    }

    double dResult = GetDifficulty() * 4294.967296 / nTargetSpacingWork;
    if (fRPCPerfTrace)
        RPCPERF_LOG("RPCPERF rpc=GetPoWMHashPS blocks_examined=%d pow_blocks_found=%d duration_us=%lld result=%.17g\n",
                    nBlocksChecked, nPoWBlocksFound,
                    (long long)(RPCPerfTimeMicros() - nStartTime), dResult);
    return dResult;
}

double GetPoSKernelPS()
{
    const bool fRPCPerfTrace = GetBoolArg("-rpcperftrace", false);
    const int64_t nStartTime = fRPCPerfTrace ? RPCPerfTimeMicros() : 0;
    int nPoSInterval = 72;
    double dStakeKernelsTriedAvg = 0;
    int nStakesHandled = 0, nStakesTime = 0;
    int nBlocksExamined = 0;

    CBlockIndex* pindex = pindexBest;;
    CBlockIndex* pindexPrevStake = NULL;

    while (pindex && nStakesHandled < nPoSInterval)
    {
        if (pindex->IsProofOfStake())
        {
            dStakeKernelsTriedAvg += GetDifficulty(pindex) * 4294967296.0;
            nStakesTime += pindexPrevStake ? (pindexPrevStake->nTime - pindex->nTime) : 0;
            pindexPrevStake = pindex;
            nStakesHandled++;
        };

        pindex = pindex->pprev;
        nBlocksExamined++;
    };

    double dResult = nStakesTime ? dStakeKernelsTriedAvg / nStakesTime : 0;
    if (fRPCPerfTrace)
        RPCPERF_LOG("RPCPERF rpc=GetPoSKernelPS blocks_examined=%d pos_blocks_found=%d duration_us=%lld result=%.17g\n",
                    nBlocksExamined, nStakesHandled,
                    (long long)(RPCPerfTimeMicros() - nStartTime), dResult);
    return dResult;
}

static bool ReadAuthoritativeBlockByHash(const uint256& hash,
                                         BlockIndexSnapshot* snapshot,
                                         CBlock* block,
                                         std::string* error)
{
    if (!snapshot || !block)
        return false;
    std::string resolveError;
    if (GetBlockIndexManager().LookupByHash(hash, snapshot, &resolveError) != BLOCK_INDEX_MANAGER_OK)
    {
        if (error) *error = resolveError.empty() ? "Block not found" : resolveError;
        return false;
    }
    if (!block->ReadFromDisk(snapshot->nFile, snapshot->nBlockPos, true))
    {
        if (error) *error = "authoritative block: block bytes unavailable";
        return false;
    }
    if (block->GetHash() != snapshot->hash)
    {
        if (error) *error = "authoritative block: disk hash mismatch";
        return false;
    }
    return true;
}

static uint256 AuthoritativeBlockTrust(const BlockIndexSnapshot& snapshot,
                                       const CBlock& block)
{
    // V2-R1 (PM1-P0-08): delegate to THE one authoritative by-value block-trust
    // rule. `block` is the authoritative block whose hash was already verified to
    // equal snapshot.hash (see the caller's disk-hash check), so block.GetHash()
    // is the same entropy/reciprocal input the previous in-place copy used.
    return GetAuthoritativeBlockTrustValue(snapshot.nBits, snapshot.height,
                                           snapshot.fProofOfStake, snapshot.hashProof,
                                           block.GetHash());
}

static Object AuthoritativeBlockToJSON(const CBlock& block,
                                       const BlockIndexSnapshot& snapshot,
                                       bool fPrintTransactionDetail)
{
    Object result;
    result.push_back(Pair("hash", snapshot.hash.GetHex()));
    result.push_back(Pair("confirmations", snapshot.fInMainChain
        ? nBestHeight - snapshot.height + 1 : -1));
    result.push_back(Pair("size", (int)::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION)));
    result.push_back(Pair("height", snapshot.height));
    result.push_back(Pair("version", block.nVersion));
    result.push_back(Pair("merkleroot", block.hashMerkleRoot.GetHex()));
    result.push_back(Pair("mint", ValueFromAmount(snapshot.nMint)));
    result.push_back(Pair("time", (int64_t)block.GetBlockTime()));
    result.push_back(Pair("nonce", (uint64_t)block.nNonce));
    result.push_back(Pair("bits", HexBits(block.nBits)));
    result.push_back(Pair("difficulty", BitsToDouble(snapshot.nBits)));
    result.push_back(Pair("blocktrust", leftTrim(AuthoritativeBlockTrust(snapshot, block).GetHex(), '0')));
    result.push_back(Pair("chaintrust", leftTrim(snapshot.nChainTrust.GetHex(), '0')));
    if (snapshot.hashPrev != uint256(0))
        result.push_back(Pair("previousblockhash", snapshot.hashPrev.GetHex()));
    BlockIndexSnapshot next;
    if (snapshot.fInMainChain && AuthoritativeGetActiveSnapshotByHeight(snapshot.height + 1, &next))
        result.push_back(Pair("nextblockhash", next.hash.GetHex()));
    result.push_back(Pair("flags", strprintf("%s%s",
        snapshot.fProofOfStake ? "proof-of-stake" : "proof-of-work",
        (snapshot.nFlags & BLOCK_STAKE_MODIFIER) ? " stake-modifier" : "")));
    result.push_back(Pair("proofhash", snapshot.hashProof.GetHex()));
    result.push_back(Pair("entropybit", (int)((snapshot.nFlags & BLOCK_STAKE_ENTROPY) >> 1)));

    Array txinfo;
    for (const CTransaction& tx : block.vtx)
    {
        if (fPrintTransactionDetail)
        {
            Object entry;
            entry.push_back(Pair("txid", tx.GetHash().GetHex()));
            TxToJSON(tx, 0, entry);
            txinfo.push_back(entry);
        }
        else
            txinfo.push_back(tx.GetHash().GetHex());
    }
    result.push_back(Pair("tx", txinfo));
    if (block.IsProofOfStake())
        result.push_back(Pair("signature", HexStr(block.vchBlockSig.begin(), block.vchBlockSig.end())));
    return result;
}

static Object AuthoritativeBlockHeaderToJSON(const CBlock& block,
                                             const BlockIndexSnapshot& snapshot)
{
    Object result;
    result.push_back(Pair("version", block.nVersion));
    if (snapshot.hashPrev != uint256(0))
        result.push_back(Pair("previousblockhash", snapshot.hashPrev.GetHex()));
    result.push_back(Pair("merkleroot", block.hashMerkleRoot.GetHex()));
    result.push_back(Pair("time", block.GetBlockTime()));
    result.push_back(Pair("bits", strprintf("%08x", block.nBits)));
    result.push_back(Pair("nonce", (uint64_t)block.nNonce));
    return result;
}

Object blockHeader2ToJSON(const CBlock& block, const CBlockIndex* blockindex)
{
    Object result;
    result.push_back(Pair("version", block.nVersion));
    if (blockindex->pprev)
        result.push_back(Pair("previousblockhash", blockindex->pprev->GetBlockHash().GetHex()));
    result.push_back(Pair("merkleroot", block.hashMerkleRoot.GetHex()));
    result.push_back(Pair("time", block.GetBlockTime()));
    result.push_back(Pair("bits", strprintf("%08x", block.nBits)));
    result.push_back(Pair("nonce", (uint64_t)block.nNonce));
    return result;
}

Object blockToJSON(const CBlock& block, const CBlockIndex* blockindex, bool fPrintTransactionDetail)
{
    Object result;
    result.push_back(Pair("hash", block.GetHash().GetHex()));
    CMerkleTx txGen(block.vtx[0]);
    txGen.SetMerkleBranch(&block);
    result.push_back(Pair("confirmations", (int)txGen.GetDepthInMainChain()));
    result.push_back(Pair("size", (int)::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION)));
    result.push_back(Pair("height", blockindex->nHeight));
    result.push_back(Pair("version", block.nVersion));
    result.push_back(Pair("merkleroot", block.hashMerkleRoot.GetHex()));
    result.push_back(Pair("mint", ValueFromAmount(blockindex->nMint)));
    result.push_back(Pair("time", (int64_t)block.GetBlockTime()));
    result.push_back(Pair("nonce", (uint64_t)block.nNonce));
    result.push_back(Pair("bits", HexBits(block.nBits)));
    result.push_back(Pair("difficulty", GetDifficulty(blockindex)));
    result.push_back(Pair("blocktrust", leftTrim(blockindex->GetBlockTrust().GetHex(), '0')));
    result.push_back(Pair("chaintrust", leftTrim(blockindex->nChainTrust.GetHex(), '0')));
    if (blockindex->pprev)
        result.push_back(Pair("previousblockhash", blockindex->pprev->GetBlockHash().GetHex()));
    if (blockindex->pnext)
        result.push_back(Pair("nextblockhash", blockindex->pnext->GetBlockHash().GetHex()));

    result.push_back(Pair("flags", strprintf("%s%s", blockindex->IsProofOfStake()? "proof-of-stake" : "proof-of-work", blockindex->GeneratedStakeModifier()? " stake-modifier": "")));
    result.push_back(Pair("proofhash", blockindex->hashProof.GetHex()));
    result.push_back(Pair("entropybit", (int)blockindex->GetStakeEntropyBit()));

    if (blockindex->nHeight >= FORK_HEIGHT_POEM)
    {
        uint256 hashProofVal = blockindex->IsProofOfStake() ? blockindex->hashProof : block.GetHash();
        result.push_back(Pair("entropy", GetBlockEntropy(hashProofVal).GetHex()));
    }

    result.push_back(Pair("modifier", strprintf("%016" PRIx64, blockindex->nStakeModifier)));
    result.push_back(Pair("modifierchecksum", strprintf("%08x", blockindex->nStakeModifierChecksum)));
    Array txinfo;
    for (const CTransaction& tx : block.vtx)
    {
        if (fPrintTransactionDetail)
        {
            Object entry;

            entry.push_back(Pair("txid", tx.GetHash().GetHex()));
            TxToJSON(tx, 0, entry);

            txinfo.push_back(entry);
        }
        else
            txinfo.push_back(tx.GetHash().GetHex());
    }

    result.push_back(Pair("tx", txinfo));

    if (block.IsProofOfStake())
        result.push_back(Pair("signature", HexStr(block.vchBlockSig.begin(), block.vchBlockSig.end())));

    return result;
}

Value dumpbootstrap(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 2)
        throw runtime_error(
            "dumpbootstrap \"destination\" \"blocks\"\n"
            "\nCreates a bootstrap format block dump of the blockchain in destination, which can be a directory or a path with filename, up to the given block number.");

    string strDest = params[0].get_str();
    int nBlocks = params[1].get_int();
    if (nBlocks < 0 || nBlocks > nBestHeight)
        throw runtime_error("Block number out of range.");

    // Sanitize destination path — confine to data directory
    for (size_t ci = 0; ci < strDest.size(); ci++)
    {
        char c = strDest[ci];
        if (c < 0x20 || c == 0x7F)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Destination path contains control characters");
    }
    boost::filesystem::path pathDest(strDest);
    if (boost::filesystem::is_directory(pathDest))
        pathDest /= "bootstrap.dat";

    try {
        FILE* file = fopen(pathDest.string().c_str(), "wb");
        if (!file)
            throw JSONRPCError(RPC_MISC_ERROR, "Error: Could not open bootstrap file for writing.");

        CAutoFile fileout = CAutoFile(file, SER_DISK, CLIENT_VERSION);
        if (!fileout)
            throw JSONRPCError(RPC_MISC_ERROR, "Error: Could not open bootstrap file for writing.");

        for (int nHeight = 0; nHeight <= nBlocks; nHeight++)
        {
            CBlock block;
            if (g_fAuthoritativeStartup)
            {
                BlockIndexSnapshot snapshot;
                std::string error;
                if (!AuthoritativeGetActiveSnapshotByHeight(nHeight, &snapshot) ||
                    !block.ReadFromDisk(snapshot.nFile, snapshot.nBlockPos, true))
                    throw JSONRPCError(RPC_MISC_ERROR, "Authoritative bootstrap block unavailable");
            }
            else
            {
                CBlockIndex* pblockindex = FindBlockByHeight(nHeight);
                if (!block.ReadFromDisk(pblockindex, true))
                    throw JSONRPCError(RPC_MISC_ERROR, "Bootstrap block unavailable");
            }
            fileout << FLATDATA(pchMessageStart) << fileout.GetSerializeSize(block) << block;
        }
    } catch(const boost::filesystem::filesystem_error &e) {
        throw JSONRPCError(RPC_MISC_ERROR, "Error: Bootstrap dump failed!");
    }

    return Value::null;
}

Value proofofdata(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1)
    throw runtime_error(
        "proofofdata\n"
        "\nArguments:\n"
        "1. \"filelocation\"          (string, required) The file location of the file to upload (e.g. /home/name/file.jpg)\n"
        "Returns the Innova address and transaction ID of the proof of data submission of the file hashed into an INN address");

    Object obj;
    std::string userFile = params[0].get_str();
    std::ifstream dataFile;

    if(userFile == "")
    {
        return 0; //return with no value prev
    }

    std::string filename = userFile.c_str();

    boost::filesystem::path p(filename);
    std::string basename = p.filename().string();

    dataFile.open(userFile.c_str(), std::ios::binary);
    std::vector<char> dataContents((std::istreambuf_iterator<char>(dataFile)), std::istreambuf_iterator<char>());

    printf("POD Upload File Start: %s\n", basename.c_str());

    //Hash the file for Innova POD
    uint256 datahash = SerializeHash(dataContents);
    CKeyID keyid(Hash160(datahash.begin(), datahash.end()));
    CBitcoinAddress baddr = CBitcoinAddress(keyid);
    std::string addr = baddr.ToString();

    CAmount nAmount = 0.001 * COIN; // 0.001 INN Fee

    // Wallet comments
    CWalletTx wtx;
    wtx.mapValue["comment"] = basename.c_str();
    std::string sNarr = "POD";
    wtx.mapValue["to"]      = "Proof of Data";

    // Comment
    // CWalletTx wtx;
    // CScript podScript = CScript() << OP_RETURN; //CScript()
    // if (!basename.c_str().empty()) {
    //     if (basename.c_str().length() > MAX_OP_RETURN_RELAY - 3) //Max 45 Bytes
    //         throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Comment cannot be longer than %u characters", MAX_OP_RETURN_RELAY - 3));
    //     podScript << ToByteVector("POD: " + basename.c_str());
    // }

    if (pwalletMain->IsLocked())
    {
        obj.push_back(Pair("error",  "Error, Your wallet is locked! Please unlock your wallet!"));
        //ui->txLineEdit->setText("ERROR: Your wallet is locked! Cannot send POD. Unlock your wallet!");
    } else if (pwalletMain->GetBalance() < 0.001) {
        obj.push_back(Pair("error",  "Error, You need at least 0.001 INN to send POD!"));
        //ui->txLineEdit->setText("ERROR: You need at least a 0.001 INN balance to send POD.");
    } else {
        //std::string sNarr;
        std::string strError = pwalletMain->SendMoneyToDestination(baddr.Get(), nAmount, sNarr, wtx);

        if(strError != "")
        {
            obj.push_back(Pair("error",  strError.c_str()));
        }

        obj.push_back(Pair("filename",           basename.c_str()));
        //obj.push_back(Pair("sizebytes",        size));
        obj.push_back(Pair("podaddress",         addr.c_str()));
        obj.push_back(Pair("podtxid",            wtx.GetHash().GetHex()));
    }

    return obj;

}

Value getbestblockhash(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getbestblockhash\n"
            "Returns the hash of the best block in the longest block chain.");

    if (g_fAuthoritativeStartup)
    {
        uint256 tip;
        std::string error;
        if (GetBlockIndexManager().BestTipHash(&tip, &error) == BLOCK_INDEX_MANAGER_OK)
            return tip.GetHex();
        throw JSONRPCError(RPC_MISC_ERROR, error.empty() ? "Best block unavailable" : error);
    }

    return hashBestChain.GetHex();
}

Value getblockcount(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getblockcount\n"
            "Returns the number of blocks in the longest block chain.");

    if (g_fAuthoritativeStartup)
    {
        int tipHeight = -1;
        std::string error;
        if (GetBlockIndexManager().ActiveTipHeight(&tipHeight, &error) == BLOCK_INDEX_MANAGER_OK)
            return tipHeight;
        throw JSONRPCError(RPC_MISC_ERROR, error.empty() ? "Block count unavailable" : error);
    }

    return nBestHeight;
}


Value getdifficulty(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getdifficulty\n"
            "Returns the difficulty as a multiple of the minimum difficulty.");

    double dPoW, dPoS;
    if (!ManagerDifficultyPair(&dPoW, &dPoS))
    {
        dPoW = GetDifficulty();
        dPoS = GetDifficulty(GetLastBlockIndex(pindexBest, true));
    }
    Object obj;
    obj.push_back(Pair("proof-of-work",        dPoW));
    obj.push_back(Pair("proof-of-stake",       dPoS));
    obj.push_back(Pair("search-interval",      (int)nLastCoinStakeSearchInterval));
    return obj;
}


Value settxfee(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 1 || AmountFromValue(params[0]) < MIN_TX_FEE)
        throw runtime_error(
            "settxfee <amount>\n"
            "<amount> is a real and is rounded to the nearest 0.01");

    nTransactionFee = AmountFromValue(params[0]);
    nTransactionFee = (nTransactionFee / CENT) * CENT;  // round to cent

    return true;
}

Value getrawmempool(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getrawmempool\n"
            "Returns all transaction ids in memory pool.");

    vector<uint256> vtxid;
    mempool.queryHashes(vtxid);

    Array a;
    for (const uint256& hash : vtxid)
        a.push_back(hash.ToString());

    return a;
}

Value getblockhash(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "getblockhash <index>\n"
            "Returns hash of block in best-block-chain at <index>.");

    int nHeight = params[0].get_int();
    if (nHeight < 0 || nHeight > nBestHeight)
        throw runtime_error("Block number out of range.");

    // Stage F: authoritative-only. LEGACY_RESIDENT is retired (init hard-fails
    // unless the authoritative V2 startup succeeded), so the historical
    // FindBlockByHeight()/CBlockIndex* fallback was unreachable production code
    // and is physically deleted. The active-chain height lookup is by-value.
    BlockIndexSnapshot snapshot;
    std::string error;
    if (GetBlockIndexManager().GetActiveByHeight(nHeight, &snapshot, &error) != BLOCK_INDEX_MANAGER_OK)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block height not found in authoritative active chain");
    return snapshot.hash.GetHex();
}

//New getblock RPC Command for Innovaium Compatibility
Value getblock(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "getblock \"blockhash\" ( verbosity ) \n"
            "\nIf verbosity is 0, returns a string that is serialized, hex-encoded data for block 'hash'.\n"
            "If verbosity is 1, returns an Object with information about block <hash>.\n"
            "If verbosity is 2, returns an Object with information about block <hash> and information about each transaction. \n"
            "\nArguments:\n"
            "1. \"blockhash\"          (string, required) The block hash\n"
            "2. verbosity              (numeric, optional, default=1) 0 for hex encoded data, 1 for a json object, and 2 for json object with transaction data\n"
            "\nResult (for verbosity = 0):\n"
            "\"data\"             (string) A string that is serialized, hex-encoded data for block 'hash'.\n"
            "\nResult (for verbosity = 1):\n"
            "{\n"
            "  \"hash\" : \"hash\",     (string) the block hash (same as provided)\n"
            "  \"confirmations\" : n,   (numeric) The number of confirmations, or -1 if the block is not on the main chain\n"
            "  \"size\" : n,            (numeric) The block size\n"
            "  \"strippedsize\" : n,    (numeric) The block size excluding witness data\n"
            "  \"weight\" : n           (numeric) The block weight as defined in BIP 141\n"
            "  \"height\" : n,          (numeric) The block height or index\n"
            "  \"version\" : n,         (numeric) The block version\n"
            "  \"versionHex\" : \"00000000\", (string) The block version formatted in hexadecimal\n"
            "  \"merkleroot\" : \"xxxx\", (string) The merkle root\n"
            "  \"tx\" : [               (array of string) The transaction ids\n"
            "     \"transactionid\"     (string) The transaction id\n"
            "     ,...\n"
            "  ],\n"
            "  \"time\" : ttt,          (numeric) The block time in seconds since epoch (Jan 1 1970 GMT)\n"
            "  \"mediantime\" : ttt,    (numeric) The median block time in seconds since epoch (Jan 1 1970 GMT)\n"
            "  \"nonce\" : n,           (numeric) The nonce\n"
            "  \"bits\" : \"1d00ffff\", (string) The bits\n"
            "  \"difficulty\" : x.xxx,  (numeric) The difficulty\n"
            "  \"previousblockhash\" : \"hash\",  (string) The hash of the previous block\n"
            "  \"nextblockhash\" : \"hash\"       (string) The hash of the next block\n"
            "}\n"
            "\nResult (for verbosity = 2):\n"
            "{\n"
            "  ...,                     Same output as verbosity = 1.\n"
            "  \"tx\" : [               (array of Objects) The transactions in the format of the getrawtransaction RPC. Different from verbosity = 1 \"tx\" result.\n"
            "         ,...\n"
            "  ],\n"
            "  ,...                     Same output as verbosity = 1.\n"
            "}\n"
            "\nExamples:\n"
        );

    LOCK(cs_main);

    std::string strHash = params[0].get_str();
    uint256 hash(strHash);
    //std::string strHash = params[0].get_str();
	//uint256 hash(uint256S(strHash));

    int verbosity = 1;
    if (params.size() > 1) {
            verbosity = params[1].get_bool() ? 1 : 0;
    }

    if (g_fAuthoritativeStartup)
    {
        BlockIndexSnapshot snapshot;
        CBlock block;
        std::string error;
        if (!ReadAuthoritativeBlockByHash(hash, &snapshot, &block, &error))
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, error.empty() ? "Block not found" : error);
        if (verbosity <= 0)
        {
            CDataStream ssBlock(SER_NETWORK, PROTOCOL_VERSION);
            ssBlock << block;
            return HexStr(ssBlock.begin(), ssBlock.end());
        }
        return AuthoritativeBlockToJSON(block, snapshot, params.size() > 1 ? params[1].get_bool() : false);
    }

    if (mapBlockIndex.count(hash) == 0)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");

    CBlock block;
    CBlockIndex* pblockindex = mapBlockIndex[hash];

	if(!block.ReadFromDisk(pblockindex, true)){
        // Block not found on disk. This could be because we have the block
        // header in our index but don't have the block (for example if a
        // non-whitelisted node sends us an unrequested long chain of valid
        // blocks, we add the headers to our index, but don't accept the
        // block).
		throw JSONRPCError(RPC_MISC_ERROR, "Block not found on disk");
	}

	block.ReadFromDisk(pblockindex, true);

    if (verbosity <= 0)
    {
        CDataStream ssBlock(SER_NETWORK, PROTOCOL_VERSION);
        ssBlock << block;
        std::string strHex = HexStr(ssBlock.begin(), ssBlock.end());
		//strHex.insert(0, "testar ");
        return strHex;
    }

    //return blockToJSON(block, pblockindex, verbosity >= 2);
	return blockToJSON(block, pblockindex, params.size() > 1 ? params[1].get_bool() : false);
}

Value getblockheader(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "getblockheader \"hash\" ( verbose )\n"
            "\nIf verbose is false, returns a string that is serialized, hex-encoded data for block 'hash' header.\n"
            "If verbose is true, returns an Object with information about block <hash> header.\n"
            "\nArguments:\n"
            "1. \"hash\"          (string, required) The block hash\n"
            "2. verbose           (boolean, optional, default=true) true for a json object, false for the hex encoded data\n"
            "\nResult (for verbose = true):\n"
            "{\n"
            "  \"version\" : n,         (numeric) The block version\n"
            "  \"previousblockhash\" : \"hash\",  (string) The hash of the previous block\n"
            "  \"merkleroot\" : \"xxxx\", (string) The merkle root\n"
            "  \"time\" : ttt,          (numeric) The block time in seconds since epoch (Jan 1 1970 GMT)\n"
            "  \"bits\" : \"1d00ffff\", (string) The bits\n"
            "  \"nonce\" : n,           (numeric) The nonce\n"
            "}\n"
            "\nResult (for verbose=false):\n"
            "\"data\"             (string) A string that is serialized, hex-encoded data for block 'hash' header.\n"
            "\nExamples:\n"
            );

    std::string strHash = params[0].get_str();
    uint256 hash(strHash);

    bool fVerbose = true;
    if (params.size() > 1)
        fVerbose = params[1].get_bool();

    if (g_fAuthoritativeStartup)
    {
        LOCK(cs_main);
        BlockIndexSnapshot snapshot;
        CBlock block;
        std::string error;
        if (!ReadAuthoritativeBlockByHash(hash, &snapshot, &block, &error))
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, error.empty() ? "Block not found" : error);
        if (!fVerbose)
        {
            CDataStream ssBlock(SER_NETWORK, PROTOCOL_VERSION);
            ssBlock << block;
            return HexStr(ssBlock.begin(), ssBlock.end());
        }
        return AuthoritativeBlockHeaderToJSON(block, snapshot);
    }

    if (mapBlockIndex.count(hash) == 0)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");

    CBlock block;
    CBlockIndex* pblockindex = mapBlockIndex[hash];

	if(!block.ReadFromDisk(pblockindex, true)){
        // Block not found on disk. This could be because we have the block
        // header in our index but don't have the block (for example if a
        // non-whitelisted node sends us an unrequested long chain of valid
        // blocks, we add the headers to our index, but don't accept the
        // block).
		throw JSONRPCError(RPC_MISC_ERROR, "Block not found on disk");
	}

	block.ReadFromDisk(pblockindex, true);

    if (!fVerbose) {
        CDataStream ssBlock(SER_NETWORK, PROTOCOL_VERSION);
        ssBlock << block;
        std::string strHex = HexStr(ssBlock.begin(), ssBlock.end());
        return strHex;
    }

    return blockHeader2ToJSON(block, pblockindex);
}

//Old getblock RPC Command, Not deprecated
Value getblock_old(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "getblock <hash> [txinfo]\n"
            "txinfo optional to print more detailed tx info\n"
            "Returns details of a block with given block-hash.");

    std::string strHash = params[0].get_str();
    uint256 hash(strHash);

    if (g_fAuthoritativeStartup)
    {
        LOCK(cs_main);
        BlockIndexSnapshot snapshot;
        CBlock block;
        std::string error;
        if (!ReadAuthoritativeBlockByHash(hash, &snapshot, &block, &error))
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, error.empty() ? "Block not found" : error);
        return AuthoritativeBlockToJSON(block, snapshot, params.size() > 1 ? params[1].get_bool() : false);
    }

    if (mapBlockIndex.count(hash) == 0)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");

    CBlock block;
    CBlockIndex* pblockindex = mapBlockIndex[hash];
    block.ReadFromDisk(pblockindex, true);

    return blockToJSON(block, pblockindex, params.size() > 1 ? params[1].get_bool() : false);
}

Value getblockbynumber(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "getblockbynumber <number> [txinfo]\n"
            "txinfo optional to print more detailed tx info\n"
            "Returns details of a block with given block-number.");

    int nHeight = params[0].get_int();
    if (nHeight < 0 || nHeight > nBestHeight)
        throw runtime_error("Block number out of range.");

    if (g_fAuthoritativeStartup)
    {
        LOCK(cs_main);
        BlockIndexSnapshot snapshot;
        CBlock block;
        std::string error;
        if (GetBlockIndexManager().GetActiveByHeight(nHeight, &snapshot, &error) != BLOCK_INDEX_MANAGER_OK ||
            !block.ReadFromDisk(snapshot.nFile, snapshot.nBlockPos, true))
            throw JSONRPCError(RPC_MISC_ERROR, "Authoritative block bytes unavailable");
        return AuthoritativeBlockToJSON(block, snapshot, params.size() > 1 ? params[1].get_bool() : false);
    }

    CBlock block;
    CBlockIndex* pblockindex = mapBlockIndex[hashBestChain];
    while (pblockindex->nHeight > nHeight)
        pblockindex = pblockindex->pprev;

    uint256 hash = *pblockindex->phashBlock;

    pblockindex = mapBlockIndex[hash];
    block.ReadFromDisk(pblockindex, true);

    return blockToJSON(block, pblockindex, params.size() > 1 ? params[1].get_bool() : false);
}

Value setbestblockbyheight(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "setbestblockbyheight <height>\n"
            "Sets the tip of the chain with a block at <height>.\n"
            "WARNING: This command is restricted and can only be used for\n"
            "minor rollbacks (max 10 blocks) in regtest mode only.\n"
            "Use 'invalidateblock' for reorg recovery in production.");

    // Regtest only
    extern bool fRegTest;
    if (!fRegTest)
        throw runtime_error(
            "setbestblockbyheight is disabled in production.\n"
            "Use 'invalidateblock' followed by 'reconsiderblock' for chain recovery.");

    int nHeight = params[0].get_int();
    if (nHeight < 0 || nHeight > nBestHeight)
        throw runtime_error("Block height out of range.");

    static const int MAX_ROLLBACK_DEPTH = 10;
    if (nBestHeight - nHeight > MAX_ROLLBACK_DEPTH)
        throw runtime_error(
            strprintf("Rollback too deep: %d blocks (max %d).\n"
                      "Use 'invalidateblock' for larger rollbacks.",
                      nBestHeight - nHeight, MAX_ROLLBACK_DEPTH));

    CBlock block;
    CBlockIndex* pblockindex = mapBlockIndex[hashBestChain];
    while (pblockindex->nHeight > nHeight)
        pblockindex = pblockindex->pprev;

    uint256 hash = *pblockindex->phashBlock;

    pblockindex = mapBlockIndex[hash];
    block.ReadFromDisk(pblockindex, true);


    Object result;

    CTxDB txdb;
    {
        LOCK(cs_main);

        printf("setbestblockbyheight: rolling back from %d to %d (regtest mode)\n",
               nBestHeight, nHeight);

        if (!block.SetBestChain(txdb, pblockindex))
            result.push_back(Pair("result", "failure"));
        else
            result.push_back(Pair("result", "success"));

    };

    return result;
}

Value invalidateblock(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "invalidateblock <hash>\n"
            "Permanently marks a block as invalid, as if it failed validation.\n"
            "Note: this applies to the block and all its descendants. The active\n"
            "chain is rolled back to the block's parent and the best eligible\n"
            "alternative chain is re-activated. The invalidation persists across\n"
            "restarts; use 'reconsiderblock' to undo it.\n"
            "This is an operator action, not a consensus failure - it never\n"
            "punishes peers. Cannot invalidate the genesis block or a block below\n"
            "the finalized height.");

    std::string strError;
    if (!InvalidateBlock(ParseHashV(params[0], "hash"), strError))
        throw JSONRPCError(RPC_INVALID_PARAMETER, strError);
    return Value::null;
}

Value reconsiderblock(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "reconsiderblock <hash>\n"
            "Removes the invalidity caused by 'invalidateblock' for the given\n"
            "block (and, transitively, for its descendants that are not otherwise\n"
            "explicitly invalidated). The best eligible chain is then re-activated,\n"
            "which may re-connect the reconsidered branch.");

    std::string strError;
    if (!ReconsiderBlock(ParseHashV(params[0], "hash"), strError))
        throw JSONRPCError(RPC_INVALID_PARAMETER, strError);
    return Value::null;
}

// ppcoin: get information of sync-checkpoint
Value getcheckpoint(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getcheckpoint\n"
            "Show info of synchronized checkpoint.\n");

    Object result;

    result.push_back(Pair("synccheckpoint", Checkpoints::hashSyncCheckpoint.ToString().c_str()));
    if (g_fAuthoritativeStartup)
    {
        BlockIndexSnapshot snapshot;
        std::string error;
        if (!ResolveAuthoritativeActiveBlock(Checkpoints::hashSyncCheckpoint, &snapshot, &error))
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Authoritative synchronized checkpoint is unknown or non-active");
        result.push_back(Pair("height", snapshot.height));
        result.push_back(Pair("timestamp", DateTimeStrFormat(snapshot.nTime).c_str()));
    }
    else
    {
        CBlockIndex* pindexCheckpoint;
        pindexCheckpoint = mapBlockIndex[Checkpoints::hashSyncCheckpoint];
        result.push_back(Pair("height", pindexCheckpoint->nHeight));
        result.push_back(Pair("timestamp", DateTimeStrFormat(pindexCheckpoint->GetBlockTime()).c_str()));
    }

    // Check that the block satisfies synchronized checkpoint
    if (CheckpointsMode == Checkpoints::STRICT)
        result.push_back(Pair("policy", "strict"));

    if (CheckpointsMode == Checkpoints::ADVISORY)
        result.push_back(Pair("policy", "advisory"));

    if (CheckpointsMode == Checkpoints::PERMISSIVE)
        result.push_back(Pair("policy", "permissive"));

    if (mapArgs.count("-checkpointkey"))
        result.push_back(Pair("checkpointmaster", true));

    return result;
}

Value gettxout(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 3)
        throw runtime_error(
            "gettxout \"txid\" n ( includemempool )\n"
            "\nReturns details about an unspent transaction output.\n"
            "\nArguments:\n"
            "1. \"txid\"       (string, required) The transaction id\n"
            "2. n              (numeric, required) vout value\n"
            "3. includemempool  (boolean, optional) Whether to included the mem pool\n"
            "\nResult:\n"
            "{\n"
            "  \"bestblock\" : \"hash\",    (string) the block hash\n"
            "  \"confirmations\" : n,       (numeric) The number of confirmations\n"
            "  \"value\" : x.xxx,           (numeric) The transaction value in btc\n"
            "  \"scriptPubKey\" : {         (json object)\n"
            "     \"asm\" : \"code\",       (string) \n"
            "     \"hex\" : \"hex\",        (string) \n"
            "     \"reqSigs\" : n,          (numeric) Number of required signatures\n"
            "     \"type\" : \"pubkeyhash\", (string) The type, eg pubkeyhash\n"
            "     \"addresses\" : [          (array of string) array of bitcoin addresses\n"
            "        \"bitcoinaddress\"     (string) bitcoin address\n"
            "        ,...\n"
            "     ]\n"
            "  },\n"
            "  \"version\" : n,            (numeric) The version\n"
            "  \"coinbase\" : true|false   (boolean) Coinbase or not\n"
            "  \"coinstake\" : true|false  (boolean) Coinstake or not\n"
            "}\n"
        );

    LOCK(cs_main);

    // R1 slice-1: resolve the active tip BY VALUE (bestblock hash + tip height)
    // from the authoritative block-index manager. On the authoritative path a
    // failure fails closed; the historical pindexBest object is never dereferenced.
    BlockIndexSnapshot tipSnapshot; std::string tipError;
    const bool fTipByValue = BestTipSnapshotByValue(&tipSnapshot, &tipError);
    if (!fTipByValue && g_fAuthoritativeStartup)
        throw JSONRPCError(RPC_MISC_ERROR, tipError.empty() ? "Best block unavailable" : tipError);

    Object ret;

    uint256 hash;
    hash.SetHex(params[0].get_str());
    int n = params[1].get_int();
    bool mem = true;
    if (params.size() == 3)
        mem = params[2].get_bool();

    CTransaction tx;
    uint256 hashBlock = 0;
    if (!GetTransaction(hash, tx, hashBlock, mem))
      return Value::null;

    if (n<0 || (unsigned int)n>=tx.vout.size() || tx.vout[n].IsNull())
      return Value::null;

    ret.push_back(Pair("bestblock", (fTipByValue ? tipSnapshot.hash : pindexBest->GetBlockHash()).GetHex()));
    if (hashBlock == 0)
      ret.push_back(Pair("confirmations", 0));
    else
    {
      map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
      if (mi != mapBlockIndex.end() && (*mi).second)
      {
        CBlockIndex* pindex = (*mi).second;
        if (pindex->IsInMainChain())
        {
          bool isSpent=false;
          CBlockIndex* p = pindex;
          p=p->pnext;
          for (; p; p = p->pnext)
          {
            CBlock block;
            CBlockIndex* pblockindex = mapBlockIndex[p->GetBlockHash()];
            block.ReadFromDisk(pblockindex, true);
            for (const CTransaction& tx : block.vtx)
            {
              for (const CTxIn& txin : tx.vin)
              {
                if( hash == txin.prevout.hash &&
                   (int64_t)txin.prevout.n )
                {
                  printf("spent at block %s\n", block.GetHash().GetHex().c_str());
                  isSpent=true; break;
                }
              }

              if(isSpent) break;
            }

            if(isSpent) break;
          }

          if(isSpent)
            return Value::null;

          ret.push_back(Pair("confirmations", (fTipByValue ? tipSnapshot.height : pindexBest->nHeight) - pindex->nHeight + 1));
        }
        else
          return Value::null;
      }
    }

    ret.push_back(Pair("value", ValueFromAmount(tx.vout[n].nValue)));
    Object o;
    spj(tx.vout[n].scriptPubKey, o, true);
    ret.push_back(Pair("scriptPubKey", o));
    ret.push_back(Pair("coinbase", tx.IsCoinBase()));
    ret.push_back(Pair("coinstake", tx.IsCoinStake()));

    return ret;
}

Value getblockchaininfo(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
                "getblockchaininfo\n"
                "Returns an object containing various state info regarding block chain processing.\n"
                "\nResult:\n"
                "{\n"
                "  \"chain\": \"xxxx\",        (string) current chain (main, testnet)\n"
                "  \"blocks\": xxxxxx,         (numeric) the current number of blocks processed in the server\n"
                "  \"bestblockhash\": \"...\", (string) the hash of the currently best block\n"
                "  \"difficulty\": xxxxxx,     (numeric) the current difficulty\n"
                "  \"initialblockdownload\": xxxx, (bool) estimate of whether this INN node is in Initial Block Download mode.\n"
                "  \"moneysupply\": xxxx, (numeric) the current supply of INN in circulation\n"
                "}\n"
        );

    proxyType proxy;
    GetProxy(NET_IPV4, proxy);

    Object obj, diff;
    std::string chain = "testnet";
    if(!fTestNet)
        chain = "main";
    obj.push_back(Pair("chain",          chain));
    obj.push_back(Pair("blocks",         (int)nBestHeight));
    obj.push_back(Pair("bestblockhash",  hashBestChain.GetHex()));

    double dPoW, dPoS;
    if (!ManagerDifficultyPair(&dPoW, &dPoS))
    {
        dPoW = GetDifficulty();
        dPoS = GetDifficulty(GetLastBlockIndex(pindexBest, true));
    }
    diff.push_back(Pair("proof-of-work",  dPoW));
    diff.push_back(Pair("proof-of-stake", dPoS));

    obj.push_back(Pair("difficulty",     diff));
    obj.push_back(Pair("initialblockdownload",  IsInitialBlockDownload()));
    // R1 slice-1: best-tip money supply read BY VALUE from the authoritative
    // block-index manager. The historical pindexBest object is NOT dereferenced
    // on the authoritative path (a failure fails closed rather than falling back).
    {
        BlockIndexSnapshot tipSnapshot; std::string tipError;
        if (BestTipSnapshotByValue(&tipSnapshot, &tipError))
            obj.push_back(Pair("moneysupply", ValueFromAmount(tipSnapshot.nMoneySupply)));
        else if (g_fAuthoritativeStartup)
            throw JSONRPCError(RPC_MISC_ERROR, tipError.empty() ? "Money supply unavailable" : tipError);
        else
            obj.push_back(Pair("moneysupply", ValueFromAmount(pindexBest->nMoneySupply)));
    }
    //obj.push_back(Pair("size_on_disk",   CalculateCurrentUsage()));
    return obj;
}

Value getspvinfo(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getspvinfo\n"
            "Returns information about SPV (light client) mode.\n");

    Object obj;
    obj.push_back(Pair("spv_enabled", fSPVMode));
    obj.push_back(Pair("spv_headers_only", fSPVHeadersOnly));
    obj.push_back(Pair("spv_start_height", nSPVStartHeight));
    obj.push_back(Pair("headers_synced", nBestHeight));

    if (fSPVMode)
    {
        obj.push_back(Pair("mode", "light"));
        obj.push_back(Pair("description", "Operating in SPV mode - headers only, no full block validation"));
    }
    else
    {
        obj.push_back(Pair("mode", "full"));
        obj.push_back(Pair("description", "Operating as full node with complete block validation"));
    }

    return obj;
}

Value spvrescan(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "spvrescan [startheight]\n"
            "Rescan blockchain for wallet transactions in SPV mode.\n"
            "Arguments:\n"
            "1. startheight  (numeric, optional) Height to start scanning from (default: 0)\n");

    if (!fSPVMode)
        throw runtime_error("spvrescan is only available in SPV mode. Start with -spv flag.");

    int nStartHeight = 0;
    if (params.size() > 0)
        nStartHeight = params[0].get_int();

    if (nStartHeight < 0)
        throw runtime_error("Invalid start height");

    CNode* pnode = NULL;
    {
        LOCK(cs_vNodes);
        for (CNode* pn : vNodes)
        {
            if (pn->fSuccessfullyConnected && !pn->fDisconnect)
            {
                pnode = pn;
                break;
            }
        }
    }

    if (!pnode)
        throw runtime_error("No connected peers available for SPV rescan");

    pwalletMain->RequestSPVTransactions(pnode, nStartHeight);

    Object obj;
    obj.push_back(Pair("status", "started"));
    obj.push_back(Pair("start_height", nStartHeight));
    obj.push_back(Pair("peer", pnode->addr.ToString()));

    return obj;
}

Value getstakemodifiercheckpoints(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 2)
        throw runtime_error(
            "getstakemodifiercheckpoints [startheight] [interval]\n"
            "Generate stake modifier checkpoints for kernel.cpp.\n"
            "Arguments:\n"
            "1. startheight  (numeric, optional) Height to start from (default: 2250000)\n"
            "2. interval     (numeric, optional) Interval between checkpoints (default: 250000)\n"
            "\nResult:\n"
            "Returns checkpoint data in C++ format ready to paste into kernel.cpp\n");

    int nStartHeight = 2250000;  
    int nInterval = 250000;      

    if (params.size() > 0)
        nStartHeight = params[0].get_int();
    if (params.size() > 1)
        nInterval = params[1].get_int();

    if (nStartHeight < 0 || nInterval < 1000)
        throw runtime_error("Invalid parameters: startheight must be >= 0, interval must be >= 1000");

    LOCK(cs_main);

    // R1 slice-1: the best-tip height is read BY VALUE from the authoritative
    // block-index manager; the historical pindexBest object is not dereferenced
    // on the authoritative path (a failure fails closed rather than falling back).
    int nBestTipHeight = -1;
    {
        BlockIndexSnapshot tipSnapshot; std::string tipError;
        if (BestTipSnapshotByValue(&tipSnapshot, &tipError))
            nBestTipHeight = tipSnapshot.height;
        else if (g_fAuthoritativeStartup)
            throw JSONRPCError(RPC_MISC_ERROR, tipError.empty() ? "Block index not available" : tipError);
        else
        {
            if (!pindexBest)
                throw runtime_error("Block index not available");
            nBestTipHeight = pindexBest->nHeight;
        }
    }

    Object result;
    Array checkpoints;
    std::string cppOutput = "// Stake modifier checkpoints - generated by getstakemodifiercheckpoints\n";

    int nCurrentHeight = nStartHeight;
    int nBestHeight = nBestTipHeight;

    while (nCurrentHeight <= nBestHeight)
    {
        CBlockIndex* pindex = FindBlockByHeight(nCurrentHeight);
        if (!pindex)
        {
            nCurrentHeight += nInterval;
            continue;
        }

        unsigned int nChecksum = pindex->nStakeModifierChecksum;

        Object checkpoint;
        checkpoint.push_back(Pair("height", nCurrentHeight));
        checkpoint.push_back(Pair("checksum", strprintf("0x%08x", nChecksum)));
        checkpoints.push_back(checkpoint);

        cppOutput += strprintf("        ( %d, 0x%08x )\n", nCurrentHeight, nChecksum);

        nCurrentHeight += nInterval;
    }

    result.push_back(Pair("start_height", nStartHeight));
    result.push_back(Pair("end_height", nBestHeight));
    result.push_back(Pair("interval", nInterval));
    result.push_back(Pair("count", (int)checkpoints.size()));
    result.push_back(Pair("checkpoints", checkpoints));
    result.push_back(Pair("cpp_output", cppOutput));

    return result;
}

Value downloadbootstrap(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 2)
        throw runtime_error(
            "downloadbootstrap [url] [force]\n"
            "Download and apply blockchain bootstrap.\n"
            "This downloads the bootstrap from the latest GitHub release.\n"
            "Requires restart after completion.\n"
            "\nWARNING: This will overwrite existing blockchain data!\n"
            "\nArguments:\n"
            "1. url    (string, optional) Custom bootstrap URL. Default: latest GitHub release\n"
            "2. force  (bool, optional) Force download even if blockchain data exists. Default: false\n"
            "\nResult:\n"
            "{\n"
            "  \"status\": \"success|failed\",\n"
            "  \"message\": \"description\"\n"
            "}\n");

    std::string url = params.size() > 0 ? params[0].get_str() : "";
    bool force = params.size() > 1 ? params[1].get_bool() : false;

    if (!Bootstrap::IsNeeded(GetDataDir()) && !force) {
        throw runtime_error(
            "Blockchain data already exists. This command would overwrite existing data.\n"
            "If you really want to do this, call with force=true:\n"
            "  downloadbootstrap \"\" true\n"
            "WARNING: Your existing blockchain data will be overwritten!");
    }

    if (!url.empty()) {
        printf("Bootstrap: WARNING - Using custom URL: %s\n", url.c_str());
        printf("Bootstrap: Only use URLs from trusted sources!\n");
    }

    printf("Bootstrap: Starting download via RPC...\n");

    int64_t lastPercent = -1;
    auto progressCallback = [&lastPercent](int64_t downloaded, int64_t total) {
        if (total > 0) {
            int64_t percent = static_cast<int64_t>((static_cast<double>(downloaded) / total) * 100.0);
            if (percent > 100) percent = 100;
            if (percent < 0) percent = 0;
            if (percent != lastPercent && percent % 10 == 0) {
                printf("Bootstrap download: %lld%% (%lld MB / %lld MB)\n",
                       (long long)percent,
                       (long long)(downloaded / 1048576),
                       (long long)(total / 1048576));
                lastPercent = percent;
            }
        }
    };

    bool success = Bootstrap::DownloadAndApply(url, GetDataDir(), progressCallback);

    Object result;
    if (success) {
        result.push_back(Pair("status", "success"));
        result.push_back(Pair("message", "Bootstrap applied successfully. Please restart Innova to load the new blockchain data."));
    } else {
        result.push_back(Pair("status", "failed"));
        result.push_back(Pair("message", "Bootstrap download or extraction failed. Check debug.log for details."));
    }

    return result;
}

// Diagnostic-only CN guard outcome telemetry. Exposes the guard/validation
// counters added for the AUD-002/AUD-003 discriminator. No consensus change.
Value getcnvalidationstats(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getcnvalidationstats\n"
            "\nReturns diagnostic counters for the collateralnode payee-enforcement guard "
            "and CN block validation (read-only; no consensus behavior change).\n");

    size_t nLocalCN = 0;
    unsigned int nMedian = 0;
    {
        LOCK(cs_collateralnodes);
        nLocalCN = vecCollateralnodes.size();
        nMedian = mnCount;
    }
    Object obj;
    obj.push_back(Pair("guard_passed", GetCNValidationGuardPassedCount()));
    obj.push_back(Pair("deferred", GetCNPaymentsDeferredCount()));
    obj.push_back(Pair("guard_passed_minus_deferred",
                       GetCNValidationGuardPassedCount() - GetCNPaymentsDeferredCount()));
    obj.push_back(Pair("payee_found", GetCNValidationPayeeFoundCount()));
    obj.push_back(Pair("payee_missing", GetCNValidationPayeeMissingCount()));
    obj.push_back(Pair("findcnpayment_entered", GetCNValidationFindCNPaymentEnteredCount()));
    obj.push_back(Pair("last_guard_reason", GetCNValidationLastGuardReason()));
    obj.push_back(Pair("last_guard_height", GetCNValidationLastGuardHeight()));
    obj.push_back(Pair("local_cn_count", (int64_t)nLocalCN));
    obj.push_back(Pair("network_median_mnCount", (int64_t)nMedian));
    return obj;
}

// Diagnostic-only: cs_db acquisition-wait telemetry for the wallet-flush
// contention discriminator. Exposes how often a DB open waits on bitdb.cs_db
// (whether held by the wallet-flush txn_checkpoint). No consensus/lock change.
Value getdbwaitstats(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getdbwaitstats\n"
            "\nReturns diagnostic counters for cs_db acquisition waits (wallet-flush "
            "contention discriminator; read-only).\n");
    Object obj;
    obj.push_back(Pair("db_lock_wait_count", GetDbLockWaitCount()));
    obj.push_back(Pair("db_lock_wait_us_total", GetDbLockWaitUsTotal()));
    obj.push_back(Pair("db_lock_wait_us_max", GetDbLockWaitUsMax()));
    obj.push_back(Pair("db_lock_wait_over_100ms", GetDbLockWaitOver100ms()));
    obj.push_back(Pair("db_lock_wait_over_500ms", GetDbLockWaitOver500ms()));
    obj.push_back(Pair("db_lock_wait_over_100ms_while_flushing", GetDbLockWaitOver100msWhileFlushing()));
    obj.push_back(Pair("flush_in_progress", (int)(dbFlushInProgress.load(std::memory_order_relaxed))));
    return obj;
}

// ============================================================================
// RECONSTRUCTED diagnostic RPCs (self-contained CN / memory). Strictly read-only.
// ============================================================================

static int GetDepthInMainChainReadOnly(const CWalletTx& tx)
{
    AssertLockHeld(cs_main);
    if (tx.hashBlock == 0 || tx.nIndex == -1)
        return mempool.exists(tx.GetHash()) ? 0 : -1;

    map<uint256, CBlockIndex*>::const_iterator it = mapBlockIndex.find(tx.hashBlock);
    if (it == mapBlockIndex.end() || !it->second || !it->second->IsInMainChain())
        return mempool.exists(tx.GetHash()) ? 0 : -1;
    if (!tx.fMerkleVerified &&
        CBlock::CheckMerkleBranch(tx.GetHash(), tx.vMerkleBranch, tx.nIndex) !=
            it->second->hashMerkleRoot)
        return mempool.exists(tx.GetHash()) ? 0 : -1;
    if (!pindexBest)
        return 0;
    return pindexBest->nHeight - it->second->nHeight + 1;
}

static int GetBlocksToMaturityReadOnly(const CWalletTx& tx, int depth)
{
    if (!(tx.IsCoinBase() || tx.IsCoinStake()))
        return 0;
    int walletMaturity = fRegTest ? nCoinbaseMaturity : nCoinbaseMaturity + 10;
    return std::max(0, walletMaturity - depth);
}

static std::string IsMineCategory(isminetype mine)
{
    if ((mine & MINE_SPENDABLE) && (mine & MINE_WATCH_ONLY)) return "spendable+watch_only";
    if (mine & MINE_SPENDABLE) return "spendable";
    if (mine & MINE_WATCH_ONLY) return "watch_only";
    return "no";
}

Value getcollateraloutpointdiagnostics(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 2)
        throw runtime_error(
            "getcollateraloutpointdiagnostics <txid> <vout>\n"
            "\nReturns strictly read-only wallet and collateralnode selector diagnostics for one outpoint.\n");
    if (params[0].type() != str_type || params[0].get_str().size() != 64 ||
        !IsHex(params[0].get_str()))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "txid must be exactly 64 hexadecimal characters");
    if (params[1].type() != int_type)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "vout must be an integer");
    int64_t voutValue = params[1].get_int64();
    if (voutValue < 0 || voutValue > (int64_t)std::numeric_limits<unsigned int>::max())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "vout is out of range");

    uint256 txid;
    txid.SetHex(params[0].get_str());
    unsigned int vout = (unsigned int)voutValue;
    Object result;
    result.push_back(Pair("txid", txid.GetHex()));
    result.push_back(Pair("vout", (int64_t)vout));

    if (!pwalletMain)
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is not initialized");
    LOCK2(cs_main, pwalletMain->cs_wallet);
    map<uint256, CWalletTx>::const_iterator walletIt = pwalletMain->mapWallet.find(txid);
    bool txExists = walletIt != pwalletMain->mapWallet.end();
    bool outputExists = txExists && vout < walletIt->second.vout.size();
    result.push_back(Pair("tx_exists", txExists));
    result.push_back(Pair("output_exists", outputExists));
    if (!outputExists)
    {
        string reason = txExists ? "vout does not exist" : "wallet transaction not found";
        result.push_back(Pair("amount", Value()));
        result.push_back(Pair("scriptPubKey", Value()));
        result.push_back(Pair("is_mine", "unavailable"));
        result.push_back(Pair("is_spent", false));
        result.push_back(Pair("is_locked_coin", false));
        result.push_back(Pair("depth", -1));
        result.push_back(Pair("trusted", false));
        result.push_back(Pair("trusted_reason", reason));
        result.push_back(Pair("spendable", false));
        result.push_back(Pair("in_available_coins", false));
        result.push_back(Pair("in_available_coins_mn", false));
        result.push_back(Pair("in_select_coins_collateralnode", false));
        result.push_back(Pair("in_select_coins_collateralnode_for_pubkey", false));
        result.push_back(Pair("get_vin_from_output_success", false));
        result.push_back(Pair("get_key_exists", false));
        result.push_back(Pair("available_coins_rejection", reason));
        result.push_back(Pair("available_coins_mn_rejection", reason));
        result.push_back(Pair("select_coins_collateralnode_rejection", reason));
        result.push_back(Pair("select_coins_collateralnode_for_pubkey_rejection", reason));
        result.push_back(Pair("get_vin_from_output_rejection", reason));
        result.push_back(Pair("get_key_rejection", reason));
        result.push_back(Pair("rejection_stage", txExists ? "output_lookup" : "wallet_lookup"));
        result.push_back(Pair("rejection_reason", reason));
        return result;
    }

    const CWalletTx& tx = walletIt->second;
    const CTxOut& txout = tx.vout[vout];
    isminetype mine = pwalletMain->IsMine(txout);
    bool spent = tx.IsSpent(vout);
    bool locked = pwalletMain->IsLockedCoin(txid, vout);
    int depth = GetDepthInMainChainReadOnly(tx);
    bool trusted = tx.IsFinal() && depth >= 1;
    string trustedReason = (depth != 0) ? (trusted ? "confirmed in the main chain" :
        "transaction is not final or is not in the main chain") :
        "unknown at zero depth because exact trust evaluation would mutate wallet caches";
    bool spendable = (mine & ISMINE_SPENDABLE) != ISMINE_NO;

    CTxDestination destination;
    bool destinationExists = ExtractDestination(txout.scriptPubKey, destination);
    string address = destinationExists ? CBitcoinAddress(destination).ToString() : "";
    Object script;
    script.push_back(Pair("asm", txout.scriptPubKey.ToString()));
    script.push_back(Pair("hex", HexStr(txout.scriptPubKey.begin(), txout.scriptPubKey.end())));
    script.push_back(Pair("address", address));

    // ---- Real AvailableCoinsMN(fOnlyConfirmed=true) predicate observations (mutation-free) ----
    bool txIsFinal = tx.IsFinal();
    bool txIsCoinBase = tx.IsCoinBase();
    bool txIsCoinStake = tx.IsCoinStake();
    int  depthReal = depth;
    int  maturityReal = GetBlocksToMaturityReadOnly(tx, depthReal);
    bool nTimeLeBest = false;
    bool hashBlockPresent = false;
    {
        LOCK(cs_main);
        nTimeLeBest = pindexBest ? (tx.nTime <= pindexBest->GetBlockTime()) : false;
        hashBlockPresent = mapBlockIndex.count(tx.hashBlock) > 0;
    }
    bool trustedReal = txIsFinal && nTimeLeBest && hashBlockPresent && depthReal >= 1;

    bool mnWholeTxOk = txIsFinal && trustedReal && maturityReal == 0 && depthReal > 0;
    bool voutOk = !spent && txout.nValue > 0;
    bool inAvailableRealMN = mnWholeTxOk && voutOk;
    bool inSelectRealCN   = inAvailableRealMN && txout.nValue == GetMNCollateral() * COIN;

    string realMNRejection;
    if (!inAvailableRealMN) {
        if (!txIsFinal) realMNRejection = "transaction is not final";
        else if (!trustedReal)
            realMNRejection = "transaction is not trusted [" +
                std::string(!nTimeLeBest ? "nTime>bestBlockTime; " : "") +
                std::string(!hashBlockPresent ? "hashBlock-not-in-index; " : "") +
                std::string(depthReal < 1 ? "depth<1; " : "") + "]";
        else if (maturityReal > 0) realMNRejection = "transaction is immature";
        else if (depthReal <= 0)   realMNRejection = "transaction has no positive main-chain depth";
        else if (spent)            realMNRejection = "output is spent";
        else if (txout.nValue <= 0) realMNRejection = "output amount is not positive";
        else realMNRejection = "excluded by AvailableCoinsMN for an unmodeled reason";
    }
    string realCNRejection = inSelectRealCN ? "" :
        (realMNRejection.empty() ? "output amount is not the collateralnode collateral amount" : realMNRejection);

    bool acWholeTxOk = txIsFinal && maturityReal == 0 && depthReal >= 0;
    bool inAvailableRealAC = acWholeTxOk && mine != MINE_NO &&
                             !spent && !locked && txout.nValue >= nMinimumInputValue;
    string realACRejection;
    if (!inAvailableRealAC) {
        if (!txIsFinal) realACRejection = "transaction is not final";
        else if (maturityReal > 0) realACRejection = "transaction is immature";
        else if (depthReal < 0) realACRejection = "transaction conflicts with the main chain";
        else if (mine == MINE_NO) realACRejection = "output is not owned by this wallet";
        else if (spent) realACRejection = "output is spent";
        else if (locked) realACRejection = "output is locked";
        else if (txout.nValue < nMinimumInputValue) realACRejection = "output is below mininput";
        else realACRejection = "excluded by AvailableCoins for an unmodeled reason";
    }

    CScript selectedAddressScript;
    if (destinationExists)
        selectedAddressScript.SetDestination(CBitcoinAddress(address).Get());
    bool inSelectedForPubKeyReal = inAvailableRealAC && destinationExists &&
        txout.scriptPubKey == selectedAddressScript &&
        txout.nValue == GetMNCollateral() * COIN;
    string selectedForPubKeyReason = inSelectedForPubKeyReal ? "" : realACRejection;
    if (!inSelectedForPubKeyReal && selectedForPubKeyReason.empty() && !destinationExists)
        selectedForPubKeyReason = "script has no extractable destination";
    if (!inSelectedForPubKeyReal && selectedForPubKeyReason.empty() &&
        txout.scriptPubKey != selectedAddressScript)
        selectedForPubKeyReason = "output script does not match the collateral address";
    if (!inSelectedForPubKeyReal && selectedForPubKeyReason.empty() &&
        txout.nValue != GetMNCollateral() * COIN)
        selectedForPubKeyReason = "output amount is not the collateralnode collateral amount";
    if (!inSelectedForPubKeyReal && selectedForPubKeyReason.empty())
        selectedForPubKeyReason = "excluded by SelectCoinsCollateralnodeForPubKey for an unmodeled reason";

    CKeyID keyID;
    bool keyIDExists = destinationExists && CBitcoinAddress(destination).GetKeyID(keyID);
    CKey key;
    bool keyExists = keyIDExists && pwalletMain->GetKey(keyID, key);
    CTxIn vin;
    CPubKey pubkey;
    CKey vinKey;
    bool getVinSuccess = activeCollateralnode.GetVinFromOutput(
        COutput(&tx, vout, depth, spendable), vin, pubkey, vinKey);
    string keyReason = keyExists ? "" :
        (!destinationExists ? "script has no extractable destination" :
         (!keyIDExists ? "destination does not refer to a key" : "private key is not available"));
    string vinReason = getVinSuccess ? "" : keyReason;

    result.push_back(Pair("amount", ValueFromAmount(txout.nValue)));
    result.push_back(Pair("scriptPubKey", script));
    result.push_back(Pair("is_mine", IsMineCategory(mine)));
    result.push_back(Pair("is_spent", spent));
    result.push_back(Pair("is_locked_coin", locked));
    result.push_back(Pair("depth", depth));
    result.push_back(Pair("trusted", (depth != 0) ? Value(trusted) : Value()));
    result.push_back(Pair("trusted_reason", trustedReason));
    result.push_back(Pair("spendable", spendable));
    result.push_back(Pair("in_available_coins", inAvailableRealAC));
    result.push_back(Pair("in_available_coins_mn", inAvailableRealMN));
    result.push_back(Pair("in_select_coins_collateralnode", inSelectRealCN));
    result.push_back(Pair("in_select_coins_collateralnode_for_pubkey", inSelectedForPubKeyReal));
    result.push_back(Pair("get_vin_from_output_success", getVinSuccess));
    result.push_back(Pair("get_key_exists", keyExists));
    result.push_back(Pair("tx_final", txIsFinal));
    result.push_back(Pair("tx_coinbase", txIsCoinBase));
    result.push_back(Pair("tx_coinstake", txIsCoinStake));
    result.push_back(Pair("tx_depth", depthReal));
    result.push_back(Pair("tx_blocks_to_maturity", maturityReal));
    result.push_back(Pair("tx_ntime", (int64_t)tx.nTime));
    result.push_back(Pair("best_block_time", pindexBest ? (int64_t)pindexBest->GetBlockTime() : (int64_t)-1));
    result.push_back(Pair("tx_ntime_le_besttime", nTimeLeBest));
    result.push_back(Pair("tx_hashblock_present", hashBlockPresent));
    result.push_back(Pair("tx_trusted_real_semantics", trustedReal));
    result.push_back(Pair("available_coins_mn_fonly_confirmed", true));
    result.push_back(Pair("available_coins_mn_fonly_unlocked", false));
    result.push_back(Pair("available_coins_mn_coin_type", "ALL_COINS"));
    result.push_back(Pair("available_coins_rejection", realACRejection));
    result.push_back(Pair("available_coins_mn_rejection", realMNRejection));
    result.push_back(Pair("select_coins_collateralnode_rejection", realCNRejection));
    result.push_back(Pair("select_coins_collateralnode_for_pubkey_rejection", selectedForPubKeyReason));
    result.push_back(Pair("get_vin_from_output_rejection", vinReason));
    result.push_back(Pair("get_key_rejection", keyReason));
    result.push_back(Pair("rejection_stage", realCNRejection.empty() ? "" : "SelectCoinsCollateralnode"));
    result.push_back(Pair("rejection_reason", realCNRejection));
    return result;
}

static inline size_t MemoryDiagMallocChunkRound(size_t n)
{
    const size_t align = 16;
    return (n + align - 1) / align * align;
}

#if defined(__GLIBCXX__) && defined(__GNUC__)
typedef std::_Rb_tree_node<std::pair<const uint256, CBlockIndex*> > CBlockIndexMapRbTreeNode;
typedef std::_Rb_tree_node<std::pair<const COutPoint, unsigned int> > StakeSeenSetRbTreeNode;
#endif

Value getmemorydiagnostics(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getmemorydiagnostics\n"
            "\nReturns strictly read-only, cheap memory-usage diagnostics for the main\n"
            "full-chain globals (mapBlockIndex, stake-seen set, collateralnode list, wallet).\n"
            "Every value is a compile-time sizeof() constant, a container size() count, or\n"
            "simple arithmetic on those; container contents are never traversed and no\n"
            "wallet/collateralnode state is mutated.\n");

    const size_t sizeofCBlockIndex = sizeof(CBlockIndex);
    const size_t sizeofMapValueType = sizeof(std::pair<const uint256, CBlockIndex*>);
#if defined(__GLIBCXX__) && defined(__GNUC__)
    const size_t sizeofMapNode = sizeof(CBlockIndexMapRbTreeNode);
#else
    const size_t sizeofMapNode = sizeof(std::pair<const uint256, CBlockIndex*>)
                                 + sizeof(void*) * 3 + 1;
#endif
    const size_t sizeofMapNodeAlloc = MemoryDiagMallocChunkRound(sizeofMapNode);
    const size_t sizeofCBlockIndexAlloc = MemoryDiagMallocChunkRound(sizeofCBlockIndex);
#if defined(__GLIBCXX__) && defined(__GNUC__)
    const size_t sizeofStakeSeenNode = sizeof(StakeSeenSetRbTreeNode);
#else
    const size_t sizeofStakeSeenNode = sizeof(std::pair<const COutPoint, unsigned int>)
        + sizeof(void*) * 3 + 1;
#endif

    size_t count = 0;
    size_t setStakeSeenSize = 0;
    size_t setStakeSeenOrphanSize = 0;
    size_t setInvalidSize = 0;
    size_t setRegSize = 0;
    {
        LOCK(cs_main);
        count = mapBlockIndex.size();
        setStakeSeenSize = setStakeSeen.size();
        setStakeSeenOrphanSize = setStakeSeenOrphan.size();
        setInvalidSize = setInvalidBlockHash.size();
        setRegSize = setpwalletRegistered.size();
    }
    size_t mempoolSize = 0;
    {
        LOCK(cs_main);
        mempoolSize = mempool.size();
    }

    size_t vecCNCount = 0;
    size_t vecCNRanksCount = 0;
    size_t mapSeenCNVotesCount = 0;
    size_t mapCacheBlockHashesCount = 0;
    {
        LOCK(cs_collateralnodes);
        vecCNCount = vecCollateralnodes.size();
        vecCNRanksCount = vecCollateralnodeRanks.size();
        mapSeenCNVotesCount = mapSeenCollateralnodeVotes.size();
        mapCacheBlockHashesCount = mapCacheBlockHashes.size();
    }

    Object result;
    result.push_back(Pair("mapBlockIndex_count", (int64_t)count));
    result.push_back(Pair("sizeof_CBlockIndex", (int64_t)sizeofCBlockIndex));
    result.push_back(Pair("sizeof_map_value_type", (int64_t)sizeofMapValueType));
    result.push_back(Pair("sizeof_map_node", (int64_t)sizeofMapNode));
    result.push_back(Pair("map_node_allocated_bytes", (int64_t)sizeofMapNodeAlloc));
    result.push_back(Pair("sizeof_CBlockIndex_allocated_bytes", (int64_t)sizeofCBlockIndexAlloc));
    result.push_back(Pair("estimated_payload_bytes", (int64_t)(count * sizeofCBlockIndex)));
    result.push_back(Pair("estimated_container_bytes", (int64_t)(count * sizeofMapNodeAlloc)));
    result.push_back(Pair("estimated_allocated_bytes",
        (int64_t)(count * sizeofCBlockIndexAlloc + count * sizeofMapNodeAlloc)));

    result.push_back(Pair("setStakeSeen_count", (int64_t)setStakeSeenSize));
    result.push_back(Pair("setStakeSeen_estimated_bytes",
        (int64_t)(setStakeSeenSize * sizeofStakeSeenNode)));
    result.push_back(Pair("setStakeSeenOrphan_count", (int64_t)setStakeSeenOrphanSize));
    result.push_back(Pair("setInvalidBlockHash_count", (int64_t)setInvalidSize));
    result.push_back(Pair("setpwalletRegistered_count", (int64_t)setRegSize));
    result.push_back(Pair("vecCollateralnodes_count", (int64_t)vecCNCount));
    result.push_back(Pair("vecCollateralnodes_estimated_bytes",
        (int64_t)(vecCNCount * sizeof(CCollateralNode))));
    result.push_back(Pair("vecCollateralnodeRanks_count", (int64_t)vecCNRanksCount));
    result.push_back(Pair("mapSeenCollateralnodeVotes_count", (int64_t)mapSeenCNVotesCount));
    result.push_back(Pair("mapCacheBlockHashes_count", (int64_t)mapCacheBlockHashesCount));
    result.push_back(Pair("mempool_count", (int64_t)mempoolSize));

    if (pwalletMain)
    {
        LOCK(pwalletMain->cs_wallet);
        result.push_back(Pair("mapWallet_count", (int64_t)pwalletMain->mapWallet.size()));
        result.push_back(Pair("mapWallet_estimated_bytes",
            (int64_t)(pwalletMain->mapWallet.size() * sizeof(std::pair<const uint256, CWalletTx>))));
        result.push_back(Pair("setLockedCoins_count", (int64_t)pwalletMain->setLockedCoins.size()));
    }
    return result;
}