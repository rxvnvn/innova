// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
// lock ordering requirements:
//   cs_main -> cs_wallet -> cs_spvutxos (never reverse)
//   LOCK2(cs_main, cs_wallet) is the standard pattern used throughout.
//   cs_wallet alone is acceptable when cs_main is not needed.
//   cs_spvutxos is always acquired last when needed.

#include "wallet.h"
#include "txlat.h"
#include "walletdb.h"
#include "chainstate/observe_hook.h"
#include "innovarpc.h"
#include "crypter.h"
#include "ui_interface.h"
#include "base58.h"
#include "kernel.h"
#include "blockindex_residency_counters.h"
#include "blockindex_shadow_startup.h"
#include "coincontrol.h"
#include "collateral.h"
#include "collateralnode.h"
#include "bloom.h"
#include "silentpayments.h"
#include <openssl/crypto.h>  
#include <boost/algorithm/string/replace.hpp>
#include <boost/range/algorithm.hpp>
#include <boost/numeric/ublas/matrix.hpp>
#include <chrono>

static int64_t RPCPerfTimeMicros(bool fEnabled)
{
    if (!fEnabled)
        return 0;
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

#define RPCPERF_LOG(...) do { try { printf(__VA_ARGS__); } catch (...) {} } while (0)

namespace {

static ColdHotSeamResult GetWalletTxStakingDepth(const CWalletTx& wtx, int* outDepth)
{
    if (!outDepth)
        return COLD_HOT_SEAM_AUTHORITY_FAILURE;
    AssertLockHeld(cs_main);
    *outDepth = 0;

    const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
    if (nav)
    {
        std::string err;
        return nav->GetHybridSvmMaturityAuthorityR(
            BlockIndexLogicalId(wtx.hashBlock), wtx.GetHash(),
            wtx.vMerkleBranch, wtx.nIndex, outDepth, &err);
    }

    CBlockIndex* pindexRet = NULL;
    *outDepth = wtx.GetDepthInMainChain(pindexRet);
    return (*outDepth > 0) ? COLD_HOT_SEAM_OK : COLD_HOT_SEAM_NOT_FOUND;
}

static ColdHotSeamResult GetWalletTxStakingMaturity(const CWalletTx& wtx,
    int* outDepth, int* outBlocksToMaturity)
{
    if (outDepth)
        *outDepth = 0;
    if (outBlocksToMaturity)
        *outBlocksToMaturity = 0;

    int depth = 0;
    const ColdHotSeamResult r = GetWalletTxStakingDepth(wtx, &depth);
    if (outDepth)
        *outDepth = depth;
    if (r == COLD_HOT_SEAM_AUTHORITY_FAILURE)
        return r;

    if (wtx.IsCoinBase() || wtx.IsCoinStake())
    {
        const int nWalletMaturity = fRegTest ? nCoinbaseMaturity : nCoinbaseMaturity + 10;
        if (outBlocksToMaturity)
            *outBlocksToMaturity = std::max(0, nWalletMaturity - depth);
    }
    return r;
}

static bool GetWalletTxStakingAvailableCredit(const CWallet& wallet,
    const CWalletTx& wtx, int64_t* outCredit)
{
    if (!outCredit)
        return false;
    int64_t nCredit = 0;
    for (unsigned int i = 0; i < wtx.vout.size(); i++)
    {
        if (wtx.IsSpent(i))
            continue;
        nCredit += wallet.GetCredit(wtx.vout[i], ISMINE_SPENDABLE);
        if (!MoneyRange(nCredit))
            throw std::runtime_error("GetWalletTxStakingAvailableCredit() : value out of range");
    }
    *outCredit = nCredit;
    return true;
}

static bool GetTransparentStakingBalance(const CWallet& wallet,
    int64_t* outBalance)
{
    if (!outBalance)
        return false;
    AssertLockHeld(cs_main);
    int64_t nTotal = 0;
    for (WalletTxMap::const_iterator it = wallet.mapWallet.begin(); it != wallet.mapWallet.end(); ++it)
    {
        const CWalletTx& wtx = it->second;
        if (!wtx.IsFinal())
            continue;
        if (!pindexBest || wtx.nTime > pindexBest->GetBlockTime())
            continue;

        int nDepth = 0;
        int nBlocksToMaturity = 0;
        const ColdHotSeamResult r = GetWalletTxStakingMaturity(wtx, &nDepth, &nBlocksToMaturity);
        if (r == COLD_HOT_SEAM_AUTHORITY_FAILURE)
            return false;
        if (nDepth < 1 || nBlocksToMaturity > 0)
            continue;

        int64_t nCredit = 0;
        if (!GetWalletTxStakingAvailableCredit(wallet, wtx, &nCredit))
            return false;
        if (nCredit > 0 && nTotal > std::numeric_limits<int64_t>::max() - nCredit)
        {
            printf("ERROR: GetTransparentStakingBalance() : balance overflow detected\n");
            *outBalance = std::numeric_limits<int64_t>::max();
            return true;
        }
        nTotal += nCredit;
    }
    *outBalance = nTotal;
    return true;
}

static bool GetStakingTxIndex(const CWallet& wallet,
    const CWalletTx& wtx, CTxIndex* txindexOut)
{
    if (!txindexOut)
        return false;
    AssertLockHeld(cs_main);
    // M6: in the armed-V2 mode the staking tx source position is resolved BY VALUE
    // from the authoritative block (COLD_HOT_SEAM/BI-V2), never from the legacy
    // txindex. Non-V2 keeps the legacy fast path.
    const bool fV2Stake = (s16::g_chainObserve.enabled && s16::g_chainObserve.hardFail &&
                           s16::g_chainObserve.store && s16::g_chainObserve.store->opened());
    if (fV2Stake)
    {
        // V2: a stake source must be confirmed on-chain (hashBlock set) for its
        // position to be derivable from the authoritative block; no legacy read.
        if (wtx.hashBlock == uint256(0))
            return false;
    }
    // By-value authority path (the V2 staking source; non-V2 only when fHybridSPV).
    unsigned int nFile = 0, nBlockPos = 0;
    bool available = false;
    if (GetStakingSourceDiskPositionR(wallet, wtx.hashBlock,
            &nFile, &nBlockPos, &available) != COLD_HOT_SEAM_OK || !available)
        return false;

    CBlock block;
    if (!block.ReadFromDisk(nFile, nBlockPos, true) ||
        block.GetHash() != wtx.hashBlock)
        return false;

    unsigned int nTxPos = nBlockPos +
        ::GetSerializeSize(CBlock(), SER_DISK, CLIENT_VERSION) -
        (2 * GetSizeOfCompactSize(0)) +
        GetSizeOfCompactSize(block.vtx.size());
    for (std::vector<CTransaction>::const_iterator it = block.vtx.begin();
         it != block.vtx.end(); ++it)
    {
        if (it->GetHash() == wtx.GetHash())
        {
            *txindexOut = CTxIndex(CDiskTxPos(nFile, nBlockPos, nTxPos),
                                   it->vout.size());
            return true;
        }
        nTxPos += ::GetSerializeSize(*it, SER_DISK, CLIENT_VERSION);
    }
    return false;
}

} // namespace

ColdHotSeamResult GetStakingSourceDiskPositionR(const CWallet& wallet,
    const uint256& hashBlock, unsigned int* nFileOut,
    unsigned int* nBlockPosOut, bool* availableOut)
{
    if (nFileOut)
        *nFileOut = 0;
    if (nBlockPosOut)
        *nBlockPosOut = 0;
    if (availableOut)
        *availableOut = false;
    AssertLockHeld(cs_main);
    if (hashBlock == uint256(0))
        return COLD_HOT_SEAM_NOT_FOUND;

    ColdHotSeamSnapshot snap;
    std::string err;
    const ColdHotSeamResult authority =
        GetStakingSourceAuthority(hashBlock, &snap, &err);
    if (authority != COLD_HOT_SEAM_OK)
        return authority;

    // Materialization is an ephemeral stable-hash overlay only.  It is
    // consulted after authority and cannot make an inactive source acceptable.
    StakingMaterializationInfo materialization;
    const bool overlayKnown =
        wallet.GetStakingMaterialization(hashBlock, &materialization);
    if (overlayKnown)
    {
        if (materialization.available && materialization.nFile > 0)
        {
            if (nFileOut)
                *nFileOut = materialization.nFile;
            if (nBlockPosOut)
                *nBlockPosOut = materialization.nBlockPos;
            if (availableOut)
                *availableOut = true;
        }
        // A pending/invalidated overlay entry intentionally suppresses stale
        // frozen coordinates until a new arrival publishes usable bytes.
        return COLD_HOT_SEAM_OK;
    }

    if (snap.snapshot.nFile > 0)
    {
        if (nFileOut)
            *nFileOut = snap.snapshot.nFile;
        if (nBlockPosOut)
            *nBlockPosOut = snap.snapshot.nBlockPos;
        if (availableOut)
            *availableOut = true;
    }
    return COLD_HOT_SEAM_OK;
}

ColdHotSeamResult GetStakingSourceAuthority(const uint256& hashBlock,
                                            ColdHotSeamSnapshot* out,
                                            std::string* error)
{
    if (!out || hashBlock == uint256(0))
        return COLD_HOT_SEAM_NOT_FOUND;
    AssertLockHeld(cs_main);
    *out = ColdHotSeamSnapshot();

    const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
    if (nav)
    {
        const ColdHotSeamResult r = nav->ResolveLogicalR(
            BlockIndexLogicalId(hashBlock), out, error);
        if (r != COLD_HOT_SEAM_OK)
            return r;
        if (!out->snapshot.fInMainChain)
            return COLD_HOT_SEAM_NOT_ACTIVE;
        if (out->ref.IsCold() && !nav->VerifySeam(error))
            return COLD_HOT_SEAM_AUTHORITY_FAILURE;
        return COLD_HOT_SEAM_OK;
    }

    std::map<uint256, CBlockIndex*>::const_iterator it = mapBlockIndex.find(hashBlock);
    g_res_legacyaccessor_fallbacks++;
    if (it == mapBlockIndex.end() || !it->second)
        return COLD_HOT_SEAM_NOT_FOUND;
    if (!it->second->IsInMainChain())
        return COLD_HOT_SEAM_NOT_ACTIVE;
    out->snapshot = LegacyBlockIndexAccessor().LookupByHash(hashBlock);
    if (!out->snapshot.found)
        return COLD_HOT_SEAM_NOT_FOUND;
    return COLD_HOT_SEAM_OK;
}

#if BOOST_VERSION >= 107300
#include <boost/bind/bind.hpp>
using boost::placeholders::_1;
using boost::placeholders::_2;
#else
#include <boost/bind.hpp>
#endif

using namespace std;

unsigned int nStakeSplitAge = 1 * 24 * 60 * 60;
int64_t nStakeCombineThreshold = 1000 * COIN;
int64_t nStakeMinSplitThreshold = 100 * COIN;

int64_t gcd(int64_t n,int64_t m) { return m == 0 ? n : gcd(m, n % m); }
static uint64_t CoinWeightCost(const COutput &out)
{
    int64_t nTimeWeight = (int64_t)GetTime() - (int64_t)out.tx->nTime;
    CBigNum bnCoinDayWeight = CBigNum(out.tx->vout[out.i].nValue) * nTimeWeight / (24 * 60 * 60);
    return bnCoinDayWeight.getuint64();
}

//////////////////////////////////////////////////////////////////////////////
//
// mapWallet
//

struct CompareValueOnly
{
    bool operator()(const pair<int64_t, pair<const CWalletTx*, unsigned int> >& t1,
                    const pair<int64_t, pair<const CWalletTx*, unsigned int> >& t2) const
    {
        return t1.first < t2.first;
    }
};

CPubKey CWallet::GenerateNewKey()
{
    AssertLockHeld(cs_wallet); // mapKeyMetadata
    bool fCompressed = CanSupportFeature(FEATURE_COMPRPUBKEY); // default to compressed public keys if we want 0.6.0 wallets

    RandAddSeedPerfmon();
    CKey key;
    key.MakeNewKey(fCompressed);

    if (!key.IsValid())
        throw std::runtime_error("CWallet::GenerateNewKey() : MakeNewKey failed");

    // Compressed public keys were introduced in version 0.6.0
    if (fCompressed)
        SetMinVersion(FEATURE_COMPRPUBKEY);

    CPubKey pubkey = key.GetPubKey();
    if (!pubkey.IsValid())
        throw std::runtime_error("CWallet::GenerateNewKey() : GetPubKey returned invalid key");

    // Create new metadata
    int64_t nCreationTime = GetTime();
    mapKeyMetadata[pubkey.GetID()] = CKeyMetadata(nCreationTime);
    if (!nTimeFirstKey || nCreationTime < nTimeFirstKey)
        nTimeFirstKey = nCreationTime;

    if (!AddKey(key))
        throw std::runtime_error("CWallet::GenerateNewKey() : AddKey failed");
    return key.GetPubKey();
}

bool CWallet::AddKeyPubKey(const CKey& key, const CPubKey &pubkey)
{
    AssertLockHeld(cs_wallet); // mapKeyMetadata

    if (!CCryptoKeyStore::AddKeyPubKey(key, pubkey))
        return false;

    // check if we need to remove from watch-only
    CScript script;
    script = GetScriptForDestination(pubkey.GetID());
    if (HaveWatchOnly(script))
        RemoveWatchOnly(script);

    if (!fFileBacked)
        return true;
    if (!IsCrypted())
        return CWalletDB(strWalletFile).WriteKey(pubkey, key.GetPrivKey(), mapKeyMetadata[pubkey.GetID()]);
    return true;
}

bool CWallet::AddKeyInDBTxn(CWalletDB* pdb, const CKey& key)
{
    LOCK(cs_KeyStore);
    // -- can't use CWallet::AddKey(), as in a db transaction
    //    hack: pwalletdbEncryption CCryptoKeyStore::AddKey calls CWallet::AddCryptedKey
    //    DB Write() uses activeTxn
    CWalletDB *pwalletdbEncryptionOld = pwalletdbEncryption;
    pwalletdbEncryption = pdb;

    if (!CCryptoKeyStore::AddKey(key))
    {
        printf("CCryptoKeyStore::AddKey failed.\n");
        return false;
    };

    CPubKey pubkey = key.GetPubKey();

    pwalletdbEncryption = pwalletdbEncryptionOld;

    if (fFileBacked
        && !IsCrypted())
    {
        if (!pdb->WriteKey(pubkey, key.GetPrivKey(), mapKeyMetadata[pubkey.GetID()]))
        {
            printf("WriteKey() failed.\n");
            return false;
        };
    };
    return true;
};

bool CWallet::AddCryptedKey(const CPubKey &vchPubKey, const vector<unsigned char> &vchCryptedSecret)
{
    if (!CCryptoKeyStore::AddCryptedKey(vchPubKey, vchCryptedSecret))
        return false;
    if (!fFileBacked)
        return true;
    {
        LOCK(cs_wallet);
        if (pwalletdbEncryption)
            return pwalletdbEncryption->WriteCryptedKey(vchPubKey, vchCryptedSecret, mapKeyMetadata[vchPubKey.GetID()]);
        else
            return CWalletDB(strWalletFile).WriteCryptedKey(vchPubKey, vchCryptedSecret, mapKeyMetadata[vchPubKey.GetID()]);
    }
    return false;
}

bool CWallet::LoadKeyMetadata(const CPubKey &pubkey, const CKeyMetadata &meta)
{
    AssertLockHeld(cs_wallet); // mapKeyMetadata
    if (meta.nCreateTime && (!nTimeFirstKey || meta.nCreateTime < nTimeFirstKey))
        nTimeFirstKey = meta.nCreateTime;

    mapKeyMetadata[pubkey.GetID()] = meta;
    return true;
}

bool CWallet::LoadCryptedKey(const CPubKey &vchPubKey, const std::vector<unsigned char> &vchCryptedSecret)
{
    return CCryptoKeyStore::AddCryptedKey(vchPubKey, vchCryptedSecret);
}

bool CWallet::AddCScript(const CScript& redeemScript)
{
    if (!CCryptoKeyStore::AddCScript(redeemScript))
        return false;
    if (!fFileBacked)
        return true;
    return CWalletDB(strWalletFile).WriteCScript(Hash160(redeemScript), redeemScript);
}

// optional setting to unlock wallet for staking only
// serves to disable the trivial sendmoney when OS account compromised
// provides no real security
bool fWalletUnlockStakingOnly = false;

bool CWallet::LoadCScript(const CScript& redeemScript)
{
    /* A sanity check was added in pull #3843 to avoid adding redeemScripts
     * that never can be redeemed. However, old wallets may still contain
     * these. Do not add them to the wallet and warn. */
    if (redeemScript.size() > MAX_SCRIPT_ELEMENT_SIZE)
    {
        std::string strAddr = CBitcoinAddress(redeemScript.GetID()).ToString();
        printf("%s: Warning: This wallet contains a redeemScript of size %" PRIszu" which exceeds maximum size %i thus can never be redeemed. Do not use address %s.\n",
            __func__, redeemScript.size(), MAX_SCRIPT_ELEMENT_SIZE, strAddr.c_str());
        return true;
    }

    return CCryptoKeyStore::AddCScript(redeemScript);
}

bool CWallet::Lock()
{
    if (IsLocked())
        return true;

    if (fDebug)
        printf("Locking wallet.\n");

    {
        LOCK(cs_wallet);
        CWalletDB wdb(strWalletFile);

        // -- load encrypted spend_secret of stealth addresses
        CStealthAddress sxAddrTemp;
        std::set<CStealthAddress>::iterator it;
        for (it = stealthAddresses.begin(); it != stealthAddresses.end(); ++it)
        {
            if (it->scan_secret.size() < 32)
                continue; // stealth address is not owned
            // -- CStealthAddress are only sorted on spend_pubkey
            CStealthAddress &sxAddr = const_cast<CStealthAddress&>(*it);
            if (fDebug)
                printf("Recrypting stealth key %s\n", sxAddr.Encoded().c_str());

            sxAddrTemp.scan_pubkey = sxAddr.scan_pubkey;
            if (!wdb.ReadStealthAddress(sxAddrTemp))
            {
                printf("Error: Failed to read stealth key from db %s\n", sxAddr.Encoded().c_str());
                continue;
            }
            sxAddr.spend_secret = sxAddrTemp.spend_secret;
        };
    }
    return LockKeyStore();
};

bool CWallet::AddWatchOnly(const CScript &dest)
{
    if (!CCryptoKeyStore::AddWatchOnly(dest))
        return false;
    nTimeFirstKey = 1; // No birthday information for watch-only keys.
    NotifyWatchonlyChanged(true);
    if (!fFileBacked)
        return true;
    return CWalletDB(strWalletFile).WriteWatchOnly(dest);
}

bool CWallet::RemoveWatchOnly(const CScript &dest)
{
    AssertLockHeld(cs_wallet);
    if (!CCryptoKeyStore::RemoveWatchOnly(dest))
        return false;
    if (!HaveWatchOnly())
        NotifyWatchonlyChanged(false);
    if (fFileBacked)
        if (!CWalletDB(strWalletFile).EraseWatchOnly(dest))
            return false;

    return true;
}

bool CWallet::LoadWatchOnly(const CScript &dest)
{
    return CCryptoKeyStore::AddWatchOnly(dest);
}

bool CWallet::Unlock(const SecureString& strWalletPassphrase)
{
    CCrypter crypter;
    CKeyingMaterial vMasterKey;

    {
        LOCK(cs_wallet);
        if (!IsLocked())
            return false;

        BOOST_FOREACH(MasterKeyMap::value_type& pMasterKey, mapMasterKeys)
        {
            if(!crypter.SetKeyFromPassphrase(strWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
                return false;
            if (!crypter.Decrypt(pMasterKey.second.vchCryptedKey, vMasterKey))
                return false;
            if (!CCryptoKeyStore::Unlock(vMasterKey))
                return false;

            static const unsigned int MIN_DERIVE_ITERATIONS = 25000;
            if (pMasterKey.second.nDeriveIterations < MIN_DERIVE_ITERATIONS)
            {
                printf("Upgrading legacy wallet encryption from %u to %u iterations\n",
                       pMasterKey.second.nDeriveIterations, MIN_DERIVE_ITERATIONS);

                unsigned int nOldIterations = pMasterKey.second.nDeriveIterations;
                pMasterKey.second.nDeriveIterations = MIN_DERIVE_ITERATIONS;

                CCrypter upgrader;
                if (upgrader.SetKeyFromPassphrase(strWalletPassphrase, pMasterKey.second.vchSalt,
                                                   pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
                {
                    std::vector<unsigned char> vchNewCryptedKey;
                    if (upgrader.Encrypt(vMasterKey, vchNewCryptedKey))
                    {
                        pMasterKey.second.vchCryptedKey = vchNewCryptedKey;
                        CWalletDB(strWalletFile).WriteMasterKey(pMasterKey.first, pMasterKey.second);
                        printf("Successfully upgraded and persisted wallet encryption iterations\n");
                    }
                    else
                    {
                        pMasterKey.second.nDeriveIterations = nOldIterations;
                        printf("WARNING: Failed to re-encrypt master key during iteration upgrade\n");
                    }
                }
                else
                {
                    pMasterKey.second.nDeriveIterations = nOldIterations;
                    printf("WARNING: Failed to derive key during iteration upgrade\n");
                }
            }

            break;
        }

        UnlockStealthAddresses(vMasterKey);
        return true;
    }
    return false;
}

void CWallet::LockCoin(COutPoint& output)
{
    AssertLockHeld(cs_wallet); // setLockedCoins
    setLockedCoins.insert(output);
}

void CWallet::UnlockCoin(COutPoint& output)
{
    AssertLockHeld(cs_wallet); // setLockedCoins
    setLockedCoins.erase(output);
}

size_t CWallet::UnlockReservedCoins(CWalletTx& wtx)
{
    AssertLockHeld(cs_wallet); // setLockedCoins
    size_t nUnlocked = 0;
    for (std::vector<COutPoint>::iterator it = wtx.vReservedCoins.begin();
         it != wtx.vReservedCoins.end(); ++it)
    {
        if (IsLockedCoin(it->hash, it->n))
        {
            UnlockCoin(*it);
            ++nUnlocked;
        }
    }
    wtx.vReservedCoins.clear();
    return nUnlocked;
}

void CWallet::UnlockAllCoins()
{
    AssertLockHeld(cs_wallet); // setLockedCoins
    setLockedCoins.clear();
}

bool CWallet::IsLockedCoin(uint256 hash, unsigned int n) const
{
    AssertLockHeld(cs_wallet); // setLockedCoins
    COutPoint outpt(hash, n);

    return (setLockedCoins.count(outpt) > 0);
}

bool CWallet::IsSpent(const uint256& hash, unsigned int n) const
{
    AssertLockHeld(cs_wallet);

    std::map<uint256, CWalletTx>::const_iterator it = mapWallet.find(hash);
    if (it != mapWallet.end())
    {
        if (n >= it->second.vout.size())
            return false;
        return it->second.IsSpent(n);
    }

    if (fHybridSPV)
    {
        // lock ordering: cs_wallet -> cs_spvutxos (never reverse)
        LOCK(cs_spvutxos);
        COutPoint outpoint(hash, n);
        std::map<COutPoint, SPVUtxo>::const_iterator spvIt = mapSPVUtxos.find(outpoint);
        if (spvIt != mapSPVUtxos.end())
        {
            return spvIt->second.fSpent;
        }
    }

    return false;
}

void CWallet::ListLockedCoins(std::vector<COutPoint>& vOutpts)
{
    AssertLockHeld(cs_wallet); // setLockedCoins
    for (std::set<COutPoint>::iterator it = setLockedCoins.begin();
         it != setLockedCoins.end(); it++) {
        COutPoint outpt = (*it);
        vOutpts.push_back(outpt);
    }
}

bool CWallet::ChangeWalletPassphrase(const SecureString& strOldWalletPassphrase, const SecureString& strNewWalletPassphrase)
{
    bool fWasLocked = IsLocked();
    bool fResult = false;

    {
        LOCK(cs_wallet);
        Lock();

        CCrypter crypter;
        CKeyingMaterial vMasterKey;
        BOOST_FOREACH(MasterKeyMap::value_type& pMasterKey, mapMasterKeys)
        {
            if(!crypter.SetKeyFromPassphrase(strOldWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
            {
                if (!vMasterKey.empty())
                    OPENSSL_cleanse(&vMasterKey[0], vMasterKey.size());
                return false;
            }
            if (!crypter.Decrypt(pMasterKey.second.vchCryptedKey, vMasterKey))
            {
                if (!vMasterKey.empty())
                    OPENSSL_cleanse(&vMasterKey[0], vMasterKey.size());
                return false;
            }
            if (CCryptoKeyStore::Unlock(vMasterKey)
                && UnlockStealthAddresses(vMasterKey))
            {
                int64_t nStartTime = GetTimeMillis();
                crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod);
                pMasterKey.second.nDeriveIterations = pMasterKey.second.nDeriveIterations * (100 / ((double)(GetTimeMillis() - nStartTime)));

                nStartTime = GetTimeMillis();
                crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod);
                pMasterKey.second.nDeriveIterations = (pMasterKey.second.nDeriveIterations + pMasterKey.second.nDeriveIterations * 100 / ((double)(GetTimeMillis() - nStartTime))) / 2;

                if (pMasterKey.second.nDeriveIterations < 25000)
                    pMasterKey.second.nDeriveIterations = 25000;

                printf("Wallet passphrase changed to an nDeriveIterations of %i\n", pMasterKey.second.nDeriveIterations);

                if (!crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
                {
                    if (!vMasterKey.empty())
                        OPENSSL_cleanse(&vMasterKey[0], vMasterKey.size());
                    return false;
                }
                if (!crypter.Encrypt(vMasterKey, pMasterKey.second.vchCryptedKey))
                {
                    if (!vMasterKey.empty())
                        OPENSSL_cleanse(&vMasterKey[0], vMasterKey.size());
                    return false;
                }

                CWalletDB(strWalletFile).WriteMasterKey(pMasterKey.first, pMasterKey.second);
                if (fWasLocked)
                    Lock();
                fResult = true;
            }
            if (!vMasterKey.empty())
                OPENSSL_cleanse(&vMasterKey[0], vMasterKey.size());
            if (fResult)
                return true;
        }
    }

    return false;
}

void CWallet::SetBestChain(const CBlockLocator& loc)
{
    CWalletDB walletdb(strWalletFile);
    walletdb.WriteBestBlock(loc);
}

bool CWallet::SetMinVersion(enum WalletFeature nVersion, CWalletDB* pwalletdbIn, bool fExplicit)
{
    LOCK(cs_wallet); // nWalletVersion
    if (nWalletVersion >= nVersion)
        return true;

    // when doing an explicit upgrade, if we pass the max version permitted, upgrade all the way
    if (fExplicit && nVersion > nWalletMaxVersion)
            nVersion = FEATURE_LATEST;

    nWalletVersion = nVersion;

    if (nVersion > nWalletMaxVersion)
        nWalletMaxVersion = nVersion;

    if (fFileBacked)
    {
        CWalletDB* pwalletdb = pwalletdbIn ? pwalletdbIn : new CWalletDB(strWalletFile);
        if (nWalletVersion > 40000)
            pwalletdb->WriteMinVersion(nWalletVersion);
        if (!pwalletdbIn)
            delete pwalletdb;
    }

    return true;
}

bool CWallet::SetMaxVersion(int nVersion)
{
    LOCK(cs_wallet); // nWalletVersion, nWalletMaxVersion
    // cannot downgrade below current version
    if (nWalletVersion > nVersion)
        return false;

    nWalletMaxVersion = nVersion;

    return true;
}

bool CWallet::EncryptWallet(const SecureString& strWalletPassphrase)
{
    if (IsCrypted())
        return false;

    CKeyingMaterial vMasterKey;
    RandAddSeedPerfmon();

    vMasterKey.resize(WALLET_CRYPTO_KEY_SIZE);
    RAND_bytes(&vMasterKey[0], WALLET_CRYPTO_KEY_SIZE);

    CMasterKey kMasterKey(nDerivationMethodIndex);

    RandAddSeedPerfmon();
    kMasterKey.vchSalt.resize(WALLET_CRYPTO_SALT_SIZE);
    RAND_bytes(&kMasterKey.vchSalt[0], WALLET_CRYPTO_SALT_SIZE);

    CCrypter crypter;
    int64_t nStartTime = GetTimeMillis();
    crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, 25000, kMasterKey.nDerivationMethod);
    kMasterKey.nDeriveIterations = 2500000 / ((double)(GetTimeMillis() - nStartTime));

    nStartTime = GetTimeMillis();
    crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, kMasterKey.nDeriveIterations, kMasterKey.nDerivationMethod);
    kMasterKey.nDeriveIterations = (kMasterKey.nDeriveIterations + kMasterKey.nDeriveIterations * 100 / ((double)(GetTimeMillis() - nStartTime))) / 2;

    if (kMasterKey.nDeriveIterations < 25000)
        kMasterKey.nDeriveIterations = 25000;

    printf("Encrypting Wallet with an nDeriveIterations of %i\n", kMasterKey.nDeriveIterations);

    if (!crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, kMasterKey.nDeriveIterations, kMasterKey.nDerivationMethod))
        return false;
    if (!crypter.Encrypt(vMasterKey, kMasterKey.vchCryptedKey))
        return false;

    {
        LOCK(cs_wallet);
        mapMasterKeys[++nMasterKeyMaxID] = kMasterKey;
        if (fFileBacked)
        {
            pwalletdbEncryption = new CWalletDB(strWalletFile);
            if (!pwalletdbEncryption->TxnBegin())
                return false;
            pwalletdbEncryption->WriteMasterKey(nMasterKeyMaxID, kMasterKey);
        }

        if (!EncryptKeys(vMasterKey))
        {
            if (fFileBacked)
                pwalletdbEncryption->TxnAbort();
            return false;
        }

        std::set<CStealthAddress>::iterator it;
        for (it = stealthAddresses.begin(); it != stealthAddresses.end(); ++it)
        {
            if (it->scan_secret.size() < 32)
                continue; // stealth address is not owned
            // -- CStealthAddress is only sorted on spend_pubkey
            CStealthAddress &sxAddr = const_cast<CStealthAddress&>(*it);

            if (fDebug)
                printf("Encrypting stealth key %s\n", sxAddr.Encoded().c_str());

            std::vector<unsigned char> vchCryptedSecret;

            CSecret vchSecret;
            vchSecret.resize(32);
            if (sxAddr.spend_secret.size() != 32)
            {
                printf("EncryptWallet() : stealth spend_secret wrong size %d\n", (int)sxAddr.spend_secret.size());
                continue;
            }
            memcpy(&vchSecret[0], &sxAddr.spend_secret[0], 32);

            uint256 iv = Hash(sxAddr.spend_pubkey.begin(), sxAddr.spend_pubkey.end());
            if (!EncryptSecret(vMasterKey, vchSecret, iv, vchCryptedSecret))
            {
                printf("Error: Failed encrypting stealth key %s\n", sxAddr.Encoded().c_str());
                continue;
            };

            sxAddr.spend_secret = vchCryptedSecret;
            pwalletdbEncryption->WriteStealthAddress(sxAddr);
        };

        // Encryption was introduced in version 0.4.0
        SetMinVersion(FEATURE_WALLETCRYPT, pwalletdbEncryption, true);

        if (fFileBacked)
        {
            if (!pwalletdbEncryption->TxnCommit())
            {
                // Keys encrypted in memory but not persisted to disk
                printf("ERROR: EncryptWallet - failed to commit encrypted keys to disk. Wallet may be in inconsistent state.\n");
                delete pwalletdbEncryption;
                pwalletdbEncryption = NULL;
                return false;
            }

            delete pwalletdbEncryption;
            pwalletdbEncryption = NULL;
        }

        Lock();
        Unlock(strWalletPassphrase);
        NewKeyPool();
        Lock();

        // Need to completely rewrite the wallet file; if we don't, bdb might keep
        // bits of the unencrypted private key in slack space in the database file.
        CDB::Rewrite(strWalletFile);

    }
    NotifyStatusChanged(this);

    return true;
}

int64_t CWallet::IncOrderPosNext(CWalletDB *pwalletdb)
{
    AssertLockHeld(cs_wallet); // nOrderPosNext
    int64_t nRet = nOrderPosNext++;
    if (pwalletdb) {
        pwalletdb->WriteOrderPosNext(nOrderPosNext);
    } else {
        CWalletDB(strWalletFile).WriteOrderPosNext(nOrderPosNext);
    }
    return nRet;
}

CWallet::TxItems CWallet::OrderedTxItems(std::list<CAccountingEntry>& acentries, std::string strAccount, bool fShowCoinstake)
{
    AssertLockHeld(cs_wallet); // mapWallet
    CWalletDB walletdb(strWalletFile);

    // First: get all CWalletTx and CAccountingEntry into a sorted-by-order multimap.
    TxItems txOrdered;

    // Note: maintaining indices in the database of (account,time) --> txid and (account, time) --> acentry
    // would make this much faster for applications that do this a lot.
    for (map<uint256, CWalletTx>::iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
    {
        CWalletTx* wtx = &((*it).second);
        txOrdered.insert(make_pair(wtx->nOrderPos, TxPair(wtx, (CAccountingEntry*)0)));
    }
    acentries.clear();
    walletdb.ListAccountCreditDebit(strAccount, acentries);
    BOOST_FOREACH(CAccountingEntry& entry, acentries)
    {
        txOrdered.insert(make_pair(entry.nOrderPos, TxPair((CWalletTx*)0, &entry)));
    }

    return txOrdered;
}

void CWallet::WalletUpdateSpent(const CTransaction &tx, bool fBlock)
{
    // Anytime a signature is successfully verified, it's proof the outpoint is spent.
    // Update the wallet spent flag if it doesn't know due to wallet.dat being
    // restored from backup or the user making copies of wallet.dat.
    {
        LOCK(cs_wallet);
        BOOST_FOREACH(const CTxIn& txin, tx.vin)
        {
            std::map<uint256, CWalletTx>::iterator mi = mapWallet.find(txin.prevout.hash);
            //map<uint256, CWalletTx>::iterator mi = mapWallet.find(txin.prevout.hash);
            if (mi != mapWallet.end())
            {
                CWalletTx& wtx = (*mi).second;
                if (txin.prevout.n >= wtx.vout.size())
                    printf("WalletUpdateSpent: bad wtx %s\n", wtx.GetHash().ToString().c_str());
                else if (!wtx.IsSpent(txin.prevout.n) && IsMine(wtx.vout[txin.prevout.n]))
                {
                    printf("WalletUpdateSpent found spent coins\n");
                    wtx.MarkSpent(txin.prevout.n);
                    wtx.WriteToDisk();
                    NotifyTransactionChanged(this, txin.prevout.hash, CT_UPDATED);
					vMintingWalletUpdated.push_back(txin.prevout.hash);
                    if (fHybridSPV)
                    {
                        MarkSPVUtxoSpent(txin.prevout);
                    }
                }
            }
        }

        if (fBlock)
        {
            uint256 hash = tx.GetHash();
            map<uint256, CWalletTx>::iterator mi = mapWallet.find(hash);
            if (mi == mapWallet.end())
                return;
            CWalletTx& wtx = (*mi).second;

            BOOST_FOREACH(const CTxOut& txout, tx.vout)
            {
                if (IsMine(txout))
                {
                    wtx.MarkUnspent(&txout - &tx.vout[0]);
                    wtx.WriteToDisk();
                    NotifyTransactionChanged(this, hash, CT_UPDATED);
					vMintingWalletUpdated.push_back(hash);
                }
            }
        }

    }
}

void CWallet::MarkDirty()
{
    {
        LOCK(cs_wallet);
        BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, mapWallet)
            item.second.MarkDirty();
    }
}

bool CWallet::AddToWallet(const CWalletTx& wtxIn)
{
    uint256 hash = wtxIn.GetHash();
    {
        LOCK(cs_wallet);
        // Inserts only if not already there, returns tx inserted or tx found
        pair<map<uint256, CWalletTx>::iterator, bool> ret = mapWallet.insert(make_pair(hash, wtxIn));
        CWalletTx& wtx = (*ret.first).second;
        wtx.BindWallet(this);
        bool fInsertedNew = ret.second;
        if (fInsertedNew)
        {
            wtx.nTimeReceived = GetAdjustedTime();
            wtx.nOrderPos = IncOrderPosNext();

            wtx.nTimeSmart = wtx.nTimeReceived;
            if (wtxIn.hashBlock != 0)
            {
                if (mapBlockIndex.count(wtxIn.hashBlock))
                {
                    unsigned int latestNow = wtx.nTimeReceived;
                    unsigned int latestEntry = 0;
                    {
                        // Tolerate times up to the last timestamp in the wallet not more than 5 minutes into the future
                        int64_t latestTolerated = latestNow + 300;
                        std::list<CAccountingEntry> acentries;
                        TxItems txOrdered = OrderedTxItems(acentries);
                        for (TxItems::reverse_iterator it = txOrdered.rbegin(); it != txOrdered.rend(); ++it)
                        {
                            CWalletTx *const pwtx = (*it).second.first;
                            if (pwtx == &wtx)
                                continue;
                            CAccountingEntry *const pacentry = (*it).second.second;
                            int64_t nSmartTime;
                            if (pwtx)
                            {
                                nSmartTime = pwtx->nTimeSmart;
                                if (!nSmartTime)
                                    nSmartTime = pwtx->nTimeReceived;
                            }
                            else
                                nSmartTime = pacentry->nTime;
                            if (nSmartTime <= latestTolerated)
                            {
                                latestEntry = nSmartTime;
                                if (nSmartTime > latestNow)
                                    latestNow = nSmartTime;
                                break;
                            }
                        }
                    }

                    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(wtxIn.hashBlock);
                    if (mi != mapBlockIndex.end() && mi->second) {
                        unsigned int blocktime = mi->second->nTime;
                        wtx.nTimeSmart = std::max(latestEntry, std::min(blocktime, latestNow));
                    }
                }
                else
                    printf("AddToWallet() : found %s in block %s not in index\n",
                           wtxIn.GetHash().ToString().substr(0,10).c_str(),
                           wtxIn.hashBlock.ToString().c_str());
            }
        }

        bool fUpdated = false;
        if (!fInsertedNew)
        {
            // Merge
            if (wtxIn.hashBlock != 0 && wtxIn.hashBlock != wtx.hashBlock)
            {
                wtx.hashBlock = wtxIn.hashBlock;
                fUpdated = true;
            }
            if (wtxIn.nIndex != -1 && (wtxIn.vMerkleBranch != wtx.vMerkleBranch || wtxIn.nIndex != wtx.nIndex))
            {
                wtx.vMerkleBranch = wtxIn.vMerkleBranch;
                wtx.nIndex = wtxIn.nIndex;
                fUpdated = true;
            }
            if (wtxIn.fFromMe && wtxIn.fFromMe != wtx.fFromMe)
            {
                wtx.fFromMe = wtxIn.fFromMe;
                fUpdated = true;
            }
            fUpdated |= wtx.UpdateSpent(wtxIn.vfSpent);
        }

        //// debug print
        printf("AddToWallet %s  %s%s\n", wtxIn.GetHash().ToString().substr(0,10).c_str(), (fInsertedNew ? "new" : ""), (fUpdated ? "update" : ""));

        // Write to disk
        if (fInsertedNew || fUpdated)
            if (!wtx.WriteToDisk())
                return false;
#ifndef QT_GUI
        // If default receiving address gets used, replace it with a new one
        if (vchDefaultKey.IsValid()) {
            CScript scriptDefaultKey;
            scriptDefaultKey.SetDestination(vchDefaultKey.GetID());
            BOOST_FOREACH(const CTxOut& txout, wtx.vout)
            {
                if (txout.scriptPubKey == scriptDefaultKey)
                {
                    CPubKey newDefaultKey;
                    if (GetKeyFromPool(newDefaultKey, false))
                    {
                        SetDefaultKey(newDefaultKey);
                        SetAddressBookName(vchDefaultKey.GetID(), "");
                    }
                }
            }
        }
#endif
        // since AddToWallet is called directly for self-originating transactions, check for consumption of own coins
        WalletUpdateSpent(wtx, (wtxIn.hashBlock != 0));

        // Notify UI of new or updated transaction
        NotifyTransactionChanged(this, hash, fInsertedNew ? CT_NEW : CT_UPDATED);

		vMintingWalletUpdated.push_back(hash);

        // notify an external script when a wallet transaction comes in or is updated
        std::string strCmd = GetArg("-walletnotify", "");

        if ( !strCmd.empty())
        {
            boost::replace_all(strCmd, "%s", wtxIn.GetHash().GetHex());
            boost::thread t(runCommand, strCmd); // thread runs free
        }

    }
    return true;
}

// Add a transaction to the wallet, or update it.
// pblock is optional, but should be provided if the transaction is known to be in a block.
// If fUpdate is true, existing transactions will be updated.
bool CWallet::AddToWalletIfInvolvingMe(const CTransaction& tx, const CBlock* pblock, bool fUpdate, bool fFindBlock)
{
    //printf("AddToWalletIfInvolvingMe() %s\n", hash.ToString().c_str()); // happens often

    uint256 hash = tx.GetHash();
    {
        LOCK(cs_wallet);
        bool fExisted = mapWallet.count(hash);
        if (fExisted && !fUpdate)
        {
            return false;
        };

        mapValue_t mapNarr;
        if (stealthAddresses.size() > 0 && !fDisableStealth) FindStealthTransactions(tx, mapNarr);

        bool fIsMine = false;
        if (fExisted || fIsMine || IsMine(tx) || IsFromMe(tx))
        {
            CWalletTx wtx(this, tx);

            if (!mapNarr.empty())
                wtx.mapValue.insert(mapNarr.begin(), mapNarr.end());

            // Get merkle branch if transaction was found in a block
            const CBlock* pcblock = (CBlock*)pblock;
            if (pcblock)
                wtx.SetMerkleBranch(pcblock);

            return AddToWallet(wtx); //AddToWallet(wtx, hash);
        } else
        {
            WalletUpdateSpent(tx);
        };
    }
    return false;
}

bool CWallet::EraseFromWallet(uint256 hash)
{
    if (!fFileBacked)
        return false;
    {
        LOCK(cs_wallet);
        if (mapWallet.erase(hash))
            CWalletDB(strWalletFile).EraseTx(hash);
    }
    return true;
}


isminetype CWallet::IsMine(const CTxIn &txin) const
{
    {
        LOCK(cs_wallet);
        map<uint256, CWalletTx>::const_iterator mi = mapWallet.find(txin.prevout.hash);
        if (mi != mapWallet.end())
        {
            const CWalletTx& prev = (*mi).second;
            if (txin.prevout.n < prev.vout.size())
                return IsMine(prev.vout[txin.prevout.n]);
        }
    }
    return MINE_NO;
}

int64_t CWallet::GetDebit(const CTxIn &txin, const isminefilter& filter) const
{
    {
        LOCK(cs_wallet);
        map<uint256, CWalletTx>::const_iterator mi = mapWallet.find(txin.prevout.hash);
        if (mi != mapWallet.end())
        {
            const CWalletTx& prev = (*mi).second;
            if (txin.prevout.n < prev.vout.size())
                if (IsMine(prev.vout[txin.prevout.n]) & filter)
                    return prev.vout[txin.prevout.n].nValue;
        }
    }
    return 0;
}


bool CWallet::IsDenominated(const CTxIn &txin) const
{
    {
        LOCK(cs_wallet);
        map<uint256, CWalletTx>::const_iterator mi = mapWallet.find(txin.prevout.hash);
        if (mi != mapWallet.end())
        {
            const CWalletTx& prev = (*mi).second;
            if (txin.prevout.n < prev.vout.size()) return IsDenominatedAmount(prev.vout[txin.prevout.n].nValue);
        }
    }
    return false;
}

bool CWallet::IsDenominatedAmount(int64_t nInputAmount) const
{
    BOOST_FOREACH(int64_t d, colLateralDenominations)
        if(nInputAmount == d)
            return true;
    return false;
}

bool CWallet::IsChange(const CTxOut& txout) const
{
    CTxDestination address;

    // TODO: fix handling of 'change' outputs. The assumption is that any
    // payment to a script that is ours but is not in the address book
    // is change. That assumption is likely to break when we implement multisignature
    // wallets that return change back into a multi-signature-protected address;
    // a better way of identifying which outputs are 'the send' and which are
    // 'the change' will need to be implemented (maybe extend CWalletTx to remember
    // which output, if any, was change).
    if (::IsMine(*this, txout.scriptPubKey))
    {
        CTxDestination address;
        if (!ExtractDestination(txout.scriptPubKey, address) && txout.scriptPubKey[0] != OP_RETURN) //Fix Null TX Data
            return true;

        LOCK(cs_wallet);
        if (!mapAddressBook.count(address))
            return true;
    }
    return false;
}

int64_t CWalletTx::GetTxTime() const
{
    int64_t n = nTimeSmart;
    return n ? n : nTimeReceived;
}

int CWalletTx::GetRequestCount() const
{
    // Returns -1 if it wasn't being tracked
    int nRequests = -1;
    {
        LOCK(pwallet->cs_wallet);
        if (IsCoinBase() || IsCoinStake())
        {
            // Generated block
            if (hashBlock != 0)
            {
                map<uint256, int>::const_iterator mi = pwallet->mapRequestCount.find(hashBlock);
                if (mi != pwallet->mapRequestCount.end())
                    nRequests = (*mi).second;
            }
        }
        else
        {
            // Did anyone request this transaction?
            map<uint256, int>::const_iterator mi = pwallet->mapRequestCount.find(GetHash());
            if (mi != pwallet->mapRequestCount.end())
            {
                nRequests = (*mi).second;

                // How about the block it's in?
                if (nRequests == 0 && hashBlock != 0)
                {
                    map<uint256, int>::const_iterator mi = pwallet->mapRequestCount.find(hashBlock);
                    if (mi != pwallet->mapRequestCount.end())
                        nRequests = (*mi).second;
                    else
                        nRequests = 1; // If it's in someone else's block it must have got out
                }
            }
        }
    }
    return nRequests;
}

void CWalletTx::GetAmounts(list<COutputEntry>& listReceived,
                           list<COutputEntry>& listSent, int64_t& nFee, string& strSentAccount, const isminefilter& filter) const
{
    nFee = 0;
    listReceived.clear();
    listSent.clear();
    strSentAccount = strFromAccount;

    // Compute fee:
    int64_t nDebit = GetDebit(filter);
    if (nDebit > 0) // debit>0 means we signed/sent this transaction
    {
        int64_t nValueOut = GetValueOut();
        nFee = nDebit - nValueOut;
    };

    // treat coinstake as a single "recieve" entry
    if (IsCoinStake())
    {
        for (unsigned int i = 0; i < vout.size(); ++i)
        {
            const CTxOut& txout = vout[i];
            isminetype fIsMine = pwallet->IsMine(txout);

            // get my vout with positive output
            if (!(fIsMine & filter) || txout.nValue <= 0)
                        continue;

            // get address
            CTxDestination address = CNoDestination();
            ExtractDestination(txout.scriptPubKey, address);

            // nfee is negative for coinstake generation, because we are gaining money from it
            COutputEntry output = {address, -nFee, (int)i};
            listReceived.push_back(output);
            nFee = 0;
            return;
        }

        // if we reach here there is probably a mistake
        COutputEntry output = {CNoDestination(), 0, 0};
        listReceived.push_back(output);
        return;
    }

	// Sent/received.
    for (unsigned int i = 0; i < vout.size(); ++i)
    {
		const CTxOut& txout = vout[i];
		// Skip special stake out
        if (txout.scriptPubKey.empty())
            continue;

        opcodetype firstOpCode;
        CScript::const_iterator pc = txout.scriptPubKey.begin();
        if (txout.scriptPubKey.GetOp(pc, firstOpCode)
            && firstOpCode == OP_RETURN)
            continue;


        bool fIsMine;
        // Only need to handle txouts if AT LEAST one of these is true:
        //   1) they debit from us (sent)
        //   2) the output is to us (received)
        if (nDebit > 0)
        {
            // Don't report 'change' txouts
            if (pwallet->IsChange(txout))
                continue;
            fIsMine = pwallet->IsMine(txout);
        } else
        if (!(fIsMine = pwallet->IsMine(txout)))
            continue;

        // In either case, we need to get the destination address
        CTxDestination address;
        if (!ExtractDestination(txout.scriptPubKey, address) && !txout.scriptPubKey.IsUnspendable())
        {
            printf("CWalletTx::GetAmounts: Unknown transaction type found, txid %s\n",
                this->GetHash().ToString().c_str());
            address = CNoDestination();
        };

		COutputEntry output = {address, txout.nValue, (int)i};

        // If we are debited by the transaction, add the output as a "sent" entry
        if (nDebit > 0)
            listSent.push_back(output);

        // If we are receiving the output, add it as a "received" entry
        if (fIsMine)
            listReceived.push_back(output);
    };
}

void CWalletTx::GetAccountAmounts(const string& strAccount, int64_t& nReceived,
                                  int64_t& nSent, int64_t& nFee, const isminefilter& filter) const
{
    nReceived = nSent = nFee = 0;

    int64_t allFee;
    string strSentAccount;
    list<COutputEntry> listReceived;
    list<COutputEntry> listSent;
    GetAmounts(listReceived, listSent, allFee, strSentAccount, filter);

    if (strAccount == strSentAccount)
    {
        BOOST_FOREACH(const COutputEntry& s, listSent)
            nSent += s.amount;
        nFee = allFee;
    }
    {
        LOCK(pwallet->cs_wallet);
        BOOST_FOREACH(const COutputEntry& r, listReceived)
        {
            if (pwallet->mapAddressBook.count(r.destination))
            {
                map<CTxDestination, string>::const_iterator mi = pwallet->mapAddressBook.find(r.destination);
                if (mi != pwallet->mapAddressBook.end() && (*mi).second == strAccount)
                    nReceived += r.amount;
            }
            else if (strAccount.empty())
            {
                nReceived += r.amount;
            }
        }
    }
}

void CWalletTx::AddSupportingTransactions()
{
    vtxPrev.clear();

    const int COPY_DEPTH = 3;
    if (SetMerkleBranch() < COPY_DEPTH)
    {
        vector<uint256> vWorkQueue;
        BOOST_FOREACH(const CTxIn& txin, vin)
            vWorkQueue.push_back(txin.prevout.hash);

        // This critsect is OK because txdb is already open
        {
            LOCK(pwallet->cs_wallet);
            map<uint256, const CMerkleTx*> mapWalletPrev;
            set<uint256> setAlreadyDone;
            for (unsigned int i = 0; i < vWorkQueue.size(); i++)
            {
                uint256 hash = vWorkQueue[i];
                if (setAlreadyDone.count(hash))
                    continue;
                setAlreadyDone.insert(hash);

                CMerkleTx tx;
                map<uint256, CWalletTx>::const_iterator mi = pwallet->mapWallet.find(hash);
                if (mi != pwallet->mapWallet.end())
                {
                    tx = (*mi).second;
                    BOOST_FOREACH(const CMerkleTx& txWalletPrev, (*mi).second.vtxPrev)
                        mapWalletPrev[txWalletPrev.GetHash()] = &txWalletPrev;
                }
                else if (mapWalletPrev.count(hash))
                {
                    tx = *mapWalletPrev[hash];
                }
                else
                {
                    CTxIndex txindex;
                    if (!s16_V2ReadTxIndex(hash, txindex) || !tx.ReadFromDisk(txindex.pos))
                    {
                        printf("ERROR: AddSupportingTransactions() : unsupported transaction\n");
                        continue;
                    }
                }

                int nDepth = tx.SetMerkleBranch();
                vtxPrev.push_back(tx);

                if (nDepth < COPY_DEPTH)
                {
                    BOOST_FOREACH(const CTxIn& txin, tx.vin)
                        vWorkQueue.push_back(txin.prevout.hash);
                }
            }
        }
    }

    reverse(vtxPrev.begin(), vtxPrev.end());
}

bool CWalletTx::WriteToDisk()
{
    return CWalletDB(pwallet->strWalletFile).WriteTx(GetHash(), *this);
}

// Scan the block chain (starting in pindexStart) for transactions
// from or to us. If fUpdate is true, found transactions that already
// exist in the wallet will be updated.
int CWallet::ScanForWalletTransactions(CBlockIndex* pindexStart, bool fUpdate)
{
    int ret = 0;

    CBlockIndex* pindex = pindexStart;
    {

        int dProgressTop;
        {
            LOCK(cs_main);
            dProgressTop = pindexBest->nHeight;
        }

        int dProgressStart = pindex ? pindex->nHeight : 0;
        int dProgressCurrent = dProgressStart;
        int dProgressTotal =  dProgressTop - dProgressStart;
        double dProgressShow = 0;
        double dProgressShowPrev = 0;

        while (pindex && !fShutdown)
        {
            if (dProgressCurrent > 0)
                dProgressShow = ((static_cast<double>(dProgressCurrent) / dProgressTop) * 100.0);

            if ((pindex->nHeight % 100 == 0) && (dProgressTotal > 0))
            {
                if (dProgressShowPrev != dProgressShow)
                {
                    dProgressShowPrev = dProgressShow;
                    uiInterface.InitMessage(strprintf("%s %d/%d %s... (%.2f%%)",_("Rescanning").c_str(), dProgressCurrent , dProgressTop,_("blocks").c_str(),dProgressShow));
                }
            }
            // no need to read and scan block, if block was created before
            // our wallet birthday (as adjusted for block time variability)
            if (nTimeFirstKey && (pindex->nTime < (nTimeFirstKey - 7200))) {
                pindex = pindex->pnext;
                continue;
            }

            CBlock block;
            block.ReadFromDisk(pindex, true);
            BOOST_FOREACH(CTransaction& tx, block.vtx)
            {
                LOCK(cs_wallet);
                if (AddToWalletIfInvolvingMe(tx, &block, fUpdate))
                    ret++;
            }
            pindex = pindex->pnext;

            // Update current height for progress
            if (pindex) dProgressCurrent = pindex->nHeight;

        }

        uiInterface.InitMessage(_("Rescanning complete."));
    }
    return ret;
}

// By-value rescan (A.10.1k/D-prereq). Mirrors the legacy
// ScanForWalletTransactions loop but iterates the active chain BY HEIGHT via
// BlockIndexActiveChainReader, reading each block by nFile/nBlockPos with NO
// resident CBlockIndex* / pnext / mapBlockIndex dependency. Honors the same
// nTimeFirstKey birthday early-continue. Only a single transient block is
// resident at a time (no accumulating active-chain cache).
int CWallet::ScanForWalletTransactionsByValue(const BlockIndexActiveChainReader& reader,
                                              int32_t startHeight,
                                              bool fUpdate)
{
    int ret = 0;
    if (!reader.IsOpen())
        return 0;

    const int32_t activeHeight = (int32_t)reader.GetActiveHeight();
    if (activeHeight < startHeight)
        return 0;

    int32_t h = (startHeight < 0) ? 0 : startHeight;
    int32_t dProgressTop = activeHeight;
    int64_t nBirthday = nTimeFirstKey; // snapshot under the caller's wallet contract

    while (h <= activeHeight && !fShutdown)
    {
        // no need to read and scan block before our wallet birthday
        // (mirrors wallet.cpp:1825-1828 legacy early-continue)
        BlockIndexActiveBlock ab;
        std::string lkErr;
        if (!reader.LookupByHeight(h, &ab, &lkErr))
            break;
        if (nBirthday && (ab.nTime < (uint32_t)(nBirthday - 7200)))
        {
            ++h;
            continue;
        }

        CBlock block;
        if (!block.ReadFromDisk(ab.nFile, ab.nBlockPos))
        {
            ++h;
            continue;
        }
        BOOST_FOREACH(CTransaction& tx, block.vtx)
        {
            LOCK(cs_wallet);
            if (AddToWalletIfInvolvingMe(tx, &block, fUpdate))
                ret++;
        }
        ++h;
    }
    return ret;
}

/*
void CWallet::ReacceptWalletTransactions()
{
    LOCK2(cs_main, cs_wallet);
    BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, mapWallet)
    {
        const uint256& wtxid = item.first;
        CWalletTx& wtx = item.second;
        if (wtx.GetHash() != wtxid)
        {
            printf("ERROR: ResendWalletTransactions: hash mismatch for tx %s\n", wtxid.ToString().c_str());
            continue;
        }

        int nDepth = wtx.GetDepthInMainChain();

        if (!wtx.IsCoinBase() || wtx.IsCoinStake() && nDepth < 0)
        {
            // Try to add to memory pool
            LOCK(mempool.cs);
            wtx.AcceptToMemoryPool(false);
        }
    }
}*/

void CWallet::ReacceptWalletTransactions()
{
    const bool fV2Wallet = (s16::g_chainObserve.enabled && s16::g_chainObserve.hardFail &&
                            s16::g_chainObserve.store && s16::g_chainObserve.store->opened());
    // P2 (S4): in V2 wallet mode the Store3A branch below NEVER touches the legacy
    // txdb (all reads/writes route via Store3A + TX-LOC). Constructing it here would
    // resurrect the removed txleveldb. The legacy handle is only needed on the non-V2
    // legacy migration path; pass a V2-inert placeholder in V2 mode (its methods are
    // no-ops, so it is safe to construct unconditionally at a single gate below).
    bool fRepeat = true;
    while (fRepeat)
    {
        LOCK2(cs_main, cs_wallet);
        fRepeat = false;
        vector<CDiskTxPos> vMissingTx;
        BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, mapWallet)
        {
            CWalletTx& wtx = item.second;
            if (wtx.IsCoinBase() && wtx.IsSpent(0))
                continue;
            if (wtx.IsCoinStake() && wtx.vout.size() > 1 && wtx.IsSpent(1))
                continue;

            CTxIndex txindex;
            bool fUpdated = false;
            if (fV2Wallet)
            {
                // M5: V2 reconstruction from Store3A + TX-LOC (no legacy txindex).
                // A confirmed-on-chain tx (TX-LOC/Store3A present) gets any MINE output whose
                // coin is ABSENT from Store3A marked spent (it was spent on-chain), exactly as
                // legacy txindex.vSpent did. A not-yet-confirmed tx is re-accepted (routed via
                // the V2 mempool from M4). Fail-closed: on a Store3A authority error we do NOT
                // mark spent (never stale a coin on an authority hiccup).
                if (s16::V2WalletTxidKnown(wtx.GetHash()))
                {
                    for (unsigned int i = 0; i < wtx.vout.size(); i++)
                    {
                        if (wtx.IsSpent(i))
                            continue;
                        cs::OutPoint k; memcpy(k.txid.data(), wtx.GetHash().begin(), 32); k.n = i;
                        bool found = false; cs::Coin coin; cs::ChainError ce;
                        if (!s16::g_chainObserve.store->get(k, &coin, &found, &ce))
                        {
                            printf("ERROR: ReacceptWalletTransactions() : Store3A read failed (V2); not marking %s\n",
                                   wtx.GetHash().ToString().substr(0,10).c_str());
                            continue;   // fail-closed: no authority -> do not change spent state
                        }
                        if (!found && IsMine(wtx.vout[i]))
                        {
                            wtx.MarkSpent(i);
                            fUpdated = true;
                            if (fHybridSPV)
                            {
                                COutPoint outpoint(wtx.GetHash(), i);
                                MarkSPVUtxoSpent(outpoint);
                            }
                        }
                    }
                    if (fUpdated)
                    {
                        printf("ReacceptWalletTransactions found spent coins (V2 Store3A)\n");
                        wtx.MarkDirty();
                        wtx.WriteToDisk();
                    }
                }
                else
                {
                    // Not confirmed on-chain yet: (re-)accept to the pool (V2 mempool).
                    if (!(wtx.IsCoinBase() || wtx.IsCoinStake()))
                        wtx.AcceptWalletTransaction();
                }
            }
            else if (s16_V2ReadTxIndex(wtx.GetHash(), txindex))
            {
                // Update fSpent if a tx got spent somewhere else by a copy of wallet.dat
                if (txindex.vSpent.size() != wtx.vout.size())
                {
                    printf("ERROR: ReacceptWalletTransactions() : txindex.vSpent.size() %" PRIszu" != wtx.vout.size() %" PRIszu"\n", txindex.vSpent.size(), wtx.vout.size());
                    continue;
                }
                for (unsigned int i = 0; i < txindex.vSpent.size(); i++)
                {
                    if (wtx.IsSpent(i))
                        continue;
                    if (!txindex.vSpent[i].IsNull() && IsMine(wtx.vout[i]))
                    {
                        wtx.MarkSpent(i);
                        fUpdated = true;
                        vMissingTx.push_back(txindex.vSpent[i]);
                        if (fHybridSPV)
                        {
                            COutPoint outpoint(wtx.GetHash(), i);
                            MarkSPVUtxoSpent(outpoint);
                        }
                    }
                }
                if (fUpdated)
                {
                    printf("ReacceptWalletTransactions found spent coins\n");
                    wtx.MarkDirty();
                    wtx.WriteToDisk();
                }
            }
            else
            {
                // Re-accept any txes of ours that aren't already in a block
                if (!(wtx.IsCoinBase() || wtx.IsCoinStake()))
                    wtx.AcceptWalletTransaction();
            }
        }
        if (!vMissingTx.empty())
        {
            // TODO: optimize this to scan just part of the block chain?
            if (ScanForWalletTransactions(pindexGenesisBlock))
                fRepeat = true;  // Found missing transactions: re-do re-accept.
        }
    }
}


void CWalletTx::RelayWalletTransaction()
{
    const bool fV2Wallet = (s16::g_chainObserve.enabled && s16::g_chainObserve.hardFail &&
                            s16::g_chainObserve.store && s16::g_chainObserve.store->opened());
    BOOST_FOREACH(const CMerkleTx& tx, vtxPrev)
    {
        if (!(tx.IsCoinBase() || tx.IsCoinStake()))
        {
            uint256 hash = tx.GetHash();
            if (!s16::V2WalletTxidKnown(hash))
                RelayTransaction((CTransaction)tx, hash);
        }
    }
    if (!(IsCoinBase() || IsCoinStake()))
    {
        uint256 hash = GetHash();
        if (!s16::V2WalletTxidKnown(hash))
        {
            printf("Relaying wtx %s\n", hash.ToString().substr(0,10).c_str());
            RelayTransaction((CTransaction)*this, hash);
        }
    }
}

void CWallet::ResendWalletTransactions(bool fForce)
{
    if (!fForce)
    {
        // Do this infrequently and randomly to avoid giving away
        // that these are our transactions.
        static int64_t nNextTime;
        if (GetTime() < nNextTime)
            return;
        bool fFirst = (nNextTime == 0);
        nNextTime = GetTime() + GetRand(30 * 60);
        if (fFirst)
            return;

        // Only do it if there's been a new block since last time
        static int64_t nLastTime;
        if (nTimeBestReceived < nLastTime)
            return;
        nLastTime = GetTime();
    }

    // Rebroadcast any of our txes that aren't in a block yet
    printf("ResendWalletTransactions()\n");
    {
        LOCK(cs_wallet);
        // Sort them in chronological order
        multimap<unsigned int, CWalletTx*> mapSorted;
        BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, mapWallet)
        {
            CWalletTx& wtx = item.second;
            // Don't rebroadcast until it's had plenty of time that
            // it should have gotten in already by now.
            if (fForce || nTimeBestReceived - (int64_t)wtx.nTimeReceived > 5 * 60)
                mapSorted.insert(make_pair(wtx.nTimeReceived, &wtx));
        }
        BOOST_FOREACH(PAIRTYPE(const unsigned int, CWalletTx*)& item, mapSorted)
        {
            CWalletTx& wtx = *item.second;
            if (wtx.CheckTransaction())
                wtx.RelayWalletTransaction();
            else
                printf("ResendWalletTransactions() : CheckTransaction failed for transaction %s\n", wtx.GetHash().ToString().c_str());
        }
    }
}






//////////////////////////////////////////////////////////////////////////////
//
// Actions
//


int64_t CWallet::GetBalance() const
{
    int64_t nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;
            if (pcoin->IsTrusted())
            {
                int64_t nCredit = pcoin->GetAvailableCredit();
                if (nCredit > 0 && nTotal > std::numeric_limits<int64_t>::max() - nCredit)
                {
                    printf("ERROR: GetBalance() : balance overflow detected\n");
                    return std::numeric_limits<int64_t>::max();
                }
                nTotal += nCredit;
            }
        }
    }

    return nTotal;
}

// Single-pass replacement for the five aggregate walks above. Predicates are
// copied verbatim from GetBalance/GetUnconfirmedBalance/GetImmatureBalance/
// GetStake/GetNewMint so the returned values are identical; the only change is
// that IsTrusted()/GetDepthInMainChain()/GetBlocksToMaturity() are evaluated
// ONCE per transaction and shared by every aggregate. That removes five
// redundant wallet walks -- and with them five redundant rounds of block-index
// tip lookups (each of which seeks into tip-records.dat / blk*.dat) -- per
// getinfo call.
CWallet::Aggregates CWallet::GetAggregates() const
{
    Aggregates out;
    const int64_t kMax = std::numeric_limits<int64_t>::max();
    bool balanceOverflow = false;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;

            const bool fTrusted = pcoin->IsTrusted();
            const int nDepth = pcoin->GetDepthInMainChain();
            const int64_t nToMaturity = pcoin->GetBlocksToMaturity();

            // GetBalance()
            if (fTrusted)
            {
                int64_t nCredit = pcoin->GetAvailableCredit();
                if (nCredit > 0 && out.balance > kMax - nCredit)
                    balanceOverflow = true;
                else if (!balanceOverflow)
                    out.balance += nCredit;
            }
            // GetUnconfirmedBalance()
            if (!pcoin->IsFinal() || (!fTrusted && nDepth == 0))
            {
                int64_t nCredit = pcoin->GetAvailableCredit();
                if (nCredit > 0 && out.unconfirmed > kMax - nCredit)
                    out.unconfirmed = kMax;
                else if (out.unconfirmed != kMax)
                    out.unconfirmed += nCredit;
            }
            // GetImmatureBalance()
            if ((pcoin->IsCoinBase() || pcoin->IsCoinStake()) && nToMaturity > 0 &&
                pcoin->IsInMainChain())
            {
                int64_t nCredit = pcoin->GetImmatureCredit();
                if (nCredit > 0 && out.immature > kMax - nCredit)
                    out.immature = kMax;
                else if (out.immature != kMax)
                    out.immature += nCredit;
            }
            // GetStake()
            if (pcoin->IsCoinStake() && nToMaturity > 0 && nDepth > 0)
                out.stake += pcoin->GetCredit(ISMINE_SPENDABLE);
            // GetNewMint()
            if (pcoin->IsCoinBase() && nToMaturity > 0 && nDepth > 0)
                out.newmint += pcoin->GetCredit(ISMINE_SPENDABLE);
        }
    }
    if (balanceOverflow)
    {
        printf("ERROR: GetBalance() : balance overflow detected\n");
        out.balance = kMax;
    }
    return out;
}


int64_t CWallet::GetUnlockedBalance() const
{
    int64_t nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const CWalletTx* pcoin = &(*it).second;

            if (pcoin->IsTrusted() && pcoin->GetDepthInMainChain() > 0)
            {
                int64_t nCredit = pcoin->GetUnlockedCredit();
                if (nCredit > 0 && nTotal > std::numeric_limits<int64_t>::max() - nCredit)
                    return std::numeric_limits<int64_t>::max();
                nTotal += nCredit;
            }
        }
    }

    return nTotal;
}

int64_t CWallet::GetLockedBalance() const
{
    int64_t nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it) {
            const CWalletTx* pcoin = &(*it).second;

            if (pcoin->IsTrusted() && pcoin->GetDepthInMainChain() > 0)
            {
                int64_t nCredit = pcoin->GetLockedCredit();
                if (nCredit > 0 && nTotal > std::numeric_limits<int64_t>::max() - nCredit)
                    return std::numeric_limits<int64_t>::max();
                nTotal += nCredit;
            }
        }
    }
    return nTotal;
}

int64_t CWallet::GetUnconfirmedBalance() const
{
    int64_t nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;
            if (!pcoin->IsFinal() || (!pcoin->IsTrusted() && pcoin->GetDepthInMainChain() == 0))
            {
                int64_t nCredit = pcoin->GetAvailableCredit();
                if (nCredit > 0 && nTotal > std::numeric_limits<int64_t>::max() - nCredit)
                    return std::numeric_limits<int64_t>::max();
                nTotal += nCredit;
            }
        }
    }
    return nTotal;
}

int64_t CWallet::GetImmatureBalance() const
{
    int64_t nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;
            if ((pcoin->IsCoinBase() || pcoin->IsCoinStake()) && pcoin->GetBlocksToMaturity() > 0 && pcoin->IsInMainChain())
            {
                int64_t nCredit = pcoin->GetImmatureCredit();
                if (nCredit > 0 && nTotal > std::numeric_limits<int64_t>::max() - nCredit)
                    return std::numeric_limits<int64_t>::max();
                nTotal += nCredit;
            }
        }
    }
    return nTotal;
}

int64_t CWallet::GetWatchOnlyBalance() const
{
    int64_t nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;
            if (pcoin->IsTrusted())
            {
                int64_t nCredit = pcoin->GetAvailableWatchOnlyCredit();
                if (nCredit > 0 && nTotal > std::numeric_limits<int64_t>::max() - nCredit)
                    return std::numeric_limits<int64_t>::max();
                nTotal += nCredit;
            }
        }
    }

    return nTotal;
}

int64_t CWallet::GetUnconfirmedWatchOnlyBalance() const
{
    int64_t nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;
            if (!IsFinalTx(*pcoin) || (!pcoin->IsTrusted() && pcoin->GetDepthInMainChain() == 0))
                nTotal += pcoin->GetAvailableWatchOnlyCredit();
        }
    }
    return nTotal;
}

int64_t CWallet::GetImmatureWatchOnlyBalance() const
{
    int64_t nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;
            if ((pcoin->IsCoinBase() || pcoin->IsCoinStake()) && pcoin->GetBlocksToMaturity() > 0 && pcoin->IsInMainChain())
                nTotal += pcoin->GetImmatureWatchOnlyCredit();
        }
    }
    return nTotal;
}

CBloomFilter* CWallet::CreateSPVBloomFilter(double nFPRate, unsigned int nFlags) const
{
    LOCK(cs_wallet);

    unsigned int nElements = 0;
    nElements += setKeyPool.size();
    nElements += mapKeyMetadata.size();
    nElements += mapAddressBook.size();

    if (nElements < 10)
        nElements = 10;

    CBloomFilter* pfilter = new CBloomFilter(nElements, nFPRate, GetRandInt(std::numeric_limits<int>::max()), nFlags);

    std::set<CKeyID> setKeys;
    GetKeys(setKeys);
    for (const CKeyID& keyid : setKeys)
    {
        std::vector<unsigned char> vchPubKeyHash(keyid.begin(), keyid.end());
        pfilter->insert(vchPubKeyHash);
    }

    for (const std::pair<CTxDestination, std::string>& item : mapAddressBook)
    {
        const CTxDestination& dest = item.first;
        if (const CKeyID* keyid = boost::get<CKeyID>(&dest))
        {
            std::vector<unsigned char> vchPubKeyHash(keyid->begin(), keyid->end());
            pfilter->insert(vchPubKeyHash);
        }
        else if (const CScriptID* scriptid = boost::get<CScriptID>(&dest))
        {
            std::vector<unsigned char> vchScriptHash(scriptid->begin(), scriptid->end());
            pfilter->insert(vchScriptHash);
        }
    }

    for (const CStealthAddress& sxAddr : stealthAddresses)
    {
        if (sxAddr.scan_pubkey.size() >= 20)
        {
            uint160 hash = Hash160(sxAddr.scan_pubkey);
            std::vector<unsigned char> vchHash(hash.begin(), hash.end());
            pfilter->insert(vchHash);
        }
    }

    printf("SPV: Created bloom filter with %u elements, FP rate %.6f\n", nElements, nFPRate);
    return pfilter;
}

bool CWallet::ProcessMerkleBlock(const CMerkleBlock& merkleBlock, std::vector<uint256>& vMatch)
{
    CPartialMerkleTree txnCopy = merkleBlock.txn;

    uint256 merkleRoot = txnCopy.ExtractMatches(vMatch);

    if (merkleRoot != merkleBlock.header.hashMerkleRoot)
    {
        printf("SPV: Merkle root mismatch in block\n");
        return false;
    }

    if (fDebug)
        printf("SPV: Merkle block validated with %u matching transactions\n", (unsigned int)vMatch.size());

    return true;
}

void CWallet::RequestSPVTransactions(CNode* pnode, int nStartHeight)
{
    if (!pnode)
        return;

    LOCK(cs_main);

    CBloomFilter* pfilter = CreateSPVBloomFilter(0.0001, BLOOM_UPDATE_ALL);
    if (pfilter)
    {
        pnode->PushMessage("filterload", *pfilter);
        printf("SPV: Sent bloom filter to peer %s\n", pnode->addr.ToString().c_str());

        CBlockIndex* pindex = pindexGenesisBlock;
        if (nStartHeight > 0)
        {
            pindex = pindexBest;
            while (pindex && pindex->nHeight > nStartHeight)
                pindex = pindex->pprev;
        }

        if (pindex)
        {
            pnode->PushGetBlocks(pindex, uint256(0), ibdmetrics::GETBLOCKS_SOURCE_WALLET_RESCAN);
            printf("SPV: Requested filtered blocks from height %d\n", pindex->nHeight);
        }

        delete pfilter;
    }
}

// populate vCoins with vector of spendable COutputs
// coin availability is checked under LOCK2(cs_main, cs_wallet).
// Callers that use the result for spending (e.g. CreateTransaction) must also hold
// cs_wallet to prevent TOCTOU race between availability check and coin reservation.
void CWallet::AvailableCoins(vector<COutput>& vCoins, bool fOnlyConfirmed, const CCoinControl *coinControl) const
{
    vCoins.clear();

    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;

            if (!pcoin->IsFinal())
                continue;

            if (fOnlyConfirmed && !pcoin->IsTrusted())
                continue;

            if (pcoin->IsCoinBase() && pcoin->GetBlocksToMaturity() > 0)
                continue;

            if(pcoin->IsCoinStake() && pcoin->GetBlocksToMaturity() > 0)
                continue;

            int nDepth = pcoin->GetDepthInMainChain();
            if (nDepth < 0)
                continue;

            for (unsigned int i = 0; i < pcoin->vout.size(); i++) {
                isminetype mine = IsMine(pcoin->vout[i]);
                if (!(pcoin->IsSpent(i)) && mine != MINE_NO &&
                    !IsLockedCoin((*it).first, i) && pcoin->vout[i].nValue >= nMinimumInputValue &&
                    (!coinControl || !coinControl->HasSelected() || coinControl->IsSelected((*it).first, i)))
                        vCoins.push_back(COutput(pcoin, i, nDepth, (mine & ISMINE_SPENDABLE) != ISMINE_NO));
            }

        }
    }
}

void CWallet::AvailableCoinsMN(vector<COutput>& vCoins, bool fOnlyConfirmed, bool fOnlyUnlocked, const CCoinControl *coinControl, AvailableCoinsType coin_type) const
{
    vCoins.clear();

    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;

            if (!pcoin->IsFinal())
                continue;

            if (fOnlyConfirmed && !pcoin->IsTrusted())
                continue;

            if (pcoin->IsCoinBase() && pcoin->GetBlocksToMaturity() > 0)
                continue;

            if(pcoin->IsCoinStake() && pcoin->GetBlocksToMaturity() > 0)
                continue;

            int nDepth = pcoin->GetDepthInMainChain();
            if (nDepth <= 0) // NOTE: coincontrol fix / ignore 0 confirm
                continue;

            for (unsigned int i = 0; i < pcoin->vout.size(); i++) {
                bool found = false;
                if(coin_type == ONLY_DENOMINATED) {
                    //should make this a vector

                    found = IsDenominatedAmount(pcoin->vout[i].nValue);
                } else if(coin_type == ONLY_NONDENOMINATED || coin_type == ONLY_NONDENOMINATED_NOTMN) {
                    found = true;
                    if (IsCollateralAmount(pcoin->vout[i].nValue)) continue; // do not use collateral amounts
                    found = !IsDenominatedAmount(pcoin->vout[i].nValue);
                    if(found && coin_type == ONLY_NONDENOMINATED_NOTMN) found = (pcoin->vout[i].nValue != GetMNCollateral()*COIN); // do not use MN funds 25,000 INN
                } else {
                    found = true;
                }
                if(!found) continue;

                if (fOnlyUnlocked)
                {
                    if (IsLockedCoin(pcoin->GetHash(),i))
                        continue;
                }

				        //isminetype mine = IsMine(pcoin->vout[i]);
		            bool mine = IsMine(pcoin->vout[i]);

                    if (!(pcoin->IsSpent(i)) && pcoin->vout[i].nValue > 0 &&
                    (!coinControl || !coinControl->HasSelected() || coinControl->IsSelected((*it).first, i)))
                        vCoins.push_back(COutput(pcoin, i, nDepth, mine));
            }
        }
    }
}

bool CWallet::AvailableCoinsForStaking(vector<COutput>& vCoins, unsigned int nSpendTime) const
{
    vCoins.clear();

    {
        AssertLockHeld(cs_main);
        AssertLockHeld(cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;

            // Filtering by tx timestamp instead of block timestamp may give false positives but never false negatives
            if (pcoin->nTime + nStakeMinAge > nSpendTime)
                continue;

            int nDepth = 0;
            int nBlocksToMaturity = 0;
            const ColdHotSeamResult r = GetWalletTxStakingMaturity(*pcoin, &nDepth, &nBlocksToMaturity);
            if (r == COLD_HOT_SEAM_AUTHORITY_FAILURE)
                return false;
            if (nBlocksToMaturity > 0)
                continue;
            if (nDepth < 1)
                continue;

            for (unsigned int i = 0; i < pcoin->vout.size(); i++)
            {
                if (!(pcoin->IsSpent(i)) && IsMine(pcoin->vout[i]) && pcoin->vout[i].nValue >= nMinimumInputValue
                        && !IsLockedCoin((*it).first, i) // ignore outputs that are locked for MNs
                        )
					          vCoins.push_back(COutput(pcoin, i, nDepth, true));
            };
        };
    }
    return true;
}

static void ApproximateBestSubset(vector<pair<int64_t, pair<const CWalletTx*,unsigned int> > >vValue, int64_t nTotalLower, int64_t nTargetValue,
                                  vector<char>& vfBest, int64_t& nBest, int iterations = 1000)
{
    vector<char> vfIncluded;

    vfBest.assign(vValue.size(), true);
    nBest = nTotalLower;

    for (int nRep = 0; nRep < iterations && nBest != nTargetValue; nRep++)
    {
        vfIncluded.assign(vValue.size(), false);
        int64_t nTotal = 0;
        bool fReachedTarget = false;
        for (int nPass = 0; nPass < 2 && !fReachedTarget; nPass++)
        {
            for (unsigned int i = 0; i < vValue.size(); i++)
            {
                if (nPass == 0 ? (GetRandInt(2) == 0) : !vfIncluded[i])
                {
                    nTotal += vValue[i].first;
                    vfIncluded[i] = true;
                    if (nTotal >= nTargetValue)
                    {
                        fReachedTarget = true;
                        if (nTotal < nBest)
                        {
                            nBest = nTotal;
                            vfBest = vfIncluded;
                        }
                        nTotal -= vValue[i].first;
                        vfIncluded[i] = false;
                    }
                }
            }
        }
    }
}

// innova: total coins available for staking - WIP needs updating
int64_t CWallet::GetStakeAmount() const
{
    int64_t nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;
            if (pcoin->IsTrusted() && pcoin->GetDepthInMainChain() > 0) //Just pulls GetBalance() currently
                nTotal += pcoin->GetAvailableCredit();
        }
    }

    return nTotal;
}

int64_t CWallet::GetStake() const
{
    int64_t nTotal = 0;
    LOCK2(cs_main, cs_wallet);
    for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
    {
        const CWalletTx* pcoin = &(*it).second;
        if (pcoin->IsCoinStake() && pcoin->GetBlocksToMaturity() > 0 && pcoin->GetDepthInMainChain() > 0)
            nTotal += pcoin->GetCredit(ISMINE_SPENDABLE);
    }
    return nTotal;
}

int64_t CWallet::GetNewMint() const
{
    int64_t nTotal = 0;
    LOCK2(cs_main, cs_wallet);
    for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
    {
        const CWalletTx* pcoin = &(*it).second;
        if (pcoin->IsCoinBase() && pcoin->GetBlocksToMaturity() > 0 && pcoin->GetDepthInMainChain() > 0)
            nTotal += pcoin->GetCredit(ISMINE_SPENDABLE);
    }
    return nTotal;
}


struct LargerOrEqualThanThreshold
{
    int64_t threshold;
    LargerOrEqualThanThreshold(int64_t threshold) : threshold(threshold) {}
    bool operator()(pair<pair<int64_t,int64_t>,pair<const CWalletTx*,unsigned int> > const &v) const { return v.first.first >= threshold; }
};

bool CWallet::SelectCoinsMinConfByCoinAge(int64_t nTargetValue, unsigned int nSpendTime, int nConfMine, int nConfTheirs, std::vector<COutput> vCoins, set<pair<const CWalletTx*,unsigned int> >& setCoinsRet, int64_t& nValueRet) const
{
    setCoinsRet.clear();
    nValueRet = 0;

    vector<pair<COutput, uint64_t> > mCoins;
    BOOST_FOREACH(const COutput& out, vCoins)
    {
        mCoins.push_back(std::make_pair(out, CoinWeightCost(out)));
    }

    // List of values less than target
    pair<pair<int64_t,int64_t>, pair<const CWalletTx*,unsigned int> > coinLowestLarger;
    coinLowestLarger.first.second = std::numeric_limits<int64_t>::max();
    coinLowestLarger.second.first = NULL;
    vector<pair<pair<int64_t,int64_t>,pair<const CWalletTx*,unsigned int> > > vValue;
    int64_t nTotalLower = 0;
    boost::sort(mCoins, boost::bind(&std::pair<COutput, uint64_t>::second, _1) < boost::bind(&std::pair<COutput, uint64_t>::second, _2));

    BOOST_FOREACH(const PAIRTYPE(COutput, uint64_t)& output, mCoins)
    {
        const CWalletTx *pcoin = output.first.tx;

        if (output.first.nDepth < (pcoin->IsFromMe(ISMINE_ALL) ? nConfMine : nConfTheirs))
            continue;

        int i = output.first.i;

        // Follow the timestamp rules
        if (pcoin->nTime > nSpendTime)
            continue;

        int64_t n = pcoin->vout[i].nValue;

        pair<pair<int64_t,int64_t>,pair<const CWalletTx*,unsigned int> > coin = make_pair(make_pair(n,output.second),make_pair(pcoin, i));

        if (n < nTargetValue + CENT)
        {
            vValue.push_back(coin);
            nTotalLower += n;
        }
        else if (output.second < (uint64_t)coinLowestLarger.first.second)
        {
            coinLowestLarger = coin;
        }
    }

    if (nTotalLower < nTargetValue)
    {
        if (coinLowestLarger.second.first == NULL)
            return false;
        setCoinsRet.insert(coinLowestLarger.second);
        nValueRet += coinLowestLarger.first.first;
        return true;
    }

    // Calculate dynamic programming matrix
    if (vValue.empty())
        return false;  // Safety check - prevent crash on empty vValue
    int64_t nTotalValue = vValue[0].first.first;
    int64_t nGCD = vValue[0].first.first;
    for (unsigned int i = 1; i < vValue.size(); ++i)
    {
        nGCD = gcd(vValue[i].first.first, nGCD);
        nTotalValue += vValue[i].first.first;
    }
    nGCD = gcd(nTargetValue, nGCD);
    int64_t denom = nGCD;
    const int64_t k = 25;
    int64_t nExcess = nTotalValue - nTargetValue;
    int64_t approx;
    if (nExcess > 0 && (int64_t)vValue.size() <= std::numeric_limits<int64_t>::max() / nExcess)
        approx = int64_t(vValue.size() * nExcess) / k;
    else
        approx = std::numeric_limits<int64_t>::max();
    if (approx > nGCD)
    {
        denom = approx; // apply approximation
    }
    if (fDebug) cerr << "nGCD " << nGCD << " denom " << denom << " k " << k << endl;

    if (nTotalValue == nTargetValue)
    {
        for (unsigned int i = 0; i < vValue.size(); ++i)
        {
            setCoinsRet.insert(vValue[i].second);
        }
        nValueRet = nTotalValue;
        return true;
    }

    size_t nBeginBundles = vValue.size();
    size_t nTotalCoinValues = vValue.size();
    size_t nBeginCoinValues = 0;
    int64_t costsum = 0;
    vector<vector<pair<pair<int64_t,int64_t>,pair<const CWalletTx*,unsigned int> > >::iterator> vZeroValueBundles;
    if (denom != nGCD)
    {
        // All coin outputs that with zero value will always be added by the dynamic programming routine
        // So we collect them into bundles of value denom
        vector<pair<pair<int64_t,int64_t>,pair<const CWalletTx*,unsigned int> > >::iterator itZeroValue = std::stable_partition(vValue.begin(), vValue.end(), LargerOrEqualThanThreshold(denom));
        vZeroValueBundles.push_back(itZeroValue);
        pair<int64_t, int64_t> pBundle = make_pair(0, 0);
        nBeginBundles = itZeroValue - vValue.begin();
        nTotalCoinValues = nBeginBundles;
        while (itZeroValue != vValue.end())
        {
            pBundle.first += itZeroValue->first.first;
            pBundle.second += itZeroValue->first.second;
            itZeroValue++;
            if (pBundle.first >= denom)
            {
                vZeroValueBundles.push_back(itZeroValue);
                vValue[nTotalCoinValues].first = pBundle;
                pBundle = make_pair(0, 0);
                nTotalCoinValues++;
            }
        }
        // We need to recalculate the total coin value due to truncation of integer division
        nTotalValue = 0;
        for (unsigned int i = 0; i < nTotalCoinValues; ++i)
        {
            nTotalValue += vValue[i].first.first / denom;
        }
        // Check if dynamic programming is still applicable with the approximation
        if (nTargetValue/denom >= nTotalValue)
        {
            // We lose too much coin value through the approximation, i.e. the residual of the previous recalculation is too large
            // Since the partitioning of the previously sorted list is stable, we can just pick the first coin outputs in the list until we have a valid target value
            for (; nBeginCoinValues < nTotalCoinValues && (nTargetValue - nValueRet)/denom >= nTotalValue; ++nBeginCoinValues)
            {
                if (nBeginCoinValues >= nBeginBundles)
                {
                    if (fDebug) cerr << "prepick bundle item " << FormatMoney(vValue[nBeginCoinValues].first.first) << " normalized " << vValue[nBeginCoinValues].first.first / denom << " cost " << vValue[nBeginCoinValues].first.second << endl;
                    const size_t nBundle = nBeginCoinValues - nBeginBundles;
                    if (nBundle + 1 < vZeroValueBundles.size()) {
                        for (vector<pair<pair<int64_t,int64_t>,pair<const CWalletTx*,unsigned int> > >::iterator it = vZeroValueBundles[nBundle]; it != vZeroValueBundles[nBundle + 1]; ++it)
                        {
                            setCoinsRet.insert(it->second);
                        }
                    }
                }
                else
                {
                    if (fDebug) cerr << "prepicking " << FormatMoney(vValue[nBeginCoinValues].first.first) << " normalized " << vValue[nBeginCoinValues].first.first / denom << " cost " << vValue[nBeginCoinValues].first.second << endl;
                    setCoinsRet.insert(vValue[nBeginCoinValues].second);
                }
                nTotalValue -= vValue[nBeginCoinValues].first.first / denom;
                nValueRet += vValue[nBeginCoinValues].first.first;
                costsum += vValue[nBeginCoinValues].first.second;
            }
            if (nValueRet >= nTargetValue)
            {
                    if (fDebug) cerr << "Done without dynprog: " << "requested " << FormatMoney(nTargetValue) << "\tnormalized " << nTargetValue/denom + (nTargetValue % denom != 0 ? 1 : 0) << "\tgot " << FormatMoney(nValueRet) << "\tcost " << costsum << endl;
                    return true;
            }
        }
    }
    else
    {
        nTotalValue /= denom;
    }

    uint64_t nAppend = 1;
    if ((nTargetValue - nValueRet) % denom != 0)
    {
        // We need to decrease the capacity because of integer truncation
        nAppend--;
    }

    // The capacity (number of columns) corresponds to the amount of coin value we are allowed to discard
    size_t nMatrixRows = (nTotalCoinValues - nBeginCoinValues) + 1;
    int64_t nColCalc = (nTotalValue - (nTargetValue - nValueRet)/denom) + nAppend;
    size_t nMatrixCols = nColCalc > 0 ? (size_t)nColCalc : 1;

    const size_t nMaxMatrixSize = 5000000;
    if (nMatrixRows * nMatrixCols > nMaxMatrixSize || nMatrixRows == 0 || nMatrixCols == 0) {
        if (fDebug) cerr << "SelectCoinsMinConfByCost: matrix too large or invalid (" << nMatrixRows << "x" << nMatrixCols << "), falling back" << endl;
        return false;
    }

    boost::numeric::ublas::matrix<uint64_t> M(nMatrixRows, nMatrixCols, std::numeric_limits<int64_t>::max());
    boost::numeric::ublas::matrix<unsigned int> B(nMatrixRows, nMatrixCols);
    for (unsigned int j = 0; j < M.size2(); ++j)
    {
        M(0,j) = 0;
    }
    for (unsigned int i = 1; i < M.size1(); ++i)
    {
        uint64_t nWeight = vValue[nBeginCoinValues + i - 1].first.first / denom;
        uint64_t nValue = vValue[nBeginCoinValues + i - 1].first.second;
        //cerr << "Weight " << nWeight << " Value " << nValue << endl;
        for (unsigned int j = 0; j < M.size2(); ++j)
        {
            B(i, j) = j;
            if (nWeight <= j)
            {
                uint64_t nStep = M(i - 1, j - nWeight) + nValue;
                if (M(i - 1, j) >= nStep)
                {
                    M(i, j) = M(i - 1, j);
                }
                else
                {
                    M(i, j) = nStep;
                    B(i, j) = j - nWeight;
                }
            }
            else
            {
                M(i, j) = M(i - 1, j);
            }
        }
    }
    // Trace back optimal solution
    int64_t nPrev = M.size2() - 1;
    for (unsigned int i = M.size1() - 1; i > 0; --i)
    {
        //cerr << i - 1 << " " << vValue[i - 1].second.second << " " << vValue[i - 1].first.first << " " << vValue[i - 1].first.second << " " << nTargetValue << " " << nPrev << " " << (nPrev == B(i, nPrev) ? "XXXXXXXXXXXXXXX" : "") << endl;
        if (nPrev == B(i, nPrev))
        {
            const size_t nValue = nBeginCoinValues + i - 1;
            // Check if this is a bundle
            if (nValue >= nBeginBundles)
            {
                if (fDebug) cerr << "pick bundle item " << FormatMoney(vValue[nValue].first.first) << " normalized " << vValue[nValue].first.first / denom << " cost " << vValue[nValue].first.second << endl;
                const size_t nBundle = nValue - nBeginBundles;
                if (nBundle + 1 < vZeroValueBundles.size()) {
                    for (vector<pair<pair<int64_t,int64_t>,pair<const CWalletTx*,unsigned int> > >::iterator it = vZeroValueBundles[nBundle]; it != vZeroValueBundles[nBundle + 1]; ++it)
                    {
                        setCoinsRet.insert(it->second);
                    }
                }
            }
            else
            {
                if (fDebug) cerr << "pick " << nValue << " value " << FormatMoney(vValue[nValue].first.first) << " normalized " << vValue[nValue].first.first / denom << " cost " << vValue[nValue].first.second << endl;
                setCoinsRet.insert(vValue[nValue].second);
            }
            nValueRet += vValue[nValue].first.first;
            costsum += vValue[nValue].first.second;
        }
        nPrev = B(i, nPrev);
    }
    if (nValueRet < nTargetValue && !vZeroValueBundles.empty())
    {
        // If we get here it means that there are either not sufficient funds to pay the transaction or that there are small coin outputs left that couldn't be bundled
        // We try to fulfill the request by adding these small coin outputs
        for (vector<pair<pair<int64_t,int64_t>,pair<const CWalletTx*,unsigned int> > >::iterator it = vZeroValueBundles.back(); it != vValue.end() && nValueRet < nTargetValue; ++it)
        {
             setCoinsRet.insert(it->second);
             nValueRet += it->first.first;
        }
    }
    if (fDebug) cerr << "requested " << FormatMoney(nTargetValue) << "\tnormalized " << nTargetValue/denom + (nTargetValue % denom != 0 ? 1 : 0) << "\tgot " << FormatMoney(nValueRet) << "\tcost " << costsum << endl;
    if (fDebug) cerr << "M " << M.size1() << "x" << M.size2() << "; vValue.size() = " << vValue.size() << endl;
    return true;
}

// TODO: find appropriate place for this sort function
// move denoms down
bool less_then_denom (const COutput& out1, const COutput& out2)
{
    const CWalletTx *pcoin1 = out1.tx;
    const CWalletTx *pcoin2 = out2.tx;

    bool found1 = false;
    bool found2 = false;
    BOOST_FOREACH(int64_t d, colLateralDenominations) // loop through predefined denoms
    {
        if(pcoin1->vout[out1.i].nValue == d) found1 = true;
        if(pcoin2->vout[out2.i].nValue == d) found2 = true;
    }
    return (!found1 && found2);
}

static bool CmpDepth(const CWalletTx* a, const CWalletTx* b) { return a->nTime > b->nTime; }

bool CWallet::SelectCoinsMinConf(int64_t nTargetValue, unsigned int nSpendTime, int nConfMine, int nConfTheirs, vector<COutput> vCoins, set<pair<const CWalletTx*,unsigned int> >& setCoinsRet, int64_t& nValueRet) const
{
    setCoinsRet.clear();
    nValueRet = 0;

    // List of values less than target
    pair<int64_t, pair<const CWalletTx*,unsigned int> > coinLowestLarger;
    coinLowestLarger.first = std::numeric_limits<int64_t>::max();
    coinLowestLarger.second.first = NULL;
    vector<pair<int64_t, pair<const CWalletTx*,unsigned int> > > vValue;
    int64_t nTotalLower = 0;

    random_shuffle(vCoins.begin(), vCoins.end(), GetRandInt);

    // move denoms down on the list
    sort(vCoins.begin(), vCoins.end(), less_then_denom);

    // try to find nondenom first to prevent unneeded spending of mixed coins
    for (unsigned int tryDenom = 0; tryDenom < 2; tryDenom++)
    {
        if (fDebug) printf("[selectcoins] tryDenom: %d\n", tryDenom);
        vValue.clear();
        nTotalLower = 0;

    BOOST_FOREACH(const COutput &output, vCoins)
    {
        if (!output.fSpendable)
            continue;

        const CWalletTx *pcoin = output.tx;

        if (output.nDepth < (pcoin->IsFromMe(ISMINE_ALL) ? nConfMine : nConfTheirs))
            continue;

        int i = output.i;

        // Follow the timestamp rules
        if (pcoin->nTime > nSpendTime)
            continue;

        int64_t n = pcoin->vout[i].nValue;

        if (tryDenom == 0 && IsDenominatedAmount(n)) continue; // we don't want denom values on first run

        pair<int64_t,pair<const CWalletTx*,unsigned int> > coin = make_pair(n,make_pair(pcoin, i));

        if (n == nTargetValue)
        {
            setCoinsRet.insert(coin.second);
            nValueRet += coin.first;
            return true;
        }
        else if (n < nTargetValue + CENT)
        {
            vValue.push_back(coin);
            nTotalLower += n;
        }
        else if (n < coinLowestLarger.first)
        {
            coinLowestLarger = coin;
        }
    }

    if (nTotalLower == nTargetValue)
    {
        for (unsigned int i = 0; i < vValue.size(); ++i)
        {
            setCoinsRet.insert(vValue[i].second);
            nValueRet += vValue[i].first;
        }
        return true;
    }

    if (nTotalLower < nTargetValue)
    {
        if (coinLowestLarger.second.first == NULL)
            return false;
        setCoinsRet.insert(coinLowestLarger.second);
        nValueRet += coinLowestLarger.first;
        return true;
    }

    // Solve subset sum by stochastic approximation
    sort(vValue.rbegin(), vValue.rend(), CompareValueOnly());
    vector<char> vfBest;
    int64_t nBest;

    ApproximateBestSubset(vValue, nTotalLower, nTargetValue, vfBest, nBest, 1000);
    if (nBest != nTargetValue && nTotalLower >= nTargetValue + CENT)
        ApproximateBestSubset(vValue, nTotalLower, nTargetValue + CENT, vfBest, nBest, 1000);

    // If we have a bigger coin and (either the stochastic approximation didn't find a good solution,
    //                                   or the next bigger coin is closer), return the bigger coin
    if (coinLowestLarger.second.first &&
        ((nBest != nTargetValue && nBest < nTargetValue + CENT) || coinLowestLarger.first <= nBest))
    {
        setCoinsRet.insert(coinLowestLarger.second);
        nValueRet += coinLowestLarger.first;
    }
    else {
        for (unsigned int i = 0; i < vValue.size(); i++)
            if (vfBest[i])
            {
                setCoinsRet.insert(vValue[i].second);
                nValueRet += vValue[i].first;
            }

        printf("[selectcoins] SelectCoins() best subset: ");
        for (unsigned int i = 0; i < vValue.size(); i++)
            if (vfBest[i])
                printf("%s ", FormatMoney(vValue[i].first).c_str());
        printf("[selectcoins] total %s\n", FormatMoney(nBest).c_str());
    }

    return true;
    }
    return false;
}

bool CWallet::SelectCoins(int64_t nTargetValue, unsigned int nSpendTime, set<pair<const CWalletTx*,unsigned int> >& setCoinsRet, int64_t& nValueRet, const CCoinControl* coinControl) const
{
    vector<COutput> vCoins;
    AvailableCoins(vCoins, true, coinControl);
    txlat::Mark("coinspool", (int64_t)vCoins.size());

    // coin control -> return all selected outputs (we want all selected to go into the transaction for sure)
    if (coinControl && coinControl->HasSelected())
    {
        BOOST_FOREACH(const COutput& out, vCoins)
        {
            nValueRet += out.tx->vout[out.i].nValue;
            setCoinsRet.insert(make_pair(out.tx, out.i));
        }
        return (nValueRet >= nTargetValue);
    }

    return (SelectCoinsMinConf(nTargetValue, nSpendTime, 1, 10, vCoins, setCoinsRet, nValueRet) ||
            SelectCoinsMinConf(nTargetValue, nSpendTime, 1, 1, vCoins, setCoinsRet, nValueRet) ||
            SelectCoinsMinConf(nTargetValue, nSpendTime, 0, 1, vCoins, setCoinsRet, nValueRet));
}

bool CWallet::SelectCoins2(int64_t nTargetValue, unsigned int nSpendTime, set<pair<const CWalletTx*,unsigned int> >& setCoinsRet, int64_t& nValueRet, const CCoinControl* coinControl) const
{
    vector<COutput> vCoins;
    AvailableCoins(vCoins, true, coinControl);

    // coin control -> return all selected outputs (we want all selected to go into the transaction for sure)
    if (coinControl && coinControl->HasSelected())
    {
        BOOST_FOREACH(const COutput& out, vCoins)
        {
            nValueRet += out.tx->vout[out.i].nValue;
            setCoinsRet.insert(make_pair(out.tx, out.i));
        }
        return (nValueRet >= nTargetValue);
    }

    return (SelectCoinsMinConf(nTargetValue, nSpendTime, 1, 10, vCoins, setCoinsRet, nValueRet) ||
            SelectCoinsMinConf(nTargetValue, nSpendTime, 1, 1, vCoins, setCoinsRet, nValueRet) ||
            SelectCoinsMinConf(nTargetValue, nSpendTime, 0, 1, vCoins, setCoinsRet, nValueRet));
}

// Select some coins without random shuffle or best subset approximation
bool CWallet::SelectCoinsForStaking(int64_t nTargetValue, unsigned int nSpendTime, set<pair<const CWalletTx*,unsigned int> >& setCoinsRet, int64_t& nValueRet) const
{
    LOCK2(cs_main, cs_wallet);

    vector<COutput> vCoins;
    if (!AvailableCoinsForStaking(vCoins, nSpendTime))
        return false;

    setCoinsRet.clear();
    nValueRet = 0;

    BOOST_FOREACH(COutput output, vCoins)
    {
        const CWalletTx *pcoin = output.tx;
        int i = output.i;

        // Stop if we've chosen enough inputs
        if (nValueRet >= nTargetValue)
            break;

        int64_t n = pcoin->vout[i].nValue;

        pair<int64_t,pair<const CWalletTx*,unsigned int> > coin = make_pair(n,make_pair(pcoin, i));

        if (n >= nTargetValue)
        {
            // If input value is greater or equal to target then simply insert
            //    it into the current subset and exit
            setCoinsRet.insert(coin.second);
            nValueRet += coin.first;
            break;
        }
        else if (n < nTargetValue + CENT)
        {
            setCoinsRet.insert(coin.second);
            nValueRet += coin.first;
        }
    }

    return true;
}

struct CompareByPriority
{
    bool operator()(const COutput& t1,
                    const COutput& t2) const
    {
        return t1.Priority() > t2.Priority();
    }
};

bool CWallet::SelectCoinsCollateral(std::vector<CTxIn>& setCoinsRet, int64_t& nValueRet) const
{
    vector<COutput> vCoins;

    //printf(" selecting coins for collateral\n");
    AvailableCoins(vCoins);

    //printf("found coins %d\n", (int)vCoins.size());

    set<pair<const CWalletTx*,unsigned int> > setCoinsRet2;

    BOOST_FOREACH(const COutput& out, vCoins)
    {
        // collateral inputs will always be a multiple of COLLATERALN_COLLATERAL, up to five
        if(IsCollateralAmount(out.tx->vout[out.i].nValue))
        {
            CTxIn vin = CTxIn(out.tx->GetHash(),out.i);

            vin.prevPubKey = out.tx->vout[out.i].scriptPubKey; // the inputs PubKey
            nValueRet += out.tx->vout[out.i].nValue;
            setCoinsRet.push_back(vin);
            setCoinsRet2.insert(make_pair(out.tx, out.i));
            return true;
        }
    }

    return false;
}

int CWallet::CountInputsWithAmount(int64_t nInputAmount)
{
    int64_t nTotal = 0;
    {
        LOCK(cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;
            if (pcoin->IsTrusted()){
                int nDepth = pcoin->GetDepthInMainChain();

                for (unsigned int i = 0; i < pcoin->vout.size(); i++) {
					//isminetype mine = IsMine(pcoin->vout[i]);
		    bool mine = IsMine(pcoin->vout[i]);
                    //COutput out = COutput(pcoin, i, nDepth, (mine & ISMINE_SPENDABLE) != ISMINE_NO);
		    COutput out = COutput(pcoin, i, nDepth, mine);
                    CTxIn vin = CTxIn(out.tx->GetHash(), out.i);

                    if(out.tx->vout[out.i].nValue != nInputAmount) continue;
                    if(!IsDenominatedAmount(pcoin->vout[i].nValue)) continue;
                    //if(IsSpent(out.tx->GetHash(), i) || !IsMine(pcoin->vout[i]) || !IsDenominated(vin)) continue;
		    if(pcoin->IsSpent(i) || !IsMine(pcoin->vout[i]) || !IsDenominated(vin)) continue;

                    nTotal++;
                }
            }
        }
    }

    return nTotal;
}

bool CWallet::HasCollateralInputs() const
{
    vector<COutput> vCoins;
    AvailableCoins(vCoins);

    int nFound = 0;
    BOOST_FOREACH(const COutput& out, vCoins)
        if(IsCollateralAmount(out.tx->vout[out.i].nValue)) nFound++;

    return nFound > 1; // should have more than one just in case
}

bool CWallet::IsCollateralAmount(int64_t nInputAmount) const
{
	return nInputAmount != 0 && nInputAmount % COLLATERALN_COLLATERAL == 0 && nInputAmount < COLLATERALN_COLLATERAL * 5 && nInputAmount > COLLATERALN_COLLATERAL;
}

bool CWallet::CreateCollateralTransaction(CTransaction& txCollateral, std::string strReason)
{
    /*
        To doublespend a collateral transaction, it will require a fee higher than this. So there's
        still a significant cost.
    */
    int64_t nFeeRet = 0.01*COIN;

    txCollateral.vin.clear();
    txCollateral.vout.clear();

    CReserveKey reservekey(this);
    int64_t nValueIn2 = 0;
    std::vector<CTxIn> vCoinsCollateral;

    if (!SelectCoinsCollateral(vCoinsCollateral, nValueIn2))
    {
        strReason = "Error: CollateralN requires a collateral transaction and could not locate an acceptable input!";
        return false;
    }

    // make our change address
    CScript scriptChange;
    CPubKey vchPubKey;

    if (!reservekey.GetReservedKey(vchPubKey))
    {
        strReason = "Error: Failed to get reserved key from keypool";
        return false;
    }
    scriptChange =GetScriptForDestination(vchPubKey.GetID());
    reservekey.KeepKey();

    BOOST_FOREACH(CTxIn v, vCoinsCollateral)
        txCollateral.vin.push_back(v);

    if(nValueIn2 - COLLATERALN_COLLATERAL - nFeeRet > 0) {
        //pay collateral charge in fees
        CTxOut vout3 = CTxOut(nValueIn2 - COLLATERALN_COLLATERAL, scriptChange);
        txCollateral.vout.push_back(vout3);
    }

    int vinNumber = 0;
    BOOST_FOREACH(CTxIn v, txCollateral.vin) {
        if(!SignSignature(*this, v.prevPubKey, txCollateral, vinNumber, int(SIGHASH_ALL|SIGHASH_ANYONECANPAY))) {
            BOOST_FOREACH(CTxIn v, vCoinsCollateral)
                UnlockCoin(v.prevout);

            strReason = "CCollaTeralPool::Sign - Unable to sign collateral transaction! \n";
            return false;
        }
        vinNumber++;
    }

    return true;
}

bool CWallet::ConvertList(std::vector<CTxIn> vCoins, std::vector<int64_t>& vecAmounts)
{
    BOOST_FOREACH(CTxIn i, vCoins){
        if (mapWallet.count(i.prevout.hash))
        {
            CWalletTx& wtx = mapWallet[i.prevout.hash];
            if(i.prevout.n < wtx.vout.size()){
                vecAmounts.push_back(wtx.vout[i.prevout.n].nValue);
            }
        } else {
            printf("ConvertList -- Couldn't find transaction\n");
        }
    }
    return true;
}

namespace
{
class CWalletTxReservationRollback
{
public:
    CWalletTxReservationRollback(CWallet& walletIn, CWalletTx& wtxIn,
                                 const std::string& failReasonIn, size_t nLockedBeforeIn)
        : wallet(walletIn), wtx(wtxIn), failReason(failReasonIn),
          nLockedBefore(nLockedBeforeIn), fRollback(true)
    {
        AssertLockHeld(wallet.cs_wallet);
    }

    ~CWalletTxReservationRollback()
    {
        if (!fRollback)
            return;

        AssertLockHeld(wallet.cs_wallet);
        const size_t nLockedByAttempt = wtx.vReservedCoins.size();
        const size_t nUnlocked = wallet.UnlockReservedCoins(wtx);
        printf("CreateTransaction: failure=\"%s\" locked_before=%" PRIszu
               " locked_by_attempt=%" PRIszu " rollback_unlocked=%" PRIszu
               " locked_after=%" PRIszu "\n",
               failReason.empty() ? "unknown" : failReason.c_str(), nLockedBefore,
               nLockedByAttempt, nUnlocked, wallet.setLockedCoins.size());
    }

    void KeepReservedCoins() { fRollback = false; }

private:
    CWallet& wallet;
    CWalletTx& wtx;
    const std::string& failReason;
    size_t nLockedBefore;
    bool fRollback;
};
}

bool CWallet::CreateTransaction(const vector<pair<CScript, int64_t> >& vecSend, CWalletTx& wtxNew, CReserveKey& reservekey, int64_t& nFeeRet, int32_t& nChangePos, const CCoinControl* coinControl, const CScript* scriptChangeOverride, std::string* strFailReasonOut)
{
    std::string strLocalFailReason;
    std::string& strFailReason = strFailReasonOut ? *strFailReasonOut : strLocalFailReason;
    strFailReason.clear();
    int64_t nValue = 0;
    BOOST_FOREACH (const PAIRTYPE(CScript, int64_t)& s, vecSend)
    {
        if (nValue < 0)
        {
            strFailReason = _("Transaction amounts must be positive");
            return false;
        }
        nValue += s.second;
    }
    if (vecSend.empty() || nValue < 0)
    {
        strFailReason = _("Transaction amounts must be positive");
        return false;
    }

    wtxNew.BindWallet(this);

    {
        LOCK2(cs_main, cs_wallet);
        // txdb must be opened before the mapWallet lock
        const size_t nLockedBefore = setLockedCoins.size();
        printf("CreateTransaction: locked_before=%" PRIszu "\n", nLockedBefore);
        CWalletTxReservationRollback reservationRollback(*this, wtxNew, strFailReason,
                                                          nLockedBefore);
        {
            nFeeRet = nTransactionFee;
            while (true)
            {
                wtxNew.vin.clear();
                wtxNew.vout.clear();
                wtxNew.fFromMe = true;

                const size_t nRetryUnlocked = UnlockReservedCoins(wtxNew);
                if (nRetryUnlocked > 0)
                    printf("CreateTransaction: fee retry rollback_unlocked=%" PRIszu
                           " locked_after=%" PRIszu "\n",
                           nRetryUnlocked, setLockedCoins.size());

                int64_t nTotalValue = nValue + nFeeRet;
                double dPriority = 0;

                // vouts to the payees with UTXO splitter - I n n o v a
                if(coinControl && !coinControl->fSplitBlock)
                {
                    BOOST_FOREACH (const PAIRTYPE(CScript, int64_t)& s, vecSend)
                    {
                        wtxNew.vout.push_back(CTxOut(s.second, s.first));
                    }
                }
                else //UTXO Splitter Transaction
                {
                    int nSplitBlock;
                    if(coinControl)
                        nSplitBlock = coinControl->nSplitBlock;
                    else
                        nSplitBlock = 1;

                    BOOST_FOREACH (const PAIRTYPE(CScript, int64_t)& s, vecSend)
                    {
                        for(int i = 0; i < nSplitBlock; i++)
                        {
                            if(i == nSplitBlock - 1)
                            {
                                uint64_t nRemainder = s.second % nSplitBlock;
                                wtxNew.vout.push_back(CTxOut((s.second / nSplitBlock) + nRemainder, s.first));
                            }
                            else
                                wtxNew.vout.push_back(CTxOut(s.second / nSplitBlock, s.first));
                        }
                    }
                }

                // Choose coins to use
                set<pair<const CWalletTx*,unsigned int> > setCoins;
                int64_t nValueIn = 0;
                if (!SelectCoins(nTotalValue, wtxNew.nTime, setCoins, nValueIn, coinControl))
                {
                    strFailReason = _("Insufficient funds");
                    return false;
                }
                txlat::Mark("selectcoins", (int64_t)setCoins.size());

                BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int) pcoin, setCoins)
                {
                    COutPoint outpt(pcoin.first->GetHash(), pcoin.second);
                    if (!IsLockedCoin(outpt.hash, outpt.n))
                    {
                        LockCoin(outpt);
                        wtxNew.vReservedCoins.push_back(outpt);
                    }
                }
                printf("CreateTransaction: locked_by_attempt=%" PRIszu
                       " locked_total=%" PRIszu "\n",
                       wtxNew.vReservedCoins.size(), setLockedCoins.size());

                BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int) pcoin, setCoins)
                {
                    //Fix priority calculation in CreateTransaction
                    //Make this projection of priority in 1 block match the
                    //calculation in the low priority reject code.
                    int64_t nCredit = pcoin.first->vout[pcoin.second].nValue;
                    //But mempool inputs might still be in the mempool, so their age stays 0
                    int age = pcoin.first->GetDepthInMainChain();
                    if (age != 0)
                        age += 1;
                    //dPriority += (double)nCredit * pcoin.first->GetDepthInMainChain();
                    dPriority += (double)nCredit * age;
                }

                if (nValueIn < nValue + nFeeRet)
                {
                    strFailReason = _("Insufficient funds for fee");
                    return false;
                }
                int64_t nChange = nValueIn - nValue - nFeeRet;
                // if sub-cent change is required, the fee must be raised to at least MIN_TX_FEE
                // or until nChange becomes zero
                // NOTE: this depends on the exact behaviour of GetMinFee
                if (nFeeRet < MIN_TX_FEE && nChange > 0 && nChange < CENT)
                {
                    int64_t nMoveToFee = min(nChange, MIN_TX_FEE - nFeeRet);
                    nChange -= nMoveToFee;
                    nFeeRet += nMoveToFee;
                }

                if (nChange > 0)
                {
                    CScript scriptChange;

                    if (scriptChangeOverride && !scriptChangeOverride->empty())
                        scriptChange = *scriptChangeOverride;
                    // coin control: send change to custom address
                    else if (coinControl && !boost::get<CNoDestination>(&coinControl->destChange))
                        scriptChange.SetDestination(coinControl->destChange);

                    // no coin control: send change to newly generated address
                    else
                    {
                        // Note: We use a new key here to keep it from being obvious which side is the change.
                        //  The drawback is that by not reusing a previous key, the change may be lost if a
                        //  backup is restored, if the backup doesn't have the new private key for the change.
                        //  If we reused the old key, it would be possible to add code to look for and
                        //  rediscover unknown transactions that were written with keys of ours to recover
                        //  post-backup change.

                        // Reserve a new key pair from key pool
                        CPubKey vchPubKey;
                    
                        if (!reservekey.GetReservedKey(vchPubKey))
                        {
                            strFailReason = _("Keypool ran out, please call keypoolrefill first");
                            return false;
                        }

                        scriptChange.SetDestination(vchPubKey.GetID());
                    }

                    if (wtxNew.vout.empty())
                    {
                        strFailReason = _("Transaction has no outputs");
                        return false;
                    }

                    // Insert change txn at random position:
                    vector<CTxOut>::iterator position = wtxNew.vout.begin()+GetRandInt(wtxNew.vout.size() + 1);

                    // -- don't put change output between value and narration outputs
                    if (position > wtxNew.vout.begin() && position < wtxNew.vout.end())
                    {
                        while (position > wtxNew.vout.begin())
                        {
                            if (position->nValue != 0)
                                break;
                            position--;
                        };
                    };
                    wtxNew.vout.insert(position, CTxOut(nChange, scriptChange));
                    nChangePos = std::distance(wtxNew.vout.begin(), position);
                }
                else
                    reservekey.ReturnKey();

                // Fill vin
                BOOST_FOREACH(const PAIRTYPE(const CWalletTx*,unsigned int)& coin, setCoins)
                    wtxNew.vin.push_back(CTxIn(coin.first->GetHash(),coin.second));

                // Sign
                int nIn = 0;
                BOOST_FOREACH(const PAIRTYPE(const CWalletTx*,unsigned int)& coin, setCoins)
                {
                    if (!SignSignature(*this, *coin.first, wtxNew, nIn++))
                    {
                        strFailReason = _("Signing transaction failed");
                        return false;
                    }
                }
                txlat::Mark("sign", nIn);

                // Limit size
                unsigned int nBytes = ::GetSerializeSize(*(CTransaction*)&wtxNew, SER_NETWORK, PROTOCOL_VERSION);
                txlat::Mark("txsize", (int64_t)nBytes, (int64_t)nIn);
                if (nBytes >= MAX_BLOCK_SIZE_GEN/5)
                {
                    strFailReason = _("Transaction too large");
                    return false;
                }
                dPriority /= nBytes;

                // Check that enough fee is included
                int64_t nPayFee = nTransactionFee * (1 + (int64_t)nBytes / 1000);
                int64_t nMinFee = wtxNew.GetMinFee(1, GMF_SEND, nBytes);

                if (nFeeRet < max(nPayFee, nMinFee))
                {
                    nFeeRet = max(nPayFee, nMinFee);
                    continue;
                }

                // Fill vtxPrev by copying from previous transactions vtxPrev
                wtxNew.AddSupportingTransactions();
                wtxNew.fTimeReceivedIsTxTime = true;

                break;
            }
            reservationRollback.KeepReservedCoins();
            printf("CreateTransaction: success locked_by_attempt=%" PRIszu
                   " locked_total=%" PRIszu "\n",
                   wtxNew.vReservedCoins.size(), setLockedCoins.size());
        }
    }
    return true;
}


bool CWallet::CreateTransaction(CScript scriptPubKey, int64_t nValue, std::string& sNarr, CWalletTx& wtxNew, CReserveKey& reservekey, int64_t& nFeeRet, const CCoinControl* coinControl, const CScript* scriptChangeOverride)
{
    vector< pair<CScript, int64_t> > vecSend;
    vecSend.push_back(make_pair(scriptPubKey, nValue));

    if (sNarr.length() > 0)
    {
        std::vector<uint8_t> vNarr(sNarr.c_str(), sNarr.c_str() + sNarr.length());
        std::vector<uint8_t> vNDesc;

        vNDesc.resize(2);
        vNDesc[0] = 'n';
        vNDesc[1] = 'p';

        CScript scriptN = CScript() << OP_RETURN << vNDesc << OP_RETURN << vNarr;

        vecSend.push_back(make_pair(scriptN, 0));
    }

    // -- CreateTransaction won't place change between value and narr output.
    //    narration output will be for preceding output

    int nChangePos;

    //bool rv = CreateTransaction(vecSend, wtxNew, reservekey, nFeeRet, nChangePos, strFailReason, coinControl);
    bool rv = CreateTransaction(vecSend, wtxNew, reservekey, nFeeRet, nChangePos, coinControl, scriptChangeOverride);

    // -- narration will be added to mapValue later in FindStealthTransactions From CommitTransaction
    return rv;
}

bool CWallet::NewStealthAddress(std::string& sError, std::string& sLabel, CStealthAddress& sxAddr)
{
    ec_secret scan_secret;
    ec_secret spend_secret;

    if (GenerateRandomSecret(scan_secret) != 0
        || GenerateRandomSecret(spend_secret) != 0)
    {
        sError = "GenerateRandomSecret failed.";
        printf("Error CWallet::NewStealthAddress - %s\n", sError.c_str());
        return false;
    };

    ec_point scan_pubkey, spend_pubkey;
    if (SecretToPublicKey(scan_secret, scan_pubkey) != 0)
    {
        sError = "Could not get scan public key.";
        printf("Error CWallet::NewStealthAddress - %s\n", sError.c_str());
        return false;
    };

    if (SecretToPublicKey(spend_secret, spend_pubkey) != 0)
    {
        sError = "Could not get spend public key.";
        printf("Error CWallet::NewStealthAddress - %s\n", sError.c_str());
        return false;
    };

    if (fDebug)
    {
        printf("getnewstealthaddress: new stealth address created\n");
    };


    sxAddr.label = sLabel;
    sxAddr.scan_pubkey = scan_pubkey;
    sxAddr.spend_pubkey = spend_pubkey;

    sxAddr.scan_secret.resize(32);
    memcpy(&sxAddr.scan_secret[0], &scan_secret.e[0], 32);
    sxAddr.spend_secret.resize(32);
    memcpy(&sxAddr.spend_secret[0], &spend_secret.e[0], 32);

    OPENSSL_cleanse(&scan_secret, sizeof(scan_secret));
    OPENSSL_cleanse(&spend_secret, sizeof(spend_secret));

    return true;
}

bool CWallet::AddStealthAddress(CStealthAddress& sxAddr)
{
    LOCK(cs_wallet);

    // must add before changing spend_secret
    stealthAddresses.insert(sxAddr);

    bool fOwned = sxAddr.scan_secret.size() == ec_secret_size;



    if (fOwned)
    {
        // -- owned addresses can only be added when wallet is unlocked
        if (IsLocked())
        {
            printf("Error: CWallet::AddStealthAddress wallet must be unlocked.\n");
            stealthAddresses.erase(sxAddr);
            return false;
        };

        if (IsCrypted())
        {
            std::vector<unsigned char> vchCryptedSecret;
            CSecret vchSecret;
            vchSecret.resize(32);
            if (sxAddr.spend_secret.size() != 32)
            {
                printf("AddStealthAddress() : stealth spend_secret wrong size %d\n", (int)sxAddr.spend_secret.size());
                stealthAddresses.erase(sxAddr);
                return false;
            }
            memcpy(&vchSecret[0], &sxAddr.spend_secret[0], 32);

            uint256 iv = Hash(sxAddr.spend_pubkey.begin(), sxAddr.spend_pubkey.end());
            if (!EncryptSecret(vMasterKey, vchSecret, iv, vchCryptedSecret))
            {
                printf("Error: Failed encrypting stealth key %s\n", sxAddr.Encoded().c_str());
                stealthAddresses.erase(sxAddr);
                return false;
            };
            sxAddr.spend_secret = vchCryptedSecret;
        };
    };


    bool rv = CWalletDB(strWalletFile).WriteStealthAddress(sxAddr);

    if (rv)
        NotifyAddressBookChanged(this, sxAddr, sxAddr.label, fOwned, CT_NEW);

    return rv;
}

bool CWallet::UnlockStealthAddresses(const CKeyingMaterial& vMasterKeyIn)
{
    // -- decrypt spend_secret of stealth addresses
    std::set<CStealthAddress>::iterator it;
    for (it = stealthAddresses.begin(); it != stealthAddresses.end(); ++it)
    {
        if (it->scan_secret.size() < EC_SECRET_SIZE)
            continue; // stealth address is not owned

        // -- CStealthAddress are only sorted on spend_pubkey
        CStealthAddress &sxAddr = const_cast<CStealthAddress&>(*it);

        if (fDebug)
            printf("Decrypting stealth key %s\n", sxAddr.Encoded().c_str());

        CSecret vchSecret;
        uint256 iv = Hash(sxAddr.spend_pubkey.begin(), sxAddr.spend_pubkey.end());
        if (!DecryptSecret(vMasterKeyIn, sxAddr.spend_secret, iv, vchSecret)
            || vchSecret.size() != EC_SECRET_SIZE)
        {
            printf("Error: Failed decrypting stealth key %s\n", sxAddr.Encoded().c_str());
            continue;
        };

        ec_secret testSecret;
        memcpy(&testSecret.e[0], &vchSecret[0], EC_SECRET_SIZE);
        ec_point pkSpendTest;

        if (SecretToPublicKey(testSecret, pkSpendTest) != 0
            || pkSpendTest != sxAddr.spend_pubkey)
        {
            printf("Error: Failed decrypting stealth key, public key mismatch %s\n", sxAddr.Encoded().c_str());
            OPENSSL_cleanse(&testSecret.e[0], EC_SECRET_SIZE);
            continue;
        };

        sxAddr.spend_secret.resize(EC_SECRET_SIZE);
        memcpy(&sxAddr.spend_secret[0], &vchSecret[0], EC_SECRET_SIZE);
        OPENSSL_cleanse(&testSecret.e[0], EC_SECRET_SIZE);
    };

    CryptedKeyMap::iterator mi = mapCryptedKeys.begin();
    for (; mi != mapCryptedKeys.end(); ++mi)
    {
        CPubKey &pubKey = (*mi).second.first;
        std::vector<unsigned char> &vchCryptedSecret = (*mi).second.second;
        if (vchCryptedSecret.size() != 0)
            continue;

        CKeyID ckid = pubKey.GetID();
        CBitcoinAddress addr(ckid);

        StealthKeyMetaMap::iterator mi = mapStealthKeyMeta.find(ckid);
        if (mi == mapStealthKeyMeta.end())
        {
            if (fDebug)
                printf("Warning: No metadata found to add secret for %s\n", addr.ToString().c_str());
            continue;
        };

        CStealthKeyMetadata& sxKeyMeta = mi->second;

        CStealthAddress sxFind;
        sxFind.SetScanPubKey(sxKeyMeta.pkScan);

        std::set<CStealthAddress>::iterator si = stealthAddresses.find(sxFind);
        if (si == stealthAddresses.end())
        {
            printf("No stealth key found to add secret for %s\n", addr.ToString().c_str());
            continue;
        };

        if (fDebug)
            printf("Expanding secret for %s\n", addr.ToString().c_str());

        ec_secret sSpendR;
        ec_secret sSpend;
        ec_secret sScan;

        if (si->spend_secret.size() != EC_SECRET_SIZE
            || si->scan_secret.size() != EC_SECRET_SIZE)
        {
            printf("Stealth address has no secret key for %s\n", addr.ToString().c_str());
            continue;
        };
        memcpy(&sScan.e[0], &si->scan_secret[0], EC_SECRET_SIZE);
        memcpy(&sSpend.e[0], &si->spend_secret[0], EC_SECRET_SIZE);

        ec_point pkEphem;;
        pkEphem.resize(sxKeyMeta.pkEphem.size());
        memcpy(&pkEphem[0], sxKeyMeta.pkEphem.begin(), sxKeyMeta.pkEphem.size());

        if (StealthSecretSpend(sScan, pkEphem, sSpend, sSpendR) != 0)
        {
            printf("StealthSecretSpend() failed.\n");
            OPENSSL_cleanse(&sScan.e[0], EC_SECRET_SIZE);
            OPENSSL_cleanse(&sSpend.e[0], EC_SECRET_SIZE);
            OPENSSL_cleanse(&sSpendR.e[0], EC_SECRET_SIZE);
            continue;
        };

        //CKey ckey;
        //ckey.Set(&sSpendR.e[0], true);

		CKey ckey;
		CSecret vchSecret;
		vchSecret.resize(ec_secret_size);

		ckey.Set(&sSpendR.e[0], &sSpendR.e[0] + ec_secret_size, true);

        if (!ckey.IsValid())
        {
            printf("Reconstructed key is invalid.\n");
            OPENSSL_cleanse(&sScan.e[0], EC_SECRET_SIZE);
            OPENSSL_cleanse(&sSpend.e[0], EC_SECRET_SIZE);
            OPENSSL_cleanse(&sSpendR.e[0], EC_SECRET_SIZE);
            continue;
        };

        CPubKey cpkT = ckey.GetPubKey();

        if (!cpkT.IsValid())
        {
            printf("%s: cpkT is invalid.\n", __func__);
            OPENSSL_cleanse(&sScan.e[0], EC_SECRET_SIZE);
            OPENSSL_cleanse(&sSpend.e[0], EC_SECRET_SIZE);
            OPENSSL_cleanse(&sSpendR.e[0], EC_SECRET_SIZE);
            continue;
        };

        if (cpkT != pubKey)
        {
            printf("%s: Error: Generated secret does not match.\n", __func__);
            if (fDebug)
            {
                printf("cpkT   %s\n", HexStr(cpkT).c_str());
                printf("pubKey %s\n", HexStr(pubKey).c_str());
            };
            OPENSSL_cleanse(&sScan.e[0], EC_SECRET_SIZE);
            OPENSSL_cleanse(&sSpend.e[0], EC_SECRET_SIZE);
            OPENSSL_cleanse(&sSpendR.e[0], EC_SECRET_SIZE);
            continue;
        };

        if (fDebug)
        {
            CKeyID keyID = cpkT.GetID();
            CBitcoinAddress coinAddress(keyID);
            printf("%s: Adding secret to key %s.\n", __func__, coinAddress.ToString().c_str());
        };

        if (!AddKeyPubKey(ckey, cpkT))
        {
            printf("%s: AddKeyPubKey failed.\n", __func__);
            OPENSSL_cleanse(&sScan.e[0], EC_SECRET_SIZE);
            OPENSSL_cleanse(&sSpend.e[0], EC_SECRET_SIZE);
            OPENSSL_cleanse(&sSpendR.e[0], EC_SECRET_SIZE);
            continue;
        };

        if (!CWalletDB(strWalletFile).EraseStealthKeyMeta(ckid))
            printf("EraseStealthKeyMeta failed for %s\n", addr.ToString().c_str());

        OPENSSL_cleanse(&sScan.e[0], EC_SECRET_SIZE);
        OPENSSL_cleanse(&sSpend.e[0], EC_SECRET_SIZE);
        OPENSSL_cleanse(&sSpendR.e[0], EC_SECRET_SIZE);
    };
    return true;
}

bool CWallet::UpdateStealthAddress(std::string &addr, std::string &label, bool addIfNotExist)
{
    if (fDebug)
        printf("UpdateStealthAddress %s\n", addr.c_str());


    CStealthAddress sxAddr;

    if (!sxAddr.SetEncoded(addr))
        return false;

    std::set<CStealthAddress>::iterator it;
    it = stealthAddresses.find(sxAddr);

    ChangeType nMode = CT_UPDATED;
    CStealthAddress sxFound;
    if (it == stealthAddresses.end())
    {
        if (addIfNotExist)
        {
            sxFound = sxAddr;
            sxFound.label = label;
            stealthAddresses.insert(sxFound);
            nMode = CT_NEW;
        } else
        {
            printf("UpdateStealthAddress %s, not in set\n", addr.c_str());
            return false;
        };
    } else
    {
        sxFound = const_cast<CStealthAddress&>(*it);

        if (sxFound.label == label)
        {
            // no change
            return true;
        };

        it->label = label; // update in .stealthAddresses

        if (sxFound.scan_secret.size() == ec_secret_size)
        {
            printf("UpdateStealthAddress: todo - update owned stealth address.\n");
            return false;
        };
    };

    sxFound.label = label;

    if (!CWalletDB(strWalletFile).WriteStealthAddress(sxFound))
    {
        printf("UpdateStealthAddress(%s) Write to db failed.\n", addr.c_str());
        return false;
    };

    bool fOwned = sxFound.scan_secret.size() == ec_secret_size;
    NotifyAddressBookChanged(this, sxFound, sxFound.label, fOwned, nMode);

    return true;
}


bool CWallet::FindStealthTransactions(const CTransaction& tx, mapValue_t& mapNarr)
{
    //if (fDebug)
        //printf("FindStealthTransactions() tx: %s\n", tx.GetHash().GetHex().c_str());

    mapNarr.clear();

    LOCK(cs_wallet);
    ec_secret sSpendR;
    ec_secret sSpend;
    ec_secret sScan;
    ec_secret sShared;

    ec_point pkExtracted;

    std::vector<uint8_t> vchEphemPK;
    std::vector<uint8_t> vchDataB;
    std::vector<uint8_t> vchENarr;
    opcodetype opCode;
    char cbuf[256];

    static const size_t MAX_STEALTH_SCAN_OUTPUTS = 500;
    int32_t nOutputIdOuter = -1;
    BOOST_FOREACH(const CTxOut& txout, tx.vout)
    {
        nOutputIdOuter++;
        if ((size_t)nOutputIdOuter >= MAX_STEALTH_SCAN_OUTPUTS)
        {
            printf("FindStealthTransactions: skipping remaining outputs (>%zu)\n", MAX_STEALTH_SCAN_OUTPUTS);
            break;
        }
        // -- for each OP_RETURN need to check all other valid outputs

        //printf("txout scriptPubKey %s\n",  txout.scriptPubKey.ToString().c_str());
        CScript::const_iterator itTxA = txout.scriptPubKey.begin();

        if (!txout.scriptPubKey.GetOp(itTxA, opCode, vchEphemPK)
            || opCode != OP_RETURN)
            continue;
        else
        if (!txout.scriptPubKey.GetOp(itTxA, opCode, vchEphemPK)
            || vchEphemPK.size() != 33)
        {
            // -- look for plaintext narrations
            if (vchEphemPK.size() > 1
                && vchEphemPK[0] == 'n'
                && vchEphemPK[1] == 'p')
            {
                if (txout.scriptPubKey.GetOp(itTxA, opCode, vchENarr)
                    && opCode == OP_RETURN
                    && txout.scriptPubKey.GetOp(itTxA, opCode, vchENarr)
                    && vchENarr.size() > 0)
                {
                    std::string sNarr = std::string(vchENarr.begin(), vchENarr.end());

                    snprintf(cbuf, sizeof(cbuf), "n_%d", nOutputIdOuter-1); // plaintext narration always matches preceding value output
                    mapNarr[cbuf] = sNarr;
                } else
                {
                    printf("Warning: FindStealthTransactions() tx: %s, Could not extract plaintext narration.\n", tx.GetHash().GetHex().c_str());
                };
            }

            continue;
        }

        int32_t nOutputId = -1;
        nStealth++;
        BOOST_FOREACH(const CTxOut& txoutB, tx.vout)
        {
            nOutputId++;

            if (&txoutB == &txout)
                continue;

            bool txnMatch = false; // only 1 txn will match an ephem pk
            //printf("txoutB scriptPubKey %s\n",  txoutB.scriptPubKey.ToString().c_str());

            CTxDestination address;
            if (!ExtractDestination(txoutB.scriptPubKey, address))
                continue;

            if (address.type() != typeid(CKeyID))
                continue;

            CKeyID ckidMatch = boost::get<CKeyID>(address);

            if (HaveKey(ckidMatch)) // no point checking if already have key
                continue;

            std::set<CStealthAddress>::iterator it;
            for (it = stealthAddresses.begin(); it != stealthAddresses.end(); ++it)
            {
                if (it->scan_secret.size() != ec_secret_size)
                    continue; // stealth address is not owned

                //printf("it->Encodeded() %s\n",  it->Encoded().c_str());
                memcpy(&sScan.e[0], &it->scan_secret[0], ec_secret_size);

                if (StealthSecret(sScan, vchEphemPK, it->spend_pubkey, sShared, pkExtracted) != 0)
                {
                    printf("StealthSecret failed.\n");
                    continue;
                };
                //printf("pkExtracted %" PRIszu": %s\n", pkExtracted.size(), HexStr(pkExtracted).c_str());

                CPubKey cpkE(pkExtracted);

                if (!cpkE.IsValid())
                    continue;
                CKeyID ckidE = cpkE.GetID();

                if (ckidMatch != ckidE)
                    continue;

                if (fDebug)
                    printf("Found stealth txn to address %s\n", it->Encoded().c_str());

                if (IsLocked())
                {
                    if (fDebug)
                        printf("Wallet is locked, adding key without secret.\n");

                    // -- add key without secret
                    std::vector<uint8_t> vchEmpty;
                    AddCryptedKey(cpkE, vchEmpty);
                    CKeyID keyId = cpkE.GetID();
                    CBitcoinAddress coinAddress(keyId);
                    std::string sLabel = it->Encoded();
                    SetAddressBookName(keyId, sLabel);

                    CPubKey cpkEphem(vchEphemPK);
                    CPubKey cpkScan(it->scan_pubkey);
                    CStealthKeyMetadata lockedSkMeta(cpkEphem, cpkScan);

                    if (!CWalletDB(strWalletFile).WriteStealthKeyMeta(keyId, lockedSkMeta))
                        printf("WriteStealthKeyMeta failed for %s\n", coinAddress.ToString().c_str());

                    mapStealthKeyMeta[keyId] = lockedSkMeta;
                    nFoundStealth++;
                } else
                {
                    if (it->spend_secret.size() != ec_secret_size)
                        continue;
                    memcpy(&sSpend.e[0], &it->spend_secret[0], ec_secret_size);


                    if (StealthSharedToSecretSpend(sShared, sSpend, sSpendR) != 0)
                    {
                        printf("StealthSharedToSecretSpend() failed.\n");
                        continue;
                    };

                    ec_point pkTestSpendR;
                    if (SecretToPublicKey(sSpendR, pkTestSpendR) != 0)
                    {
                        printf("SecretToPublicKey() failed.\n");
                        continue;
                    };

                    CSecret vchSecret;
                    vchSecret.resize(ec_secret_size);

                    memcpy(&vchSecret[0], &sSpendR.e[0], ec_secret_size);
                    CKey ckey;

                    try {
                        ckey.Set(vchSecret.begin(), vchSecret.end(), true);
                        //ckey.SetSecret(vchSecret, true);
                    } catch (std::exception& e) {
                        printf("ckey.SetSecret() threw: %s.\n", e.what());
                        continue;
                    };

                    CPubKey cpkT = ckey.GetPubKey();
                    if (!cpkT.IsValid())
                    {
                        printf("cpkT is invalid.\n");
                        continue;
                    };

                    if (!ckey.IsValid())
                    {
                        printf("Reconstructed key is invalid.\n");
                        continue;
                    };

                    CKeyID keyID = cpkT.GetID();
                    if (fDebug)
                    {
                        CBitcoinAddress coinAddress(keyID);
                        printf("Adding key %s.\n", coinAddress.ToString().c_str());
                    };

                    if (!AddKey(ckey))
                    {
                        printf("AddKey failed.\n");
                        continue;
                    };

                    std::string sLabel = it->Encoded();
                    SetAddressBookName(keyID, sLabel);
                    nFoundStealth++;
                };

                if (txout.scriptPubKey.GetOp(itTxA, opCode, vchENarr)
                    && opCode == OP_RETURN
                    && txout.scriptPubKey.GetOp(itTxA, opCode, vchENarr)
                    && vchENarr.size() > 0)
                {
                    SecMsgCrypter crypter;
                    crypter.SetKey(&sShared.e[0], &vchEphemPK[0]);
                    std::vector<uint8_t> vchNarr;
                    if (!crypter.Decrypt(&vchENarr[0], vchENarr.size(), vchNarr))
                    {
                        printf("Decrypt narration failed.\n");
                        continue;
                    };
                    std::string sNarr = std::string(vchNarr.begin(), vchNarr.end());

                    snprintf(cbuf, sizeof(cbuf), "n_%d", nOutputId);
                    mapNarr[cbuf] = sNarr;
                };

                txnMatch = true;
                break;
            };
            if (txnMatch)
                break;
        };
    };

    return true;
};



// NovaCoin: get current stake weight
bool CWallet::GetStakeWeight(const CKeyStore& keystore, uint64_t& nMinWeight, uint64_t& nMaxWeight, uint64_t& nWeight)
{
    const bool fRPCPerfTrace = GetBoolArg("-rpcperftrace", false);
    const int64_t nStartTime = RPCPerfTimeMicros(fRPCPerfTrace);
    const int64_t nBalanceStartTime = RPCPerfTimeMicros(fRPCPerfTrace);
    // Choose coins to use
    int64_t nBalance = nReserveBalance + 1;
    bool fBalanceOk = true;
    if (!fHybridSPV)
    {
        LOCK2(cs_main, cs_wallet);
        fBalanceOk = GetTransparentStakingBalance(*this, &nBalance);
    }
    const int64_t nBalanceCheckMicros = RPCPerfTimeMicros(fRPCPerfTrace) - nBalanceStartTime;

    if (!fBalanceOk || nBalance <= nReserveBalance)
    {
        if (fRPCPerfTrace)
            RPCPERF_LOG("RPCPERF rpc=GetStakeWeight reason=balance_not_above_reserve balance_check_us=%lld selection_us=0 selected_utxos=0 txindex_reads=0 txindex_read_total_us=0 txindex_read_max_us=0 total_us=%lld\n",
                        (long long)nBalanceCheckMicros,
                        (long long)(RPCPerfTimeMicros(true) - nStartTime));
        return false;
    }

    vector<const CWalletTx*> vwtxPrev;

    set<pair<const CWalletTx*,unsigned int> > setCoins;
    int64_t nValueIn = 0;
    const int64_t nSelectionStartTime = RPCPerfTimeMicros(fRPCPerfTrace);

    if (fHybridSPV)
    {
        if (!SelectCoinsForStakingSPV(setCoins))
        {
            if (fRPCPerfTrace)
                RPCPERF_LOG("RPCPERF rpc=GetStakeWeight reason=spv_selection_failed balance_check_us=%lld selection_us=%lld selected_utxos=0 txindex_reads=0 txindex_read_total_us=0 txindex_read_max_us=0 total_us=%lld\n",
                            (long long)nBalanceCheckMicros,
                            (long long)(RPCPerfTimeMicros(true) - nSelectionStartTime),
                            (long long)(RPCPerfTimeMicros(true) - nStartTime));
            return false;
        }
        for (const auto& pcoin : setCoins)
            nValueIn += pcoin.first->vout[pcoin.second].nValue;
    }
    else
    {
        if (!SelectCoinsForStaking(nBalance - nReserveBalance, GetTime(), setCoins, nValueIn))
        {
            if (fRPCPerfTrace)
                RPCPERF_LOG("RPCPERF rpc=GetStakeWeight reason=selection_failed balance_check_us=%lld selection_us=%lld selected_utxos=0 txindex_reads=0 txindex_read_total_us=0 txindex_read_max_us=0 total_us=%lld\n",
                            (long long)nBalanceCheckMicros,
                            (long long)(RPCPerfTimeMicros(true) - nSelectionStartTime),
                            (long long)(RPCPerfTimeMicros(true) - nStartTime));
            return false;
        }
    }
    const int64_t nSelectionMicros = RPCPerfTimeMicros(fRPCPerfTrace) - nSelectionStartTime;

    if (setCoins.empty())
    {
        if (fRPCPerfTrace)
            RPCPERF_LOG("RPCPERF rpc=GetStakeWeight reason=no_candidates balance_check_us=%lld selection_us=%lld selected_utxos=0 txindex_reads=0 txindex_read_total_us=0 txindex_read_max_us=0 total_us=%lld\n",
                        (long long)nBalanceCheckMicros,
                        (long long)nSelectionMicros,
                        (long long)(RPCPerfTimeMicros(true) - nStartTime));
        return false;
    }
    if (fHybridSPV && nValueIn <= nReserveBalance)
    {
        if (fRPCPerfTrace)
            RPCPERF_LOG("RPCPERF rpc=GetStakeWeight reason=spv_balance_not_above_reserve balance_check_us=%lld selection_us=%lld selected_utxos=%zu txindex_reads=0 txindex_read_total_us=0 txindex_read_max_us=0 total_us=%lld\n",
                        (long long)nBalanceCheckMicros,
                        (long long)nSelectionMicros,
                        setCoins.size(),
                        (long long)(RPCPerfTimeMicros(true) - nStartTime));
        return false;
    }

    nMinWeight = nMaxWeight = nWeight = 0;
    uint64_t nTxIndexReads = 0;
    int64_t nTxIndexReadTotalMicros = 0;
    int64_t nTxIndexReadMaxMicros = 0;

    BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int) pcoin, setCoins)
    {
        CTxIndex txindex;
        {
            LOCK2(cs_main, cs_wallet);
            const int64_t nReadStartTime = RPCPerfTimeMicros(fRPCPerfTrace);
            const bool fRead = GetStakingTxIndex(
                *this, *pcoin.first, &txindex);
            if (fRPCPerfTrace)
            {
                const int64_t nReadMicros = RPCPerfTimeMicros(true) - nReadStartTime;
                nTxIndexReads++;
                nTxIndexReadTotalMicros += nReadMicros;
                nTxIndexReadMaxMicros = std::max(nTxIndexReadMaxMicros, nReadMicros);
            }
            if (!fRead)
                continue;
        }

        if ((int64_t)pcoin.first->nTime > GetTime())
            continue;
        int64_t nTimeWeight = GetWeight((int64_t)pcoin.first->nTime, (int64_t)GetTime());
        CBigNum bnCoinDayWeight = CBigNum(pcoin.first->vout[pcoin.second].nValue) * nTimeWeight / COIN / (24 * 60 * 60);

        // Weight is greater than zero
        if (nTimeWeight > 0)
        {
            nWeight += bnCoinDayWeight.getuint64();
        }

        // Weight is greater than zero, but the maximum value isn't reached yet
        if (nTimeWeight > 0 && nTimeWeight < nStakeMaxAge)
        {
            nMinWeight += bnCoinDayWeight.getuint64();
        }

        // Maximum weight was reached
        if (nTimeWeight == nStakeMaxAge)
        {
            nMaxWeight += bnCoinDayWeight.getuint64();
        }
    }

    if (fRPCPerfTrace)
        RPCPERF_LOG("RPCPERF rpc=GetStakeWeight balance_check_us=%lld selection_us=%lld selected_utxos=%llu txindex_reads=%llu txindex_read_total_us=%lld txindex_read_max_us=%lld total_us=%lld\n",
                    (long long)nBalanceCheckMicros,
                    (long long)nSelectionMicros,
                    (unsigned long long)setCoins.size(),
                    (unsigned long long)nTxIndexReads,
                    (long long)nTxIndexReadTotalMicros,
                    (long long)nTxIndexReadMaxMicros,
                    (long long)(RPCPerfTimeMicros(true) - nStartTime));
    return true;
}

bool CWallet::CreateCoinStake(const CKeyStore& keystore, unsigned int nBits, int64_t nSearchInterval, int64_t nFees, CTransaction& txNew, CKey& key)
{
    CBlockIndex* pindexPrev = pindexBest;

    CBigNum bnTargetPerCoinDay;
    bnTargetPerCoinDay.SetCompact(nBits);

    txNew.vin.clear();
    txNew.vout.clear();

    // Mark coin stake transaction
    CScript scriptEmpty;
    scriptEmpty.clear();
    txNew.vout.push_back(CTxOut(0, scriptEmpty));

    // Choose coins to use
    int64_t nBalance = nReserveBalance + 1;
    if (!fHybridSPV)
    {
        LOCK2(cs_main, cs_wallet);
        if (!GetTransparentStakingBalance(*this, &nBalance))
            return false;
    }

    if (nBalance <= nReserveBalance)
        return false;

    vector<const CWalletTx*> vwtxPrev;

    set<pair<const CWalletTx*,unsigned int> > setCoins;
    int64_t nValueIn = 0;
    int64_t nCredit = 0;

    // Cache staking mode (UI thread may change it concurrently)
    StakingMode eStakingMode;
    {
        LOCK(cs_stakingMode);
        eStakingMode = nStakingMode;
    }

    bool fTryTransparent = (eStakingMode == STAKE_TRANSPARENT || eStakingMode == STAKE_COLD);

    if (fTryTransparent)
    {
    if (fHybridSPV)
    {
        if (!SelectCoinsForStakingSPV(setCoins))
        {
            if (fDebug && GetBoolArg("-printcoinstakedebug"))
                printf("CreateCoinStake() : SPV staking coins not found\n");
        }
        for (const auto& pcoin : setCoins)
            nValueIn += pcoin.first->vout[pcoin.second].nValue;
    }
    else
    {
        if (!SelectCoinsForStaking(nBalance - nReserveBalance, txNew.nTime, setCoins, nValueIn))
        {
            if (fDebug && GetBoolArg("-printcoinstakedebug"))
                printf("CreateCoinStake() : valid staking coins not found\n");
        }
    }
    } // end fTryTransparent coin selection

    if (fHybridSPV && nValueIn <= nReserveBalance)
        return false;

    CScript scriptPubKeyKernel;
    int nMaxStakeSearchInterval = 10;

    if (fTryTransparent && !setCoins.empty())
    BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int) pcoin, setCoins)
    {
        {
            LOCK(cs_main);
            if (pcoin.first->hashBlock != 0) {
                map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(pcoin.first->hashBlock);
                if (mi != mapBlockIndex.end() && mi->second) {
                    if (mi->second->nTime + nStakeMinAge > txNew.nTime - nMaxStakeSearchInterval)
                        continue;
                }
            }
        }

        if (eStakingMode == STAKE_COLD)
        {
            if (!IsPayToColdStaking(pcoin.first->vout[pcoin.second].scriptPubKey))
                continue;
        }

        CTxIndex txindex;
        {
            LOCK2(cs_main, cs_wallet);
            if (!GetStakingTxIndex(*this, *pcoin.first, &txindex))
                continue;
        }

        // Read block header
        CBlock block;
        {
            LOCK2(cs_main, cs_wallet);
            if (!block.ReadFromDisk(txindex.pos.nFile, txindex.pos.nBlockPos, false))
                continue;
        }

        if (block.GetBlockTime() + nStakeMinAge > txNew.nTime - nMaxStakeSearchInterval)
            continue; // only count coins meeting min age requirement

        bool fKernelFound = false;
        for (unsigned int n=0; n<min(nSearchInterval,(int64_t)nMaxStakeSearchInterval) && !fKernelFound && !fShutdown && pindexPrev == pindexBest; n++)
        {
            if (fDebug && GetBoolArg("-printcoinstakedebug"))
                printf("CreateCoinStake() : searching backward in time from %ld for %d seconds to %d\n",txNew.nTime,nSearchInterval,nMaxStakeSearchInterval);
            // Search backward in time from the given txNew timestamp
            // Search nSearchInterval seconds back up to nMaxStakeSearchInterval
            uint256 hashProofOfStake = 0, targetProofOfStake = 0;
            COutPoint prevoutStake = COutPoint(pcoin.first->GetHash(), pcoin.second);
            if (CheckStakeKernelHash(pindexPrev, nBits, block, txindex.pos.nTxPos - txindex.pos.nBlockPos, *pcoin.first, prevoutStake, txNew.nTime - n, hashProofOfStake, targetProofOfStake))
            {
                // Found a kernel
                if (fDebug && GetBoolArg("-printcoinstake"))
                    printf("CreateCoinStake() : kernel found\n");
                vector<valtype> vSolutions;
                txnouttype whichType;
                CScript scriptPubKeyOut;
                scriptPubKeyKernel = pcoin.first->vout[pcoin.second].scriptPubKey;
                if (!Solver(scriptPubKeyKernel, whichType, vSolutions))
                {
                    if (fDebug && GetBoolArg("-printcoinstake"))
                        printf("CreateCoinStake() : failed to parse kernel\n");
                    break;
                }
                if (fDebug && GetBoolArg("-printcoinstake"))
                    printf("CreateCoinStake() : parsed kernel type=%d\n", whichType);
                if (whichType != TX_PUBKEY && whichType != TX_PUBKEYHASH && whichType != TX_COLDSTAKE)
                {
                    printf("CreateCoinStake() : no support for kernel type=%d\n", whichType);
                    break;
                }
                if (whichType == TX_COLDSTAKE)
                {
                    if (vSolutions.size() < 2)
                    {
                        if (fDebug && GetBoolArg("-printcoinstake"))
                            printf("CreateCoinStake() : cold stake missing owner key in vSolutions\n");
                        break;
                    }

                    if (vSolutions[0].size() != 20 || vSolutions[1].size() != 20)
                    {
                        if (fDebug && GetBoolArg("-printcoinstake"))
                            printf("CreateCoinStake() : cold stake vSolutions wrong size (staker=%zu, owner=%zu)\n",
                                   vSolutions[0].size(), vSolutions[1].size());
                        break;
                    }
                    CKeyID stakerKeyID = CKeyID(uint160(vSolutions[0]));
                    CKeyID ownerKeyID = CKeyID(uint160(vSolutions[1]));

                    CKeyID nullKeyID;  // default constructor = all zeros
                    if (stakerKeyID == nullKeyID || ownerKeyID == nullKeyID)
                    {
                        if (fDebug && GetBoolArg("-printcoinstake"))
                            printf("CreateCoinStake() : cold stake has null key (staker=%s, owner=%s)\n",
                                   (stakerKeyID == nullKeyID) ? "null" : "ok", (ownerKeyID == nullKeyID) ? "null" : "ok");
                        break;
                    }

                    if (stakerKeyID == ownerKeyID)
                    {
                        if (fDebug && GetBoolArg("-printcoinstake"))
                            printf("CreateCoinStake() : cold stake staker and owner keys are identical\n");
                        break;
                    }

                    if (!keystore.GetKey(stakerKeyID, key))
                    {
                        if (fDebug && GetBoolArg("-printcoinstake"))
                            printf("CreateCoinStake() : failed to get staker key for cold stake\n");
                        break;
                    }

                    CScript regeneratedScript = GetScriptForColdStaking(stakerKeyID, ownerKeyID);
                    if (regeneratedScript != scriptPubKeyKernel)
                    {
                        if (fDebug && GetBoolArg("-printcoinstake"))
                            printf("CreateCoinStake() : regenerated cold stake script does not match original\n");
                        break;
                    }

                    scriptPubKeyOut = scriptPubKeyKernel;
                }
                else if (whichType == TX_PUBKEYHASH)
                {
                    // convert to pay to public key type
                    if (!keystore.GetKey(uint160(vSolutions[0]), key))
                    {
                        if (fDebug && GetBoolArg("-printcoinstake"))
                            printf("CreateCoinStake() : failed to get key for kernel type=%d\n", whichType);
                        break;  // unable to find corresponding public key
                    }
                    scriptPubKeyOut << key.GetPubKey() << OP_CHECKSIG;
                }
                else if (whichType == TX_PUBKEY)
                {
                    valtype& vchPubKey = vSolutions[0];
                    if (!keystore.GetKey(Hash160(vchPubKey), key))
                    {
                        if (fDebug && GetBoolArg("-printcoinstake"))
                            printf("CreateCoinStake() : failed to get key for kernel type=%d\n", whichType);
                        break;  // unable to find corresponding public key
                    }

                if (key.GetPubKey() != vchPubKey)
                {
                    if (fDebug && GetBoolArg("-printcoinstake"))
                        printf("CreateCoinStake() : invalid key for kernel type=%d\n", whichType);
                        break; // keys mismatch
                    }

                    scriptPubKeyOut = scriptPubKeyKernel;
                }

                txNew.nTime -= n;
                txNew.vin.push_back(CTxIn(pcoin.first->GetHash(), pcoin.second));
                nCredit += pcoin.first->vout[pcoin.second].nValue;
                vwtxPrev.push_back(pcoin.first);
                txNew.vout.push_back(CTxOut(0, scriptPubKeyOut));

                if (GetWeight(block.GetBlockTime(), (int64_t)txNew.nTime) < nStakeSplitAge && nCredit > nStakeMinSplitThreshold)
                    txNew.vout.push_back(CTxOut(0, scriptPubKeyOut));
                if (fDebug && GetBoolArg("-printcoinstake"))
                    printf("CreateCoinStake() : added kernel type=%d\n", whichType);
                fKernelFound = true;
                break;
            }
        }

        if (fKernelFound || fShutdown)
            break; // if kernel is found stop searching
    }


    if (nCredit == 0 || nCredit > nBalance - nReserveBalance)
    {
        return false;
    }

    BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int) pcoin, setCoins)
    {
        // Attempt to add more inputs
        // Only add coins of the same key/address as kernel
        if (txNew.vout.size() == 2 && ((pcoin.first->vout[pcoin.second].scriptPubKey == scriptPubKeyKernel || pcoin.first->vout[pcoin.second].scriptPubKey == txNew.vout[1].scriptPubKey))
            && pcoin.first->GetHash() != txNew.vin[0].prevout.hash)
        {
            int64_t nTimeWeight = GetWeight((int64_t)pcoin.first->nTime, (int64_t)txNew.nTime);

            // Stop adding more inputs if already too many inputs
            if (txNew.vin.size() >= 100)
                break;
            // Stop adding more inputs if value is already pretty significant
            if (nCredit >= nStakeCombineThreshold)
                break;
            // Stop adding inputs if reached reserve limit
            if (nCredit + pcoin.first->vout[pcoin.second].nValue > nBalance - nReserveBalance)
                break;
            // Do not add additional significant input
            if (pcoin.first->vout[pcoin.second].nValue >= nStakeCombineThreshold)
                continue;
            // Do not add input that is still too young
            if (nTimeWeight < nStakeMinAge)
                continue;

            txNew.vin.push_back(CTxIn(pcoin.first->GetHash(), pcoin.second));
            nCredit += pcoin.first->vout[pcoin.second].nValue;
            vwtxPrev.push_back(pcoin.first);
        }
    }

    // Calculate coin age reward
    int64_t nReward;
    {
        uint64_t nCoinAge;
        if (!txNew.GetCoinAge(nCoinAge))
            return error("CreateCoinStake() : failed to calculate coin age");

        nReward = GetProofOfStakeReward(nCoinAge, nFees);
        if (nReward <= 0)
            return false;

        nCredit += nReward;
    }

	// Collateralnode Payments
    int payments = 1;
    // start collateralnode payments
    bool bCollateralNodePayment = false;

    if (fTestNet) {
        if (pindexPrev->nHeight+1 > BLOCK_START_COLLATERALNODE_PAYMENTS_TESTNET ) {
            bCollateralNodePayment = true;
        }
    } else {
        if (pindexPrev->nHeight+1 > BLOCK_START_COLLATERALNODE_PAYMENTS && pindexPrev->nHeight+1 > 2085000){
            bCollateralNodePayment = true;
        }
    }
    if(fDebug) { printf("CreateCoinStake() : Collateralnode Payments = %i!\n", bCollateralNodePayment); }

    CScript payee;
    bool hasPayment = false;
    if(bCollateralNodePayment) {
        if(!collateralnodePayments.GetBlockPayee(pindexPrev->nHeight+1, payee)){
            int winningNode = GetCollateralnodeByRank(1);
                if(winningNode >= 0){
                    BOOST_FOREACH(PAIRTYPE(int, CCollateralNode*)& s, vecCollateralnodeScores)
                    {
                        if (s.first == winningNode)
                        {
                            payee.SetDestination(s.second->pubkey.GetID());
                            break;
                        }
                    }
                } else {
                    if(fDebug) { printf("CreateCoinStake() : Failed to detect collateralnode to pay\n"); }
                    // collateralnodes are in-eligible for payment, burn the coins in-stead
                    std::string burnAddress;
                    if (fTestNet) burnAddress = "8TestXXXXXXXXXXXXXXXXXXXXXXXXbCvpq";
                    else burnAddress = "INNXXXXXXXXXXXXXXXXXXXXXXXXXZeeDTw";
                    CBitcoinAddress burnDestination;
                    burnDestination.SetString(burnAddress);
                    payee = GetScriptForDestination(burnDestination.Get());
                }
        }
        hasPayment = true; // Payment target resolved (CN, winner, or burn)
    }

    if(hasPayment){
        payments = txNew.vout.size() + 1;
        txNew.vout.resize(payments);

        txNew.vout[payments-1].scriptPubKey = payee;
        txNew.vout[payments-1].nValue = 0;

        CTxDestination address1;
        ExtractDestination(payee, address1);
        CBitcoinAddress address2(address1);

        if(fDebug) { printf("CreateCoinStake() : Collateralnode payment to %s\n", address2.ToString().c_str()); }
    }

    int64_t blockValue = nCredit;
    int64_t collateralnodePayment = GetCollateralnodePayment(pindexPrev->nHeight+1, nReward);


    // Set output amount
    if (!hasPayment && txNew.vout.size() == 3) // 2 stake outputs, stake was split, no collateralnode payment
    {
        if(fDebug) { printf("CreateCoinStake() : 2 stake outputs, No MN payment!\n"); }
        txNew.vout[1].nValue = (blockValue / 2 / CENT) * CENT;
        txNew.vout[2].nValue = blockValue - txNew.vout[1].nValue;
    }
    else if(hasPayment && txNew.vout.size() == 4) // 2 stake outputs, stake was split, plus a collateralnode payment
    {
        if(fDebug) { printf("CreateCoinStake() : 2 stake outputs, Split stake, with MN payment\n"); }
        txNew.vout[payments-1].nValue = collateralnodePayment;
        blockValue -= collateralnodePayment;
        txNew.vout[1].nValue = (blockValue / 2 / CENT) * CENT;
        txNew.vout[2].nValue = blockValue - txNew.vout[1].nValue;
    }
    else if(!hasPayment && txNew.vout.size() == 2) // only 1 stake output, was not split, no collateralnode payment
    {
        if(fDebug) { printf("CreateCoinStake() : 1 Stake output, No MN payment!\n"); }
        txNew.vout[1].nValue = blockValue;
    }
    else if(hasPayment && txNew.vout.size() == 3) // only 1 stake output, was not split, plus a collateralnode payment
    {
        if(fDebug) { printf("CreateCoinStake() : 1 stake output, With MN payment!\n"); }
        txNew.vout[payments-1].nValue = collateralnodePayment;
        blockValue -= collateralnodePayment;
        txNew.vout[1].nValue = blockValue;
    }

    // Sign
    int nIn = 0;
    BOOST_FOREACH(const CWalletTx* pcoin, vwtxPrev)
    {
        if (!SignSignature(*this, *pcoin, txNew, nIn++))
            return error("CreateCoinStake() : failed to sign coinstake");
    }

    // Limit size
    unsigned int nBytes = ::GetSerializeSize(txNew, SER_NETWORK, PROTOCOL_VERSION);
    if (nBytes >= MAX_BLOCK_SIZE_GEN/5)
        return error("CreateCoinStake() : exceeded coinstake size limit");

    // Successfully generated coinstake
    return true;
}


// Call after CreateTransaction unless you want to abort
bool CWallet::CommitTransaction(CWalletTx& wtxNew, CReserveKey& reservekey)
{

    if (!wtxNew.CheckTransaction())
    {
        printf("CommitTransaction: CheckTransaction() failed %s\n", wtxNew.GetHash().ToString().c_str());
        {
            LOCK(cs_wallet);
            for (std::vector<COutPoint>::iterator it = wtxNew.vReservedCoins.begin();
                 it != wtxNew.vReservedCoins.end(); ++it)
            {
                UnlockCoin(*it);
            }
            wtxNew.vReservedCoins.clear();
        }
        return false;
    };

    mapValue_t mapNarr;
    if (stealthAddresses.size() > 0 && !fDisableStealth) FindStealthTransactions(wtxNew, mapNarr);

    bool fIsMine = false;
    if (!mapNarr.empty())
    {
        BOOST_FOREACH(const PAIRTYPE(string,string)& item, mapNarr)
            wtxNew.mapValue[item.first] = item.second;
    };

    {
        LOCK2(cs_main, cs_wallet);
        printf("CommitTransaction:\n%s", wtxNew.ToString().c_str());
        {
            // This is only to keep the database open to defeat the auto-flush for the
            // duration of this scope.  This is the only place where this optimization
            // maybe makes sense; please don't do it anywhere else.
            CWalletDB* pwalletdb = fFileBacked ? new CWalletDB(strWalletFile,"r") : NULL;

            // Take key pair from key pool so it won't be used again
            reservekey.KeepKey();

            // Add tx to wallet, because if it has change it's also ours,
            // otherwise just for transaction history.
            AddToWallet(wtxNew);

            // Mark old coins as spent
            set<CWalletTx*> setCoins;
            BOOST_FOREACH(const CTxIn& txin, wtxNew.vin)
            {
                std::map<uint256, CWalletTx>::iterator mi = mapWallet.find(txin.prevout.hash);
                if (mi == mapWallet.end())
                    continue;
                CWalletTx &coin = mi->second;
                coin.BindWallet(this);
                coin.MarkSpent(txin.prevout.n);
                coin.WriteToDisk();
                NotifyTransactionChanged(this, coin.GetHash(), CT_UPDATED);
				vMintingWalletUpdated.push_back(coin.GetHash());
            }

            for (std::vector<COutPoint>::iterator it = wtxNew.vReservedCoins.begin();
                 it != wtxNew.vReservedCoins.end(); ++it)
            {
                UnlockCoin(*it);
            }
            wtxNew.vReservedCoins.clear();

            if (fFileBacked)
                delete pwalletdb;
        }
        txlat::Mark("persisted", (int64_t)wtxNew.vin.size());

        // Track how many getdata requests our transaction gets
        mapRequestCount[wtxNew.GetHash()] = 0;

        // Broadcast
        if (!wtxNew.AcceptToMemoryPool())
        {
            // This must not fail. The transaction has already been signed and recorded.
            printf("CommitTransaction() : Error: Transaction not valid\n");
            for (std::vector<COutPoint>::iterator it = wtxNew.vReservedCoins.begin();
                 it != wtxNew.vReservedCoins.end(); ++it)
            {
                UnlockCoin(*it);
            }
            wtxNew.vReservedCoins.clear();
            return false;
        }
        txlat::Mark("acceptmp");
        wtxNew.RelayWalletTransaction();
        txlat::Mark("relay");
    }
    return true;
}

string CWallet::SendMoney(CScript scriptPubKey, int64_t nValue, std::string& sNarr, CWalletTx& wtxNew, bool fAskFee)
{
    txlat::Begin("send");
    CReserveKey reservekey(this);
    int64_t nFeeRequired;

    if (IsLocked())
    {
        string strError = _("Error: Wallet locked, unable to create transaction  ");
        printf("SendMoney() : %s", strError.c_str());
        return strError;
    }
    if (fWalletUnlockStakingOnly)
    {
        string strError = _("Error: Wallet unlocked for staking only, unable to create transaction.");
        printf("SendMoney() : %s", strError.c_str());
        return strError;
    }
    if (!CreateTransaction(scriptPubKey, nValue, sNarr, wtxNew, reservekey, nFeeRequired))
    {
        string strError;
        if (nValue + nFeeRequired > GetBalance())
            strError = strprintf(_("Error: This transaction requires a transaction fee of at least %s because of its amount, complexity, or use of recently received funds  "), FormatMoney(nFeeRequired).c_str());
        else
            strError = _("Error: Transaction creation failed  ");
        printf("SendMoney() : %s", strError.c_str());
        return strError;
    }

    if (fAskFee && !uiInterface.ThreadSafeAskFee(nFeeRequired, _("Sending...")))
        return "ABORTED";

    txlat::Mark("create_done", nFeeRequired);
    if (!CommitTransaction(wtxNew, reservekey))
        return _("Error: The transaction was rejected.  This might happen if some of the coins in your wallet were already spent, such as if you used a copy of wallet.dat and coins were spent in the copy but not marked as spent here.");

    txlat::Mark("commit_done");
    return "";
}


string CWallet::SendMoneyToDestination(const CTxDestination& address, int64_t nValue, std::string& sNarr, CWalletTx& wtxNew, bool fAskFee)
{
    // Check amount
    if (nValue <= 0)
        return _("Invalid amount");
    if (nValue + nTransactionFee > GetBalance())
        return _("Insufficient funds");

    if (sNarr.length() > 24)
        return _("Narration must be 24 characters or less.");

    // Parse Bitcoin address
    CScript scriptPubKey;
    scriptPubKey.SetDestination(address);

    return SendMoney(scriptPubKey, nValue, sNarr, wtxNew, fAskFee);
}

int64_t CWallet::GetTotalValue(std::vector<CTxIn> vCoins) {
    int64_t nTotalValue = 0;
    CWalletTx wtx;
    BOOST_FOREACH(CTxIn i, vCoins){
        if (mapWallet.count(i.prevout.hash))
        {
            CWalletTx& wtx = mapWallet[i.prevout.hash];
            if(i.prevout.n < wtx.vout.size()){
                nTotalValue += wtx.vout[i.prevout.n].nValue;
            }
        } else {
            printf("GetTotalValue -- Couldn't find transaction\n");
        }
    }
    return nTotalValue;
}

DBErrors CWallet::LoadWallet(bool& fFirstRunRet)
{
    if (!fFileBacked)
        return DB_LOAD_OK;
    fFirstRunRet = false;
    DBErrors nLoadWalletRet = CWalletDB(strWalletFile,"cr+").LoadWallet(this);
    if (nLoadWalletRet == DB_NEED_REWRITE)
    {
        if (CDB::Rewrite(strWalletFile, "\x04pool"))
        {
            LOCK(cs_wallet);
            setKeyPool.clear();
            // Note: can't top-up keypool here, because wallet is locked.
            // User will be prompted to unlock wallet the next operation
            // the requires a new key.
        }
    }

    if (nLoadWalletRet != DB_LOAD_OK)
        return nLoadWalletRet;
    fFirstRunRet = !vchDefaultKey.IsValid();

    NewThread(ThreadFlushWalletDB, &strWalletFile);
    return DB_LOAD_OK;
}

DBErrors CWallet::ZapWalletTx()
{
    if (!fFileBacked)
        return DB_LOAD_OK;
    DBErrors nZapWalletTxRet = CWalletDB(strWalletFile,"cr+").ZapWalletTx(this);
    if (nZapWalletTxRet == DB_NEED_REWRITE)
    {
        if (CDB::Rewrite(strWalletFile, "\x04pool"))
        {
            LOCK(cs_wallet);
            setKeyPool.clear();
            // Note: can't top-up keypool here, because wallet is locked.
            // Users will be prompted to unlock wallet the next operation
            // that requires a new key.
        }
    }

    if (nZapWalletTxRet != DB_LOAD_OK)
        return nZapWalletTxRet;

    return DB_LOAD_OK;
}

//I n n o v a
bool CWallet::SetAddressBookName(const CTxDestination& address, const string& strName)
{
    bool fOwned;
    ChangeType nMode;
    {
        LOCK(cs_wallet); // mapAddressBook
        std::map<CTxDestination, std::string>::iterator mi = mapAddressBook.find(address);
        nMode = (mi == mapAddressBook.end()) ? CT_NEW : CT_UPDATED;
        fOwned = ::IsMine(*this, address);

        mapAddressBook[address] = strName;
    }

    if (fOwned)
    {
        const CBitcoinAddress& caddress = address;
    }
    NotifyAddressBookChanged(this, address, strName, fOwned, nMode);

    if (!fFileBacked)
        return false;
    return CWalletDB(strWalletFile).WriteName(CBitcoinAddress(address).ToString(), strName);
}

bool CWallet::DelAddressBookName(const CTxDestination& address)
{
    {
        LOCK(cs_wallet); // mapAddressBook

        mapAddressBook.erase(address);
    }

    bool fOwned = ::IsMine(*this, address);
    string sName = "";
    if (fOwned)
    {
        const CBitcoinAddress& caddress = address;
    }
    NotifyAddressBookChanged(this, address, "", fOwned, CT_DELETED);

    if (!fFileBacked)
        return false;
    return CWalletDB(strWalletFile).EraseName(CBitcoinAddress(address).ToString());
}

/*
void CWallet::PrintWallet(const CBlock& block)
{
    {
        LOCK(cs_wallet);
        if (block.IsProofOfWork() && mapWallet.count(block.vtx[0].GetHash()))
        {
            CWalletTx& wtx = mapWallet[block.vtx[0].GetHash()];
            printf("    mine:  %d  %d  %" PRId64"", wtx.GetDepthInMainChain(), wtx.GetBlocksToMaturity(), FormatMoney(wtx.GetCredit()).c_str());
        }
        if (block.IsProofOfStake() && mapWallet.count(block.vtx[1].GetHash()))
        {
            CWalletTx& wtx = mapWallet[block.vtx[1].GetHash()];
            printf("    stake: %d  %d  %" PRId64"", wtx.GetDepthInMainChain(), wtx.GetBlocksToMaturity(), FormatMoney(wtx.GetCredit()).c_str());
         }

    }
    printf("\n");
}
*/

bool CWallet::GetTransaction(const uint256 &hashTx, CWalletTx& wtx)
{
    {
        LOCK(cs_wallet);
        map<uint256, CWalletTx>::iterator mi = mapWallet.find(hashTx);
        if (mi != mapWallet.end())
        {
            wtx = (*mi).second;
            return true;
        }
    }
    return false;
}

bool CWallet::SetDefaultKey(const CPubKey &vchPubKey)
{
    if (fFileBacked)
    {
        if (!CWalletDB(strWalletFile).WriteDefaultKey(vchPubKey))
            return false;
    }
    vchDefaultKey = vchPubKey;
    return true;
}

bool GetWalletFile(CWallet* pwallet, string &strWalletFileOut)
{
    if (!pwallet->fFileBacked)
        return false;
    strWalletFileOut = pwallet->strWalletFile;
    return true;
}

//
// Mark old keypool keys as used,
// and generate all new keys
//
bool CWallet::NewKeyPool()
{
    {
        LOCK(cs_wallet);
        CWalletDB walletdb(strWalletFile);
        BOOST_FOREACH(int64_t nIndex, setKeyPool)
            walletdb.ErasePool(nIndex);
        setKeyPool.clear();

        if (IsLocked())
            return false;

		    int64_t nKeys;

        nKeys = max(GetArg("-keypool", 100), (int64_t)0);

        for (int i = 0; i < nKeys; i++)
        {
            int64_t nIndex = i+1;
            walletdb.WritePool(nIndex, CKeyPool(GenerateNewKey()));
            setKeyPool.insert(nIndex);
        }
        printf("CWallet::NewKeyPool wrote %" PRId64" new keys\n", nKeys);
    }
    return true;
}

bool CWallet::TopUpKeyPool(unsigned int nSize)
{
    {
        LOCK(cs_wallet);

        if (IsLocked())
            return false;

        CWalletDB walletdb(strWalletFile);

        // Top up key pool
        unsigned int nTargetSize;

        if (nSize > 0)
            nTargetSize = nSize;
        else
            nTargetSize = max(GetArg("-keypool", 100), (int64_t)0);

        while (setKeyPool.size() < (nTargetSize + 1))
        {
            int64_t nEnd = 1;
            if (!setKeyPool.empty())
                nEnd = *(--setKeyPool.end()) + 1;
            if (!walletdb.WritePool(nEnd, CKeyPool(GenerateNewKey())))
                throw runtime_error("TopUpKeyPool() : writing generated key failed");
            setKeyPool.insert(nEnd);
            printf("keypool added key %" PRId64", size=%" PRIszu"\n", nEnd, setKeyPool.size());

			if(!fSuccessfullyLoaded) {
			    double dProgress = nEnd / 10.f;
                std::string strMsg = strprintf(_("Loading Wallet... (Generating Keys: %3.2f %%)"), dProgress);
                uiInterface.InitMessage(strMsg);
			}
        }
    }
    return true;
}

void CWallet::ReserveKeyFromKeyPool(int64_t& nIndex, CKeyPool& keypool)
{
    nIndex = -1;
    keypool.vchPubKey = CPubKey();
    {
        LOCK(cs_wallet);

        if (!IsLocked())
            TopUpKeyPool();

        // Get the oldest key
        if(setKeyPool.empty())
            return;

        CWalletDB walletdb(strWalletFile);

        nIndex = *(setKeyPool.begin());
        setKeyPool.erase(setKeyPool.begin());
        if (!walletdb.ReadPool(nIndex, keypool))
            throw runtime_error("ReserveKeyFromKeyPool() : read failed");
        if (!HaveKey(keypool.vchPubKey.GetID()))
            throw runtime_error("ReserveKeyFromKeyPool() : unknown key in key pool");
    
        if (!keypool.vchPubKey.IsValid())
            throw runtime_error("ReserveKeyFromKeyPool() : invalid key in key pool");
        if (fDebug && GetBoolArg("-printkeypool"))
            printf("keypool reserve %" PRId64"\n", nIndex);
    }
}

int64_t CWallet::AddReserveKey(const CKeyPool& keypool)
{
    {
        LOCK2(cs_main, cs_wallet);
        CWalletDB walletdb(strWalletFile);

        int64_t nIndex = 1 + *(--setKeyPool.end());
        if (!walletdb.WritePool(nIndex, keypool))
            throw runtime_error("AddReserveKey() : writing added key failed");
        setKeyPool.insert(nIndex);
        return nIndex;
    }
    return -1;
}

void CWallet::KeepKey(int64_t nIndex)
{
    // Remove from key pool
    if (fFileBacked)
    {
        CWalletDB walletdb(strWalletFile);
        walletdb.ErasePool(nIndex);
    }
    if(fDebug)
        printf("keypool keep %" PRId64"\n", nIndex);
}

void CWallet::ReturnKey(int64_t nIndex)
{
    // Return to key pool
    {
        LOCK(cs_wallet);
        setKeyPool.insert(nIndex);
    }
    if(fDebug)
        printf("keypool return %" PRId64"\n", nIndex);
}

bool CWallet::GetKeyFromPool(CPubKey& result, bool fAllowReuse)
{
    int64_t nIndex = 0;
    CKeyPool keypool;
    {
        LOCK(cs_wallet);
        ReserveKeyFromKeyPool(nIndex, keypool);
        if (nIndex == -1)
        {
            if (fAllowReuse && vchDefaultKey.IsValid())
            {
                result = vchDefaultKey;
                return true;
            }
            if (IsLocked()) return false;
            result = GenerateNewKey();
            return true;
        }
        KeepKey(nIndex);
        result = keypool.vchPubKey;
    }
    return true;
}

int64_t CWallet::GetOldestKeyPoolTime()
{
    int64_t nIndex = 0;
    CKeyPool keypool;
    ReserveKeyFromKeyPool(nIndex, keypool);
    if (nIndex == -1)
        return GetTime();
    ReturnKey(nIndex);
    return keypool.nTime;
}

std::map<CTxDestination, int64_t> CWallet::GetAddressBalances()
{
    map<CTxDestination, int64_t> balances;

    {
        LOCK(cs_wallet);
        BOOST_FOREACH(PAIRTYPE(uint256, CWalletTx) walletEntry, mapWallet)
        {
            CWalletTx *pcoin = &walletEntry.second;

            if (!pcoin->IsFinal() || !pcoin->IsTrusted())
                continue;

            if ((pcoin->IsCoinBase() || pcoin->IsCoinStake()) && pcoin->GetBlocksToMaturity() > 0)
                continue;

            int nDepth = pcoin->GetDepthInMainChain();
            if (nDepth < (pcoin->IsFromMe(ISMINE_ALL) ? 0 : 1))
                continue;

            for (unsigned int i = 0; i < pcoin->vout.size(); i++)
            {
                CTxDestination addr;
                if (!IsMine(pcoin->vout[i]))
                    continue;
                if(!ExtractDestination(pcoin->vout[i].scriptPubKey, addr))
                    continue;

                int64_t n = pcoin->IsSpent(i) ? 0 : pcoin->vout[i].nValue;

                if (!balances.count(addr))
                    balances[addr] = 0;
                balances[addr] += n;
            }
        }
    }

    return balances;
}

set< set<CTxDestination> > CWallet::GetAddressGroupings()
{
    AssertLockHeld(cs_wallet); // mapWallet
    set< set<CTxDestination> > groupings;
    set<CTxDestination> grouping;

    BOOST_FOREACH(PAIRTYPE(uint256, CWalletTx) walletEntry, mapWallet)
    {
        CWalletTx *pcoin = &walletEntry.second;

        if (pcoin->vin.size() > 0)
        {
            bool any_mine = false;
            // group all input addresses with each other
            BOOST_FOREACH(CTxIn txin, pcoin->vin)
            {
                CTxDestination address;
                if (!IsMine(txin)) /* If this input isn't mine, ignore it */
                    continue;
                if(!ExtractDestination(mapWallet[txin.prevout.hash].vout[txin.prevout.n].scriptPubKey, address))
                    continue;
                grouping.insert(address);
                any_mine = true;
            }

            // group change with input addresses
            if (any_mine) {
            BOOST_FOREACH(CTxOut txout, pcoin->vout)
                if (IsChange(txout))
                {
                    CTxDestination txoutAddr;
                    if(!ExtractDestination(txout.scriptPubKey, txoutAddr))
                        continue;
                    grouping.insert(txoutAddr);
                }
            }
            if (grouping.size() > 0) {
                groupings.insert(grouping);
                grouping.clear();
            }
        }

        // group lone addrs by themselves
        for (unsigned int i = 0; i < pcoin->vout.size(); i++)
            if (IsMine(pcoin->vout[i]))
            {
                CTxDestination address;
                if(!ExtractDestination(pcoin->vout[i].scriptPubKey, address))
                    continue;
                grouping.insert(address);
                groupings.insert(grouping);
                grouping.clear();
            }
    }

    set< set<CTxDestination>* > uniqueGroupings; // a set of pointers to groups of addresses
    map< CTxDestination, set<CTxDestination>* > setmap;  // map addresses to the unique group containing it
    BOOST_FOREACH(set<CTxDestination> grouping, groupings)
    {
        // make a set of all the groups hit by this new group
        set< set<CTxDestination>* > hits;
        map< CTxDestination, set<CTxDestination>* >::iterator it;
        BOOST_FOREACH(CTxDestination address, grouping)
            if ((it = setmap.find(address)) != setmap.end())
                hits.insert((*it).second);

        // merge all hit groups into a new single group and delete old groups
        set<CTxDestination>* merged = new set<CTxDestination>(grouping);
        BOOST_FOREACH(set<CTxDestination>* hit, hits)
        {
            merged->insert(hit->begin(), hit->end());
            uniqueGroupings.erase(hit);
            delete hit;
        }
        uniqueGroupings.insert(merged);

        // update setmap
        BOOST_FOREACH(CTxDestination element, *merged)
            setmap[element] = merged;
    }

    set< set<CTxDestination> > ret;
    BOOST_FOREACH(set<CTxDestination>* uniqueGrouping, uniqueGroupings)
    {
        ret.insert(*uniqueGrouping);
        delete uniqueGrouping;
    }

    return ret;
}

// ppcoin: check 'spent' consistency between wallet and txindex
// ppcoin: fix wallet spent state according to txindex
void CWallet::FixSpentCoins(int& nMismatchFound, int64_t& nBalanceInQuestion, bool fCheckOnly)
{
    nMismatchFound = 0;
    nBalanceInQuestion = 0;

    LOCK(cs_wallet);
    vector<CWalletTx*> vCoins;
    vCoins.reserve(mapWallet.size());
    for (map<uint256, CWalletTx>::iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        vCoins.push_back(&(*it).second);

    BOOST_FOREACH(CWalletTx* pcoin, vCoins)
    {
        for (unsigned int n = 0; n < pcoin->vout.size(); n++)
        {
            if (!IsMine(pcoin->vout[n]))
                continue;
            // Store3A is the V2 spend authority: a coin present in the active
            // chain's UTXO set is unspent. If the authority is unavailable we do
            // not guess (FAIL CLOSED, no repair).
            bool unspent = false;
            if (s16::V2OutpointUnspent(pcoin->GetHash(), n, &unspent) != 1)
                continue;
            const bool fWalletSpent = pcoin->IsSpent(n);
            const bool fChainSpent = !unspent;
            if (fWalletSpent && !fChainSpent)
            {
                printf("FixSpentCoins found lost coin %s INN %s[%d], %s\n",
                    FormatMoney(pcoin->vout[n].nValue).c_str(), pcoin->GetHash().ToString().c_str(), n, fCheckOnly? "repair not attempted" : "repairing");
                nMismatchFound++;
                nBalanceInQuestion += pcoin->vout[n].nValue;
                if (!fCheckOnly)
                {
                    pcoin->MarkUnspent(n);
                    pcoin->WriteToDisk();
                }
            }
            else if (!fWalletSpent && fChainSpent)
            {
                printf("FixSpentCoins found spent coin %s INN %s[%d], %s\n",
                    FormatMoney(pcoin->vout[n].nValue).c_str(), pcoin->GetHash().ToString().c_str(), n, fCheckOnly? "repair not attempted" : "repairing");
                nMismatchFound++;
                nBalanceInQuestion += pcoin->vout[n].nValue;
                if (!fCheckOnly)
                {
                    pcoin->MarkSpent(n);
                    pcoin->WriteToDisk();
                }
            }
        }
    }
}

// ppcoin: disable transaction (only for coinstake)
void CWallet::DisableTransaction(const CTransaction &tx)
{
    if (!tx.IsCoinStake() || !IsFromMe(tx))
        return; // only disconnecting coinstake requires marking input unspent

    LOCK(cs_wallet);
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
    {
        map<uint256, CWalletTx>::iterator mi = mapWallet.find(txin.prevout.hash);
        if (mi != mapWallet.end())
        {
            CWalletTx& prev = (*mi).second;
            if (txin.prevout.n < prev.vout.size() && IsMine(prev.vout[txin.prevout.n]))
            {
                prev.MarkUnspent(txin.prevout.n);
                prev.WriteToDisk();
            }
        }
    }
}

bool CReserveKey::GetReservedKey(CPubKey& pubkey)
{
    if (nIndex == -1)
    {
        CKeyPool keypool;
        pwallet->ReserveKeyFromKeyPool(nIndex, keypool);
        if (nIndex != -1)
            vchPubKey = keypool.vchPubKey;
        else {
            if (pwallet->vchDefaultKey.IsValid()) {
                printf("CReserveKey::GetReservedKey(): Warning: Using default key instead of a new key, top up your keypool!");
                vchPubKey = pwallet->vchDefaultKey;
            } else
                return false;
        }
    }

    if (!vchPubKey.IsValid())
        return false;
    pubkey = vchPubKey;
    return true;
}

void CReserveKey::KeepKey()
{
    if (nIndex != -1)
        pwallet->KeepKey(nIndex);
    nIndex = -1;
    vchPubKey = CPubKey();
}

void CReserveKey::ReturnKey()
{
    if (nIndex != -1)
        pwallet->ReturnKey(nIndex);
    nIndex = -1;
    vchPubKey = CPubKey();
}

void CWallet::GetAllReserveKeys(set<CKeyID>& setAddress) const
{
    setAddress.clear();

    CWalletDB walletdb(strWalletFile);

    LOCK2(cs_main, cs_wallet);
    BOOST_FOREACH(const int64_t& id, setKeyPool)
    {
        CKeyPool keypool;
        if (!walletdb.ReadPool(id, keypool))
            throw runtime_error("GetAllReserveKeyHashes() : read failed");
    
        if (!keypool.vchPubKey.IsValid())
            throw runtime_error("GetAllReserveKeyHashes() : invalid key in key pool");
        CKeyID keyID = keypool.vchPubKey.GetID();
        if (!HaveKey(keyID))
            throw runtime_error("GetAllReserveKeyHashes() : unknown key in key pool");
        setAddress.insert(keyID);
    }
}

void CWallet::UpdatedTransaction(const uint256 &hashTx)
{
    {
        LOCK(cs_wallet);
        // Only notify UI if this transaction is in this wallet
        map<uint256, CWalletTx>::const_iterator mi = mapWallet.find(hashTx);
        if (mi != mapWallet.end())
            NotifyTransactionChanged(this, hashTx, CT_UPDATED);
			vMintingWalletUpdated.push_back(hashTx);
    }
}

void CWallet::GetKeyBirthTimes(std::map<CKeyID, int64_t> &mapKeyBirth) const {
    AssertLockHeld(cs_wallet); // mapKeyMetadata
    mapKeyBirth.clear();

    // get birth times for keys with metadata
    for (std::map<CKeyID, CKeyMetadata>::const_iterator it = mapKeyMetadata.begin(); it != mapKeyMetadata.end(); it++)
        if (it->second.nCreateTime)
            mapKeyBirth[it->first] = it->second.nCreateTime;

    // map in which we'll infer heights of other keys
    CBlockIndex *pindexMax = FindBlockByHeight(std::max(0, nBestHeight - 144)); // the tip can be reorganised; use a 144-block safety margin
    std::map<CKeyID, CBlockIndex*> mapKeyFirstBlock;
    std::set<CKeyID> setKeys;
    GetKeys(setKeys);
    BOOST_FOREACH(const CKeyID &keyid, setKeys) {
        if (mapKeyBirth.count(keyid) == 0)
            mapKeyFirstBlock[keyid] = pindexMax;
    }
    setKeys.clear();

    // if there are no such keys, we're done
    if (mapKeyFirstBlock.empty())
        return;

    // find first block that affects those keys, if there are any left
    std::vector<CKeyID> vAffected;
    for (std::map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); it++) {
        // iterate over all wallet transactions...
        const CWalletTx &wtx = (*it).second;
        std::map<uint256, CBlockIndex*>::const_iterator blit = mapBlockIndex.find(wtx.hashBlock);
        if (blit != mapBlockIndex.end() && blit->second->IsInMainChain()) {
            // ... which are already in a block
            int nHeight = blit->second->nHeight;
            BOOST_FOREACH(const CTxOut &txout, wtx.vout) {
                // iterate over all their outputs
                ::ExtractAffectedKeys(*this, txout.scriptPubKey, vAffected);
                BOOST_FOREACH(const CKeyID &keyid, vAffected) {
                    // ... and all their affected keys
                    std::map<CKeyID, CBlockIndex*>::iterator rit = mapKeyFirstBlock.find(keyid);
                    if (rit != mapKeyFirstBlock.end() && nHeight < rit->second->nHeight)
                        rit->second = blit->second;
                }
                vAffected.clear();
            }
        }
    }

    // Extract block timestamps for those keys
    for (std::map<CKeyID, CBlockIndex*>::const_iterator it = mapKeyFirstBlock.begin(); it != mapKeyFirstBlock.end(); it++)
        mapKeyBirth[it->first] = it->second->nTime - 7200; // block times can be 2h off
}

bool CWallet::AddAdrenalineNodeConfig(CAdrenalineNodeConfig nodeConfig)
{
    bool rv = CWalletDB(strWalletFile).WriteAdrenalineNodeConfig(nodeConfig.sAddress, nodeConfig);
    if(rv)
	uiInterface.NotifyAdrenalineNodeChanged(nodeConfig);

    return rv;
}


extern CWallet* pwalletMain;
void SendMoneyCheck(CAmount nValue)
{
    // Check amount
    if (nValue <= 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid amount");

    if (nValue > pwalletMain->GetBalance())
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds");

    string strError;
    if (pwalletMain->IsLocked())
    {
        strError = "Error: Wallet locked, unable to create transaction!";
        printf("SendMoney() : %s", strError.c_str());
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }

    if (fWalletUnlockStakingOnly)
    {
        string strError = ("Error: Wallet unlocked for block minting only, unable to create transaction.");
        printf("SendMoney() : %s", strError.c_str());
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }
}

static const int64_t MIN_COLD_STAKE_AMOUNT = 100 * COIN; // 100 min

bool CWallet::CreateColdStakeDelegation(const CKeyID& stakerKeyID, const CKeyID& ownerKeyID,
                                        int64_t nValue, CWalletTx& wtxNew, std::string& strError)
{
    if (nValue <= 0)
    {
        strError = "Invalid amount";
        return false;
    }

    if (nValue < MIN_COLD_STAKE_AMOUNT)
    {
        strError = strprintf("Minimum cold stake delegation is %s INN", FormatMoney(MIN_COLD_STAKE_AMOUNT).c_str());
        return false;
    }

    if (nValue + nTransactionFee > GetBalance())
    {
        strError = "Insufficient funds";
        return false;
    }

    CScript scriptColdStake = GetScriptForColdStaking(stakerKeyID, ownerKeyID);

    CReserveKey reservekey(this);
    int64_t nFeeRequired;
    int32_t nChangePos;

    vector<pair<CScript, int64_t> > vecSend;
    vecSend.push_back(make_pair(scriptColdStake, nValue));

    if (!CreateTransaction(vecSend, wtxNew, reservekey, nFeeRequired, nChangePos))
    {
        if (nValue + nFeeRequired > GetBalance())
            strError = strprintf("Error: This transaction requires a fee of at least %s", FormatMoney(nFeeRequired).c_str());
        else
            strError = "Transaction creation failed";
        return false;
    }

    if (!CommitTransaction(wtxNew, reservekey))
    {
        strError = "Error: The transaction was rejected";
        return false;
    }

    return true;
}

int64_t CWallet::GetColdStakingBalance() const
{
    int64_t nTotal = 0;
    {
        LOCK2(cs_main, cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;
            if (!pcoin->IsFinal() || !pcoin->IsTrusted())
                continue;

            for (unsigned int i = 0; i < pcoin->vout.size(); i++)
            {
                if (pcoin->IsSpent(i))
                    continue;
                if (IsPayToColdStaking(pcoin->vout[i].scriptPubKey))
                {
                    isminetype mine = IsMine(pcoin->vout[i]);
                    if (mine != MINE_NO)
                    {
                        if (pcoin->vout[i].nValue > 0 && nTotal <= MAX_MONEY - pcoin->vout[i].nValue)
                            nTotal += pcoin->vout[i].nValue;
                    }
                }
            }
        }
    }
    return nTotal;
}

bool CWallet::NeedsStakingPreparation() const
{
    StakingMode eMode;
    {
        LOCK(cs_stakingMode);
        eMode = nStakingMode;
    }
    if (eMode == STAKE_COLD)
        return (GetColdStakingBalance() == 0 && GetBalance() > 0);
    return false;
}

static const int64_t SPV_BLOCK_REQUEST_INTERVAL = 30;
static const size_t SPV_BLOCK_REQUESTS_PER_SELECTION = 3;
static size_t GetSPVUtxoCacheMaxSize()
{
    return (size_t)GetArg("-spvutxocachesize", 10000);
}

static void PruneStakingMaterializationsLocked(CWallet& wallet)
{
    AssertLockHeld(wallet.cs_spvutxos);
    const size_t limit = GetSPVUtxoCacheMaxSize();
    while (wallet.mapStakingMaterializations.size() > limit)
    {
        std::map<uint256, StakingMaterializationInfo>::iterator victim =
            wallet.mapStakingMaterializations.begin();
        for (std::map<uint256, StakingMaterializationInfo>::iterator it =
                 wallet.mapStakingMaterializations.begin();
             it != wallet.mapStakingMaterializations.end(); ++it)
            if (it->second.nAccessSequence < victim->second.nAccessSequence)
                victim = it;
        wallet.mapStakingMaterializations.erase(victim);
    }
}

bool CWallet::UpdateSPVUtxo(const COutPoint& outpoint, const SPVUtxo& utxo)
{
    AssertLockHeld(cs_main);
    SPVUtxo verifiedUtxo = utxo;

    if (!utxo.vMerkleBranch.empty())
    {
        verifiedUtxo.fVerified = verifiedUtxo.VerifyMerkleProof();
        if (!verifiedUtxo.fVerified)
        {
            printf("SPV: Rejected UTXO %s:%d - merkle proof verification failed\n",
                   outpoint.hash.ToString().c_str(), outpoint.n);
            return false;
        }
    }

    if (verifiedUtxo.fVerified)
    {
        ColdHotSeamSnapshot source;
        std::string error;
        const ColdHotSeamResult authority =
            GetStakingSourceAuthority(utxo.hashBlock, &source, &error);
        if (authority != COLD_HOT_SEAM_OK)
        {
            printf("SPV: Rejected UTXO %s:%d - source block has no current authority (%d)\n",
                   outpoint.hash.ToString().c_str(), outpoint.n, (int)authority);
            return false; // mutation-closed on every failed authority class
        }
    }

    LOCK(cs_spvutxos);
    mapSPVUtxos[outpoint] = verifiedUtxo;

    if (mapSPVUtxos.size() > GetSPVUtxoCacheMaxSize())
        PruneSPVUtxos();
    return true;
}

void CWallet::PublishStakingMaterialization(const uint256& hashBlock,
                                            unsigned int nFile,
                                            unsigned int nBlockPos)
{
    if (hashBlock == uint256(0) || nFile == 0)
        return;
    LOCK(cs_spvutxos);
    StakingMaterializationInfo& info = mapStakingMaterializations[hashBlock];
    info.available = true;
    info.pending = false;
    info.nFile = nFile;
    info.nBlockPos = nBlockPos;
    info.nAccessSequence = ++nStakingMaterializationSequence;

    // Availability is not authority and does not verify transaction inclusion.
    for (std::map<COutPoint, SPVUtxo>::iterator it = mapSPVUtxos.begin();
         it != mapSPVUtxos.end(); ++it)
        if (it->second.hashBlock == hashBlock)
            it->second.fHaveBlock = true;

    PruneStakingMaterializationsLocked(*this);
}

bool CWallet::GetStakingMaterialization(const uint256& hashBlock,
                                        StakingMaterializationInfo* out) const
{
    if (!out)
        return false;
    LOCK(cs_spvutxos);
    std::map<uint256, StakingMaterializationInfo>::const_iterator it =
        mapStakingMaterializations.find(hashBlock);
    if (it == mapStakingMaterializations.end())
    {
        *out = StakingMaterializationInfo();
        return false;
    }
    *out = it->second;
    return true;
}

bool CWallet::MarkStakingMaterializationRequested(const uint256& hashBlock,
                                                   int64_t nNow)
{
    if (hashBlock == uint256(0))
        return false;
    LOCK(cs_spvutxos);
    StakingMaterializationInfo& info = mapStakingMaterializations[hashBlock];
    if (info.available)
        return false;
    if (info.pending && nNow - info.nLastRequest < SPV_BLOCK_REQUEST_INTERVAL)
        return false;
    info.pending = true;
    info.nLastRequest = nNow;
    info.nRequestCount++;
    info.nAccessSequence = ++nStakingMaterializationSequence;

    PruneStakingMaterializationsLocked(*this);
    return mapStakingMaterializations.count(hashBlock) != 0;
}

void CWallet::InvalidateStakingMaterialization(const uint256& hashBlock)
{
    if (hashBlock == uint256(0))
        return;
    LOCK(cs_spvutxos);
    StakingMaterializationInfo& info = mapStakingMaterializations[hashBlock];
    info.available = false;
    info.pending = false;
    info.nFile = 0;
    info.nBlockPos = 0;
    info.nAccessSequence = ++nStakingMaterializationSequence;
    PruneStakingMaterializationsLocked(*this);
}

bool CWallet::IsStakingMaterializationPending(const uint256& hashBlock) const
{
    StakingMaterializationInfo info;
    return GetStakingMaterialization(hashBlock, &info) && info.pending;
}

size_t CWallet::GetStakingMaterializationCount() const
{
    LOCK(cs_spvutxos);
    return mapStakingMaterializations.size();
}

void CWallet::ClearStakingMaterializations()
{
    LOCK(cs_spvutxos);
    mapStakingMaterializations.clear();
    nStakingMaterializationSequence = 0;
}

void CWallet::RemoveSPVUtxo(const COutPoint& outpoint)
{
    LOCK(cs_spvutxos);
    mapSPVUtxos.erase(outpoint);
}

void CWallet::MarkSPVUtxoSpent(const COutPoint& outpoint)
{
    LOCK(cs_spvutxos);
    auto it = mapSPVUtxos.find(outpoint);
    if (it != mapSPVUtxos.end())
    {
        it->second.fSpent = true;
    }
}

bool CWallet::IsSPVUtxoSpent(const COutPoint& outpoint) const
{
    LOCK(cs_spvutxos);
    auto it = mapSPVUtxos.find(outpoint);
    if (it != mapSPVUtxos.end())
    {
        return it->second.fSpent;
    }
    return IsSpent(outpoint.hash, outpoint.n);
}

void CWallet::PruneSPVUtxos()
{
    size_t nMaxSize = GetSPVUtxoCacheMaxSize();

    std::vector<COutPoint> toRemove;
    for (const auto& item : mapSPVUtxos)
    {
        if (item.second.fSpent)
        {
            toRemove.push_back(item.first);
        }
    }
    for (const COutPoint& outpoint : toRemove)
    {
        mapSPVUtxos.erase(outpoint);
    }

    if (mapSPVUtxos.size() > nMaxSize)
    {
        std::vector<std::pair<int, COutPoint>> vecByPriority; // (priority, outpoint)
        for (const auto& item : mapSPVUtxos)
        {
            int nPriority = item.second.fVerified ? 1000000 : 0;
            nPriority += item.second.nHeight;
            vecByPriority.push_back(std::make_pair(nPriority, item.first));
        }
        std::sort(vecByPriority.begin(), vecByPriority.end());

        size_t nToEvict = mapSPVUtxos.size() - nMaxSize;
        for (size_t i = 0; i < nToEvict && i < vecByPriority.size(); i++)
        {
            mapSPVUtxos.erase(vecByPriority[i].second);
        }

        if (fDebug)
            printf("SPV: LRU-evicted %zu UTXOs from cache (limit: %zu)\n", nToEvict, nMaxSize);
    }

    if (!toRemove.empty() && fDebug)
    {
        printf("SPV: Pruned %zu spent UTXOs from cache\n", toRemove.size());
    }
}

void CWallet::PopulateSPVUtxosFromWallet()
{
    LOCK2(cs_main, cs_wallet);
    LOCK(cs_spvutxos);

    printf("SPV: Populating UTXO cache from wallet...\n");
    int nCount = 0;
    int nVerified = 0;

    std::map<uint256, CBlock> mapBlockCache;

    for (const auto& item : mapWallet)
    {
        const CWalletTx& wtx = item.second;
        const uint256& txhash = item.first;

        // A.9a.3f (family #4): the SPV-cache population must NOT require arbitrary
        // historical CBlockIndex residency. A deep-old active wallet coin whose
        // block object is absent from mapBlockIndex must still be admitted into the
        // SPV staking cache, otherwise AvailableCoinsForStakingSPV never offers it
        // and it can never be staked. Depth/maturity and the disk position are
        // resolved BY-VALUE through the retained navigator; only the pre-A.10
        // no-navigator fallback uses legacy mapBlockIndex residency.
        uint256 hashBlock = wtx.hashBlock;
        int nDepth = 0;
        int nBlocksToMaturity = 0;
        const ColdHotSeamResult sr = GetWalletTxStakingMaturity(wtx, &nDepth, &nBlocksToMaturity);
        if (sr == COLD_HOT_SEAM_AUTHORITY_FAILURE)
            continue; // cannot vouch for membership -> fail closed (skip cache entry)
        if (nDepth < 1 || nBlocksToMaturity > 0)
            continue;

        int nHeight = 0;
        unsigned int nFile = 0;
        unsigned int nBlockPos = 0;
        const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
        if (nav)
        {
            std::string err;
            ColdHotSeamSnapshot snap;
            if (nav->ResolveLogicalR(BlockIndexLogicalId(hashBlock), &snap, &err) == COLD_HOT_SEAM_OK)
            {
                nHeight = snap.snapshot.height;
                nFile = snap.snapshot.nFile;
                nBlockPos = snap.snapshot.nBlockPos;
            }
        }
        else
        {
            std::map<uint256, CBlockIndex*>::const_iterator mi = mapBlockIndex.find(hashBlock);
            if (mi != mapBlockIndex.end() && mi->second)
            {
                nHeight = mi->second->nHeight;
                nFile = mi->second->nFile;
                nBlockPos = mi->second->nBlockPos;
            }
        }

        for (unsigned int i = 0; i < wtx.vout.size(); i++)
        {
            if (wtx.IsSpent(i))
                continue;

            if (!IsMine(wtx.vout[i]))
                continue;

            if (IsLockedCoin(txhash, i))
                continue;

            COutPoint outpoint(txhash, i);

            SPVUtxo utxo;
            utxo.txhash = txhash;
            utxo.n = i;
            utxo.nValue = wtx.vout[i].nValue;
            utxo.nHeight = nHeight;
            utxo.hashBlock = hashBlock;
            utxo.nTime = wtx.nTime;
            utxo.scriptPubKey = wtx.vout[i].scriptPubKey;
            utxo.fHaveBlock = true;
            utxo.fSpent = false;
            utxo.fVerified = false;
            utxo.nLastBlockRequest = 0;
            utxo.nTxIndex = -1;

            if (nFile > 0)
            {
                if (mapBlockCache.find(hashBlock) == mapBlockCache.end())
                {
                    CBlock block;
                    if (block.ReadFromDisk(nFile, nBlockPos, true))
                    {
                        block.BuildMerkleTree();
                        mapBlockCache[hashBlock] = block;
                    }
                }

                if (mapBlockCache.count(hashBlock))
                {
                    CBlock& block = mapBlockCache[hashBlock];

                    int nTxIndex = -1;
                    for (size_t j = 0; j < block.vtx.size(); j++)
                    {
                        if (block.vtx[j].GetHash() == txhash)
                        {
                            nTxIndex = j;
                            break;
                        }
                    }

                    if (nTxIndex >= 0)
                    {
                        utxo.nTxIndex = nTxIndex;
                        utxo.hashMerkleRoot = block.hashMerkleRoot;
                        utxo.vMerkleBranch = block.GetMerkleBranch(nTxIndex);
                        utxo.fVerified = utxo.VerifyMerkleProof();
                        if (utxo.fVerified)
                            nVerified++;
                    }
                }
            }

            if (!utxo.fVerified)
            {
                utxo.fVerified = true;
            }

            mapSPVUtxos[outpoint] = utxo;
            nCount++;
        }
    }

    printf("SPV: Populated %d UTXOs into cache (%d with merkle proofs)\n", nCount, nVerified);
}

bool CWallet::SaveSPVUtxoCache()
{
    if (!fHybridSPV)
        return true;

    LOCK(cs_spvutxos);

    boost::filesystem::path pathCache = GetDataDir() / "spvutxos.dat";
    FILE* file = fopen(pathCache.string().c_str(), "wb");
    if (!file)
    {
        printf("SPV: Failed to open %s for writing\n", pathCache.string().c_str());
        return false;
    }

    CAutoFile fileout(file, SER_DISK, CLIENT_VERSION);

    try {
        fileout << (uint32_t)mapSPVUtxos.size();
        for (const auto& item : mapSPVUtxos)
        {
            fileout << item.first;
            fileout << item.second.txhash;
            fileout << item.second.n;
            fileout << item.second.nValue;
            fileout << item.second.nHeight;
            fileout << item.second.hashBlock;
            fileout << item.second.hashMerkleRoot;
            fileout << item.second.vMerkleBranch;
            fileout << item.second.nTxIndex;
            fileout << item.second.fHaveBlock;
            fileout << item.second.fSpent;
            fileout << item.second.fVerified;
            fileout << item.second.nTime;
            fileout << item.second.scriptPubKey;
        }
    }
    catch (std::exception& e) {
        printf("SPV: Error saving UTXO cache: %s\n", e.what());
        return false;
    }

    printf("SPV: Saved %zu UTXOs to cache file\n", mapSPVUtxos.size());
    return true;
}

bool CWallet::LoadSPVUtxoCache()
{
    if (!fHybridSPV)
        return true;

    // Materialization coordinates/pending state are intentionally ephemeral;
    // restart reloads only bounded SPV candidates, which may recover by hash.
    ClearStakingMaterializations();

    boost::filesystem::path pathCache = GetDataDir() / "spvutxos.dat";
    if (!boost::filesystem::exists(pathCache))
    {
        printf("SPV: No cached UTXO file found, will rebuild from wallet\n");
        return false;
    }

    FILE* file = fopen(pathCache.string().c_str(), "rb");
    if (!file)
        return false;

    CAutoFile filein(file, SER_DISK, CLIENT_VERSION);

    LOCK(cs_spvutxos);
    mapSPVUtxos.clear();

    try {
        uint32_t nCount;
        filein >> nCount;

        for (uint32_t i = 0; i < nCount; i++)
        {
            COutPoint outpoint;
            SPVUtxo utxo;

            filein >> outpoint;
            filein >> utxo.txhash;
            filein >> utxo.n;
            filein >> utxo.nValue;
            filein >> utxo.nHeight;
            filein >> utxo.hashBlock;
            filein >> utxo.hashMerkleRoot;
            filein >> utxo.vMerkleBranch;
            filein >> utxo.nTxIndex;
            filein >> utxo.fHaveBlock;
            filein >> utxo.fSpent;
            filein >> utxo.fVerified;
            filein >> utxo.nTime;
            filein >> utxo.scriptPubKey;

            utxo.nLastBlockRequest = 0;
            mapSPVUtxos[outpoint] = utxo;
        }
    }
    catch (std::exception& e) {
        printf("SPV: Error loading UTXO cache: %s\n", e.what());
        mapSPVUtxos.clear();
        return false;
    }

    printf("SPV: Loaded %zu UTXOs from cache file\n", mapSPVUtxos.size());
    return true;
}

void CWallet::AvailableCoinsForStakingSPV(std::vector<COutPoint>& vCoins) const
{
    vCoins.clear();
    unsigned int nSpendTime = GetAdjustedTime();

    LOCK(cs_spvutxos);
    for (const auto& item : mapSPVUtxos)
    {
        const SPVUtxo& utxo = item.second;

        if (utxo.fSpent)
            continue;

        if (utxo.nTime + nStakeMinAge > nSpendTime)
            continue;

        if (utxo.nValue < nMinimumInputValue)
            continue;

        // Both-false entries are recovery candidates, never selection
        // authority. SelectCoinsForStakingSPV performs typed authority first and
        // either materializes exact bytes or schedules bounded recovery.
        vCoins.push_back(item.first);
    }
}

void CWallet::MarkSPVBlockAvailable(const uint256& hashBlock)
{
    LOCK(cs_spvutxos);
    for (auto& item : mapSPVUtxos)
    {
        if (item.second.hashBlock == hashBlock)
            item.second.fHaveBlock = true;
    }
}

bool CWallet::RequestBlockForStaking(const uint256& hashBlock, bool fWait)
{
    const int64_t nNow = GetTime();
    if (!MarkStakingMaterializationRequested(hashBlock, nNow))
        return false;

    extern bool FetchBlockForStaking(const uint256& hashBlock);
    const bool fRequested = FetchBlockForStaking(hashBlock);

    if (fWait && fRequested)
    {
        const int64_t nWaitUntil = GetTime() + 10;
        while (GetTime() < nWaitUntil)
        {
            MilliSleep(100);
            StakingMaterializationInfo info;
            if (GetStakingMaterialization(hashBlock, &info) && info.available)
                return true;
        }
        return false;
    }

    return fRequested;
}

bool CWallet::SelectCoinsForStakingSPV(std::set<std::pair<const CWalletTx*,unsigned int> >& setCoinsRet) const
{
    std::vector<COutPoint> vSPVCoins;
    AvailableCoinsForStakingSPV(vSPVCoins);
    setCoinsRet.clear();

    LOCK2(cs_main, cs_wallet);
    std::set<uint256> blocksNeeded;

    for (const COutPoint& outpoint : vSPVCoins)
    {
        if (IsSpent(outpoint.hash, outpoint.n))
        {
            const_cast<CWallet*>(this)->MarkSPVUtxoSpent(outpoint);
            continue;
        }

        std::map<uint256, CWalletTx>::const_iterator it = mapWallet.find(outpoint.hash);
        if (it != mapWallet.end())
        {
            const CWalletTx* pcoin = &(it->second);
            if (outpoint.n >= pcoin->vout.size())
                continue;
            unsigned int nFile = 0, nBlockPos = 0;
            bool available = false;
            const ColdHotSeamResult authority = GetStakingSourceDiskPositionR(
                *this, pcoin->hashBlock, &nFile, &nBlockPos, &available);
            if (authority == COLD_HOT_SEAM_OK && available)
                setCoinsRet.insert(std::make_pair(pcoin, outpoint.n));
            else if (authority == COLD_HOT_SEAM_OK && pcoin->hashBlock != uint256(0))
                blocksNeeded.insert(pcoin->hashBlock);
            continue;
        }

        SPVUtxo cached;
        bool found = false;
        {
            LOCK(cs_spvutxos);
            std::map<COutPoint, SPVUtxo>::const_iterator spvIt = mapSPVUtxos.find(outpoint);
            if (spvIt != mapSPVUtxos.end())
            {
                cached = spvIt->second;
                found = true;
            }
        }
        if (!found || cached.hashBlock == uint256(0))
            continue;

        unsigned int nFile = 0, nBlockPos = 0;
        bool available = false;
        const ColdHotSeamResult authority = GetStakingSourceDiskPositionR(
            *this, cached.hashBlock, &nFile, &nBlockPos, &available);
        if (authority != COLD_HOT_SEAM_OK)
            continue; // materialization can never override source authority
        if (!available)
        {
            blocksNeeded.insert(cached.hashBlock);
            continue;
        }

        CBlock block;
        if (!block.ReadFromDisk(nFile, nBlockPos, true) ||
            block.GetHash() != cached.hashBlock)
        {
            CWallet* mutableWallet = const_cast<CWallet*>(this);
            mutableWallet->InvalidateStakingMaterialization(cached.hashBlock);
            blocksNeeded.insert(cached.hashBlock);
            continue;
        }

        // Exact active block bytes are sufficient materialization evidence for
        // a both-false persisted cache entry; authority was established above.
        const_cast<CWallet*>(this)->PublishStakingMaterialization(
            cached.hashBlock, nFile, nBlockPos);
        for (const CTransaction& tx : block.vtx)
        {
            if (tx.GetHash() != outpoint.hash)
                continue;
            CWalletTx wtx(const_cast<CWallet*>(this), tx);
            wtx.hashBlock = cached.hashBlock;
            wtx.nTime = cached.nTime;
            const_cast<CWallet*>(this)->AddToWallet(wtx);
            std::map<uint256, CWalletTx>::const_iterator newIt = mapWallet.find(outpoint.hash);
            if (newIt != mapWallet.end() && outpoint.n < newIt->second.vout.size())
                setCoinsRet.insert(std::make_pair(&(newIt->second), outpoint.n));
            break;
        }
    }

    size_t requestsIssued = 0;
    for (std::set<uint256>::const_iterator it = blocksNeeded.begin();
         it != blocksNeeded.end() &&
             requestsIssued < SPV_BLOCK_REQUESTS_PER_SELECTION; ++it)
    {
        if (const_cast<CWallet*>(this)->RequestBlockForStaking(*it, false))
            requestsIssued++;
    }

    return !setCoinsRet.empty();
}


void CWallet::ScanBlockForSilentPayments(const CBlock& block, int nHeight)
{
    // Deferred key imports (after cs_silentpayments release to preserve lock ordering)
    struct SPendingKeyImport {
        CKey key;
        CPubKey pubkey;
        uint32_t idx;
    };
    std::vector<SPendingKeyImport> vKeysToImport;

    // Lock ordering: cs_wallet before cs_silentpayments
    { // Scope for locks
    LOCK2(cs_wallet, cs_silentpayments);

    for (const CTransaction& tx : block.vtx)
    {
        if (!vSilentPaymentKeys.empty() && tx.vin.size() > 0)
        {
            std::vector<std::vector<unsigned char>> vInputPubKeys;
            for (const CTxIn& txin : tx.vin)
            {
                CTransaction prevTx;
                uint256 hashBlock = 0;
                if (::GetTransaction(txin.prevout.hash, prevTx, hashBlock))
                {
                    if (txin.prevout.n < prevTx.vout.size())
                    {
                        const CScript& scriptPubKey = prevTx.vout[txin.prevout.n].scriptPubKey;
                        if (txin.scriptSig.size() > 0)
                        {
                            CScript::const_iterator pc = txin.scriptSig.begin();
                            opcodetype opcode;
                            std::vector<unsigned char> vchData;
                            txin.scriptSig.GetOp(pc, opcode, vchData);
                            if (txin.scriptSig.GetOp(pc, opcode, vchData))
                            {
                                if (vchData.size() == 33)
                                {
                                    vInputPubKeys.push_back(vchData);
                                }
                                else if (vchData.size() == 65 && vchData[0] == 0x04)
                                {
                                    std::vector<unsigned char> vchCompressed(33);
                                    vchCompressed[0] = (vchData[64] & 1) ? 0x03 : 0x02;
                                    memcpy(&vchCompressed[1], &vchData[1], 32);
                                    vInputPubKeys.push_back(vchCompressed);
                                }
                            }
                        }
                    }
                }
            }

            if (!vInputPubKeys.empty())
            {
                std::vector<unsigned char> vchSenderPubKeySum;
                if (ComputeInputPubKeySum(vInputPubKeys, vchSenderPubKeySum))
                {
                    std::vector<std::vector<unsigned char>> vTxOutputPubKeys;
                    for (const CTxOut& txout : tx.vout)
                    {
                        CTxDestination dest;
                        if (ExtractDestination(txout.scriptPubKey, dest))
                        {
                            opcodetype opcode;
                            std::vector<unsigned char> vchPubKey;
                            CScript::const_iterator pc = txout.scriptPubKey.begin();
                            if (txout.scriptPubKey.GetOp(pc, opcode, vchPubKey) && vchPubKey.size() == 33)
                            {
                                vTxOutputPubKeys.push_back(vchPubKey);
                            }
                            else
                            {
                                vTxOutputPubKeys.push_back(std::vector<unsigned char>());
                            }
                        }
                        else
                        {
                            vTxOutputPubKeys.push_back(std::vector<unsigned char>());
                        }
                    }

                    for (const CSilentPaymentKey& spKey : vSilentPaymentKeys)
                    {
                        std::vector<uint32_t> vMatched;
                        if (ScanForSilentPayments(spKey, vchSenderPubKeySum, vTxOutputPubKeys, vMatched))
                        {
                            for (uint32_t idx : vMatched)
                            {
                                if (fDebug)
                                    printf("ScanBlockForSilentPayments() : found silent payment output idx=%u in tx %s at height=%d\n",
                                           idx, tx.GetHash().ToString().c_str(), nHeight);

                                std::vector<unsigned char> vchSpendPrivKey;
                                if (DeriveSilentPaymentSpendKey(spKey, vchSenderPubKeySum, idx, vchSpendPrivKey))
                                {
                                    CKey spendKey;
                                    spendKey.Set(vchSpendPrivKey.begin(), vchSpendPrivKey.end(), true);
                                    OPENSSL_cleanse(vchSpendPrivKey.data(), vchSpendPrivKey.size());

                                    if (spendKey.IsValid())
                                    {
                                        SPendingKeyImport imp;
                                        imp.key = spendKey;
                                        imp.pubkey = spendKey.GetPubKey();
                                        imp.idx = idx;
                                        vKeysToImport.push_back(imp);
                                    }
                                }
                                else
                                {
                                    printf("WARNING: ScanBlockForSilentPayments() : failed to derive silent payment spend key for idx=%u\n", idx);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    } // End cs_silentpayments scope

    // Import SP keys after releasing cs_silentpayments (lock ordering)
    for (const SPendingKeyImport& imp : vKeysToImport)
    {
        if (!HaveKey(imp.pubkey.GetID()))
        {
            AddKeyPubKey(imp.key, imp.pubkey);
            if (fDebug)
                printf("ScanBlockForSilentPayments() : imported silent payment spend key for output idx=%u\n", imp.idx);
        }
    }
}

bool CWallet::AddSilentPaymentKey(CSilentPaymentKey&& key)
{
    LOCK(cs_silentpayments);
    vSilentPaymentKeys.push_back(std::move(key));
    return true;
}

bool CWallet::GenerateNewSilentPaymentKey(CSilentPaymentAddress& addrOut)
{
    CSilentPaymentKey key;
    if (!CSilentPaymentKey::Generate(key))
        return false;
    if (!key.GetAddress(addrOut))
        return false;
    AddSilentPaymentKey(std::move(key));
    return true;
}
