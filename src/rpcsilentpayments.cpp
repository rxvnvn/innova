// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Silent Payment RPC surface (sp_getnewaddress / sp_listaddresses / sp_send).
// Relocated from rpcshielded.cpp when the experimental-v5 shielded family was
// retired (M5D); silent payments are an independent retained feature.

#include "main.h"
#include "innovarpc.h"
#include "txdb-leveldb.h"
#include "wallet.h"
#include "walletdb.h"
#include "silentpayments.h"
#include "init.h"
#include "base58.h"

#include <string>
#include <sstream>
#include <algorithm>
#include <openssl/rand.h>

using namespace json_spirit;
using namespace std;

Value sp_getnewaddress(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 0)
        throw runtime_error(
            "sp_getnewaddress\n"
            "Returns a new silent payment address.\n");

    EnsureWalletIsUnlocked();

    CSilentPaymentAddress addr;
    if (!pwalletMain->GenerateNewSilentPaymentKey(addr))
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Failed to generate silent payment key or derive address");

    return addr.ToString();
}

Value sp_listaddresses(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 0)
        throw runtime_error(
            "sp_listaddresses\n"
            "Returns all silent payment addresses.\n");

    Array ret;
    LOCK(pwalletMain->cs_silentpayments);
    for (const CSilentPaymentKey& key : pwalletMain->vSilentPaymentKeys)
    {
        CSilentPaymentAddress addr;
        if (key.GetAddress(addr))
        {
            Object entry;
            entry.push_back(Pair("address", addr.ToString()));
            ret.push_back(entry);
        }
    }
    return ret;
}

Value sp_send(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 2)
        throw runtime_error(
            "sp_send <silent_payment_address> <amount>\n"
            "Send coins to a silent payment address.\n"
            "\nArguments:\n"
            "1. silent_payment_address  (string, required) Recipient's silent payment address\n"
            "2. amount                  (numeric, required) Amount in INN to send\n"
            "\nResult:\n"
            "{\n"
            "  \"txid\": \"...\",            (string) Transaction ID\n"
            "  \"silent_address\": \"...\",  (string) Recipient address used\n"
            "  \"amount\": n,              (numeric) Amount sent\n"
            "  \"fee\": n,                 (numeric) Fee paid\n"
            "  \"output_pubkey\": \"...\"   (string) One-time output public key (hex)\n"
            "}\n"
        );

    EnsureWalletIsUnlocked();

    CSilentPaymentAddress spAddr;
    if (!spAddr.FromString(params[0].get_str()))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid silent payment address");

    int64_t nAmount = AmountFromValue(params[1]);

    CWalletTx wtxNew;
    wtxNew.BindWallet(pwalletMain);
    CReserveKey reservekey(pwalletMain);

    {
        LOCK2(cs_main, pwalletMain->cs_wallet);
        CTxDB txdb("r");

        int64_t nFeeRet = nTransactionFee;
        static const int MAX_FEE_RETRIES = 20;
        for (int nFeeRetry = 0; nFeeRetry < MAX_FEE_RETRIES; nFeeRetry++)
        {
            wtxNew.vin.clear();
            wtxNew.vout.clear();
            wtxNew.fFromMe = true;

            int64_t nTotalNeeded = nAmount + nFeeRet;

            set<pair<const CWalletTx*, unsigned int> > setCoins;
            int64_t nValueIn = 0;

            if (!pwalletMain->SelectCoins2(nTotalNeeded, wtxNew.nTime, setCoins, nValueIn))
                throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds");

            vector<vector<unsigned char> > vInputPrivKeys;
            for (const auto& coin : setCoins)
            {
                const CScript& prevScript = coin.first->vout[coin.second].scriptPubKey;
                CTxDestination dest;
                if (!ExtractDestination(prevScript, dest))
                    throw JSONRPCError(RPC_WALLET_ERROR, "Cannot extract destination from input");
                const CKeyID* pKeyID = boost::get<CKeyID>(&dest);
                if (!pKeyID)
                    throw JSONRPCError(RPC_WALLET_ERROR, "Input is not a standard key destination");
                CKey key;
                if (!pwalletMain->GetKey(*pKeyID, key))
                    throw JSONRPCError(RPC_WALLET_ERROR, "Cannot get private key for input");
                if (!key.IsValid() || key.size() != 32)
                    throw JSONRPCError(RPC_WALLET_ERROR, "Private key invalid or wrong size");
                vInputPrivKeys.push_back(vector<unsigned char>(key.begin(), key.end()));
            }

            vector<unsigned char> vchSenderSecretSum;
            if (!ComputeInputPrivKeySum(vInputPrivKeys, vchSenderSecretSum))
            {
                for (auto& k : vInputPrivKeys) OPENSSL_cleanse(k.data(), k.size());
                throw JSONRPCError(RPC_INTERNAL_ERROR, "Failed to compute input key sum");
            }
            for (auto& k : vInputPrivKeys) OPENSSL_cleanse(k.data(), k.size());

            vector<unsigned char> vchOutputPubKey;
            if (!DeriveSilentPaymentOutput(vchSenderSecretSum, spAddr, 0, vchOutputPubKey))
            {
                OPENSSL_cleanse(vchSenderSecretSum.data(), vchSenderSecretSum.size());
                throw JSONRPCError(RPC_INTERNAL_ERROR, "Failed to derive silent payment output");
            }
            OPENSSL_cleanse(vchSenderSecretSum.data(), vchSenderSecretSum.size());

            CScript scriptPayee;
            scriptPayee << vchOutputPubKey << OP_CHECKSIG;

            wtxNew.vout.push_back(CTxOut(nAmount, scriptPayee));

            int64_t nChange = nValueIn - nAmount - nFeeRet;
            if (nFeeRet < MIN_TX_FEE && nChange > 0 && nChange < CENT)
            {
                int64_t nMoveToFee = min(nChange, MIN_TX_FEE - nFeeRet);
                nChange -= nMoveToFee;
                nFeeRet += nMoveToFee;
            }

            if (nChange > 0)
            {
                CPubKey vchPubKey;
                if (!reservekey.GetReservedKey(vchPubKey))
                    throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");
                CScript scriptChange;
                scriptChange.SetDestination(vchPubKey.GetID());
                wtxNew.vout.push_back(CTxOut(nChange, scriptChange));
            }
            else
            {
                reservekey.ReturnKey();
            }

            for (const auto& coin : setCoins)
                wtxNew.vin.push_back(CTxIn(coin.first->GetHash(), coin.second));

            int nIn = 0;
            for (const auto& coin : setCoins)
            {
                if (!SignSignature(*pwalletMain, *coin.first, wtxNew, nIn++))
                    throw JSONRPCError(RPC_WALLET_ERROR, "Failed to sign transaction");
            }

            unsigned int nBytes = ::GetSerializeSize(*(CTransaction*)&wtxNew, SER_NETWORK, PROTOCOL_VERSION);
            if (nBytes >= MAX_BLOCK_SIZE_GEN / 5)
                throw JSONRPCError(RPC_WALLET_ERROR, "Transaction too large");

            int64_t nPayFee = nTransactionFee * (1 + (int64_t)nBytes / 1000);
            int64_t nMinFee = wtxNew.GetMinFee(1, GMF_SEND, nBytes);

            if (nFeeRet < max(nPayFee, nMinFee))
            {
                nFeeRet = max(nPayFee, nMinFee);
                continue;
            }

            wtxNew.AddSupportingTransactions(txdb);
            wtxNew.fTimeReceivedIsTxTime = true;

            if (!pwalletMain->CommitTransaction(wtxNew, reservekey))
                throw JSONRPCError(RPC_WALLET_ERROR, "Failed to commit transaction");

            Object result;
            result.push_back(Pair("txid", wtxNew.GetHash().GetHex()));
            result.push_back(Pair("silent_address", params[0].get_str()));
            result.push_back(Pair("amount", ValueFromAmount(nAmount)));
            result.push_back(Pair("fee", ValueFromAmount(nFeeRet)));
            result.push_back(Pair("output_pubkey", HexStr(vchOutputPubKey)));
            return result;
        }

        throw JSONRPCError(RPC_WALLET_ERROR, "Fee estimation failed after maximum retries");
    }

    throw JSONRPCError(RPC_INTERNAL_ERROR, "Unexpected sp_send exit");
}
