// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "hreg_registration.h"

#include "checkpoints.h"
#include "script.h"
#include "txdb.h"

#include <map>
#include <set>
#include <vector>

namespace hreg {

namespace {

static const int64_t HREG_COLLATERAL = 25000LL * COIN;
static const unsigned char HREG_MAGIC_BYTES[] = {'I', 'N', 'C', 'N', 0x01, 0x01};
static int g_nHRegActivationOverride = -1;
static std::map<uint256, int> g_mapPrevTxHeightForTesting;

bool GetPrevTxHeight(const uint256& hashTx, int& nHeightOut)
{
    std::map<uint256, int>::const_iterator it = g_mapPrevTxHeightForTesting.find(hashTx);
    if (it != g_mapPrevTxHeightForTesting.end())
    {
        nHeightOut = it->second;
        return true;
    }

    CTransaction tx;
    uint256 hashBlock;
    if (!GetTransaction(hashTx, tx, hashBlock))
        return false;
    if (!mapBlockIndex.count(hashBlock) || mapBlockIndex[hashBlock] == NULL)
        return false;
    nHeightOut = mapBlockIndex[hashBlock]->nHeight;
    return true;
}

bool ExtractExactMarkerPayload(const CScript& scriptPubKey, std::vector<unsigned char>& payloadOut)
{
    payloadOut.clear();
    CScript::const_iterator pc = scriptPubKey.begin();
    opcodetype opcode;
    std::vector<unsigned char> data;
    if (!scriptPubKey.GetOp(pc, opcode, data))
        return false;
    if (opcode != OP_RETURN)
        return false;
    if (!scriptPubKey.GetOp(pc, opcode, data))
        return false;
    if (opcode > OP_PUSHDATA4)
        return false;
    payloadOut = data;
    if (pc != scriptPubKey.end())
        return false;
    return true;
}

bool IsHRegMarkerPayload(const std::vector<unsigned char>& payload)
{
    return payload.size() == sizeof(HREG_MAGIC_BYTES) &&
           memcmp(&payload[0], HREG_MAGIC_BYTES, sizeof(HREG_MAGIC_BYTES)) == 0;
}

bool FindUniqueQualifyingInput(const CTransaction& tx, const MapPrevTx& mapInputs,
                               COutPoint& outpointRet, CScript& payeeRet, bool& fFound,
                               bool& fDuplicate, bool& fMature)
{
    fFound = false;
    fDuplicate = false;
    fMature = false;
    int nFoundHeight = 0;
    for (unsigned int i = 0; i < tx.vin.size(); ++i)
    {
        const COutPoint& prevout = tx.vin[i].prevout;
        MapPrevTx::const_iterator mi = mapInputs.find(prevout.hash);
        if (mi == mapInputs.end())
            continue;
        const CTxIndex& txindex = (*mi).second.first;
        const CTransaction& txPrev = (*mi).second.second;
        if (prevout.n >= txPrev.vout.size() || prevout.n >= txindex.vSpent.size())
            continue;
        if (!txindex.vSpent[prevout.n].IsNull())
            continue;
        if (txPrev.IsCoinBase() || txPrev.IsCoinStake())
            continue;
        const CTxOut& prevTxOut = txPrev.vout[prevout.n];
        if (prevTxOut.nValue != HREG_COLLATERAL)
            continue;
        if (!IsStandardP2PKHScript(prevTxOut.scriptPubKey, NULL))
            continue;
        int nPrevHeight = 0;
        if (!GetPrevTxHeight(prevout.hash, nPrevHeight))
            continue;
        if (fFound)
        {
            fDuplicate = true;
            return false;
        }
        fFound = true;
        outpointRet = prevout;
        payeeRet = prevTxOut.scriptPubKey;
        nFoundHeight = nPrevHeight;
    }
    if (!fFound)
        return true;
    fMature = (nFoundHeight >= 0);
    return true;
}

unsigned int CountMatchingCollateralOutputs(const CTransaction& tx,
                                            const CScript& payee,
                                            std::vector<unsigned int>* pvMatchIndices)
{
    unsigned int nMatches = 0;
    if (pvMatchIndices)
        pvMatchIndices->clear();
    for (unsigned int i = 0; i < tx.vout.size(); ++i)
    {
        if (tx.vout[i].nValue == HREG_COLLATERAL && tx.vout[i].scriptPubKey == payee)
        {
            ++nMatches;
            if (pvMatchIndices)
                pvMatchIndices->push_back(i);
        }
    }
    return nMatches;
}

} // namespace

RegistryState g_registry;

bool IsHRegRecognitionActive(int nHeight)
{
    if (g_nHRegActivationOverride >= 0)
        return nHeight >= g_nHRegActivationOverride;
    int nFork = (::fRegTest || ::fTestNet) ? 1 : MAINNET_EXPERIMENTAL_V5_DISABLED_HEIGHT;
    return nHeight >= nFork;
}

void SetHRegActivationOverrideForTesting(int nHeight)
{
    g_nHRegActivationOverride = nHeight;
}

void ClearHRegActivationOverrideForTesting()
{
    g_nHRegActivationOverride = -1;
}

void SetHRegPrevTxHeightForTesting(const uint256& txid, int nHeight)
{
    g_mapPrevTxHeightForTesting[txid] = nHeight;
}

void ClearHRegPrevTxHeightsForTesting()
{
    g_mapPrevTxHeightForTesting.clear();
}

bool IsExactHRegMarkerScript(const CScript& scriptPubKey)
{
    std::vector<unsigned char> payload;
    if (!ExtractExactMarkerPayload(scriptPubKey, payload))
        return false;
    return IsHRegMarkerPayload(payload);
}

bool IsStandardP2PKHScript(const CScript& scriptPubKey, CKeyID* pKeyIdOut)
{
    txnouttype whichType;
    std::vector<std::vector<unsigned char> > vSolutions;
    if (!Solver(scriptPubKey, whichType, vSolutions))
        return false;
    if (whichType != TX_PUBKEYHASH || vSolutions.size() != 1 || vSolutions[0].size() != sizeof(uint160))
        return false;
    if (pKeyIdOut)
    {
        uint160 key = 0;
        memcpy(key.begin(), &vSolutions[0][0], sizeof(uint160));
        *pKeyIdOut = CKeyID(key);
    }
    return true;
}

RegistrationState ComputePreSpendState(int registrationHeight, int spendingBlockHeight)
{
    if (((spendingBlockHeight - 1) - registrationHeight) >= 15)
        return HREG_STATE_ELIGIBLE;
    return HREG_STATE_PENDING;
}



bool BuildReplayInputsForTx(CTxDB& txdb,
                            const CTransaction& tx,
                            const std::map<uint256, CTransaction>& mapLocalTx,
                            const std::set<COutPoint>& setLocalSpent,
                            MapPrevTx& mapInputs,
                            std::string& strError)
{
    mapInputs.clear();
    for (unsigned int i = 0; i < tx.vin.size(); ++i)
    {
        const COutPoint& prevout = tx.vin[i].prevout;
        if (mapInputs.count(prevout.hash))
            continue;
        std::map<uint256, CTransaction>::const_iterator itLocal = mapLocalTx.find(prevout.hash);
        if (itLocal != mapLocalTx.end())
        {
            CTxIndex txindex(CDiskTxPos(0,0,0), itLocal->second.vout.size());
            for (unsigned int n = 0; n < txindex.vSpent.size(); ++n)
            {
                if (setLocalSpent.count(COutPoint(prevout.hash, n)))
                    txindex.vSpent[n] = CDiskTxPos(9,9,9);
            }
            mapInputs[prevout.hash] = std::make_pair(txindex, itLocal->second);
            continue;
        }

        CTxIndex txindex;
        if (!txdb.ReadTxIndex(prevout.hash, txindex))
        {
            strError = strprintf("HREG rebuild: missing prev tx index %s", prevout.hash.ToString().c_str());
            return false;
        }
        CTransaction txPrev;
        if (txindex.pos == CDiskTxPos(1,1,1))
        {
            strError = strprintf("HREG rebuild: unexpected sentinel tx index for %s", prevout.hash.ToString().c_str());
            return false;
        }
        if (!txPrev.ReadFromDisk(txindex.pos))
        {
            strError = strprintf("HREG rebuild: ReadFromDisk failed for %s", prevout.hash.ToString().c_str());
            return false;
        }
        mapInputs[prevout.hash] = std::make_pair(txindex, txPrev);
    }
    return true;
}
RegistrationParseResult EvaluateHRegRegistrationTx(const CTransaction& tx,
                                                   const MapPrevTx& mapInputs,
                                                   int nBlockHeight)
{
    RegistrationParseResult result;
    if (!IsHRegRecognitionActive(nBlockHeight))
        return result;

    unsigned int nExactMarkers = 0;
    for (unsigned int i = 0; i < tx.vout.size(); ++i)
    {
        if (IsExactHRegMarkerScript(tx.vout[i].scriptPubKey))
            ++nExactMarkers;
    }
    if (nExactMarkers == 0)
        return result;
    if (nExactMarkers > 1)
    {
        result.action = RegistrationParseResult::ACTION_REJECT;
        result.reason = "multiple exact HREG markers";
        return result;
    }

    if (tx.IsCoinBase() || tx.IsCoinStake() || tx.IsShielded() || tx.nVersion == ANON_TXN_VERSION)
        return result;

    COutPoint oldCollateral;
    CScript payee;
    bool fFoundInput = false;
    bool fDuplicateInputs = false;
    bool fMature = false;
    if (!FindUniqueQualifyingInput(tx, mapInputs, oldCollateral, payee, fFoundInput, fDuplicateInputs, fMature))
    {
        result.action = RegistrationParseResult::ACTION_REJECT;
        result.reason = "duplicate qualifying collateral inputs";
        return result;
    }
    if (fDuplicateInputs)
    {
        result.action = RegistrationParseResult::ACTION_REJECT;
        result.reason = "multiple qualifying collateral inputs";
        return result;
    }
    if (!fFoundInput)
        return result;

    int nPrevHeight = 0;
    if (!GetPrevTxHeight(oldCollateral.hash, nPrevHeight))
        return result;
    if (((nBlockHeight - 1) - nPrevHeight) < 15)
        return result;

    std::vector<unsigned int> vMatchIndices;
    unsigned int nMatches = CountMatchingCollateralOutputs(tx, payee, &vMatchIndices);
    if (nMatches == 0)
        return result;
    if (nMatches > 1)
    {
        result.action = RegistrationParseResult::ACTION_REJECT;
        result.reason = "multiple recreated matching collateral outputs";
        return result;
    }

    result.action = RegistrationParseResult::ACTION_APPLY;
    result.oldCollateral = oldCollateral;
    result.newCollateral = COutPoint(tx.GetHash(), vMatchIndices[0]);
    result.paymentScript = payee;
    return result;
}

void RegistryState::Clear()
{
    m_states.clear();
}

bool RegistryState::Get(const COutPoint& outpoint, RegisteredCollateralState& out) const
{
    std::map<COutPoint, RegisteredCollateralState>::const_iterator it = m_states.find(outpoint);
    if (it == m_states.end())
        return false;
    out = it->second;
    return true;
}

bool RegistryState::Exists(const COutPoint& outpoint) const
{
    return m_states.count(outpoint) != 0;
}

std::vector<RegisteredCollateralState> RegistryState::Snapshot() const
{
    std::vector<RegisteredCollateralState> out;
    for (std::map<COutPoint, RegisteredCollateralState>::const_iterator it = m_states.begin(); it != m_states.end(); ++it)
        out.push_back(it->second);
    return out;
}

bool RegistryState::ApplyConnectedTx(const CTransaction& tx,
                                     const MapPrevTx& mapInputs,
                                     int nBlockHeight,
                                     std::string& strError)
{
    if (!IsHRegRecognitionActive(nBlockHeight))
        return true;

    RegistrationParseResult parsed = EvaluateHRegRegistrationTx(tx, mapInputs, nBlockHeight);
    if (parsed.action == RegistrationParseResult::ACTION_REJECT)
    {
        strError = parsed.reason;
        return false;
    }

    for (unsigned int i = 0; i < tx.vin.size(); ++i)
    {
        const COutPoint& prevout = tx.vin[i].prevout;
        std::map<COutPoint, RegisteredCollateralState>::iterator it = m_states.find(prevout);
        if (it != m_states.end() && it->second.state != HREG_STATE_SPENT)
            it->second.state = HREG_STATE_SPENT;
    }

    if (parsed.action == RegistrationParseResult::ACTION_APPLY)
    {
        RegisteredCollateralState st;
        st.outpoint = parsed.newCollateral;
        st.paymentScript = parsed.paymentScript;
        st.registrationHeight = nBlockHeight;
        st.state = HREG_STATE_PENDING;
        m_states[st.outpoint] = st;
    }
    return true;
}

bool RegistryState::DisconnectTx(const CTransaction& tx,
                                 const MapPrevTx& mapInputs,
                                 int nBlockHeight,
                                 std::string& strError)
{
    if (!IsHRegRecognitionActive(nBlockHeight))
        return true;

    RegistrationParseResult parsed = EvaluateHRegRegistrationTx(tx, mapInputs, nBlockHeight);
    if (parsed.action == RegistrationParseResult::ACTION_REJECT)
    {
        strError = parsed.reason;
        return false;
    }

    if (parsed.action == RegistrationParseResult::ACTION_APPLY)
    {
        m_states.erase(parsed.newCollateral);
    }

    for (unsigned int i = 0; i < tx.vin.size(); ++i)
    {
        const COutPoint& prevout = tx.vin[i].prevout;
        std::map<COutPoint, RegisteredCollateralState>::iterator it = m_states.find(prevout);
        if (it != m_states.end() && it->second.state == HREG_STATE_SPENT)
            it->second.state = ComputePreSpendState(it->second.registrationHeight, nBlockHeight);
    }
    return true;
}

void ClearHRegStateForTesting()
{
    g_registry.Clear();
    ClearHRegPrevTxHeightsForTesting();
}

std::vector<RegisteredCollateralState> GetHRegStateSnapshotForTesting()
{
    return g_registry.Snapshot();
}

bool ApplyHRegConnectedTx(const CTransaction& tx,
                          const MapPrevTx& mapInputs,
                          int nBlockHeight,
                          std::string& strError)
{
    return g_registry.ApplyConnectedTx(tx, mapInputs, nBlockHeight, strError);
}

bool DisconnectHRegTx(const CTransaction& tx,
                      const MapPrevTx& mapInputs,
                      int nBlockHeight,
                      std::string& strError)
{
    return g_registry.DisconnectTx(tx, mapInputs, nBlockHeight, strError);
}

bool RebuildHRegStateFromActiveChain(std::string& strError)
{
    g_registry.Clear();
    if (pindexBest == NULL || pindexGenesisBlock == NULL)
        return true;
    if (!IsHRegRecognitionActive(nBestHeight))
        return true;

    CTxDB txdb("r");
    for (CBlockIndex* pindex = pindexGenesisBlock; pindex; pindex = pindex->pnext)
    {
        if (pindex->nHeight < 0)
            continue;
        if (!IsHRegRecognitionActive(pindex->nHeight))
            continue;
        CBlock block;
        if (!block.ReadFromDisk(pindex))
        {
            strError = strprintf("HREG rebuild: failed ReadFromDisk height=%d", pindex->nHeight);
            return false;
        }
        std::map<uint256, CTransaction> mapLocalTx;
        std::set<COutPoint> setLocalSpent;
        for (unsigned int i = 0; i < block.vtx.size(); ++i)
        {
            CTransaction tx = block.vtx[i];
            MapPrevTx mapInputs;
            if (!BuildReplayInputsForTx(txdb, tx, mapLocalTx, setLocalSpent, mapInputs, strError))
                return false;
            if (!g_registry.ApplyConnectedTx(tx, mapInputs, pindex->nHeight, strError))
                return false;
            for (unsigned int nIn = 0; nIn < tx.vin.size(); ++nIn)
                setLocalSpent.insert(tx.vin[nIn].prevout);
            mapLocalTx[tx.GetHash()] = tx;
        }
    }
    return true;
}

} // namespace hreg
