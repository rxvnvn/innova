// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license.
#ifndef INN_HREG_REGISTRATION_H
#define INN_HREG_REGISTRATION_H

#include "core.h"
#include "main.h"
#include "blockindex_active_chain_reader.h"

#include <map>
#include <string>
#include <vector>

namespace hreg {

enum RegistrationState {
    HREG_STATE_PENDING = 1,
    HREG_STATE_ELIGIBLE = 2,
    HREG_STATE_SPENT = 3,
};

struct RegisteredCollateralState {
    COutPoint outpoint;
    CScript paymentScript;
    int registrationHeight;
    RegistrationState state;

    RegisteredCollateralState()
        : registrationHeight(0), state(HREG_STATE_PENDING) {}
};

struct RegistrationParseResult {
    enum Action {
        ACTION_NONE = 0,
        ACTION_APPLY = 1,
        ACTION_REJECT = 2,
    };

    Action action;
    std::string reason;
    COutPoint oldCollateral;
    COutPoint newCollateral;
    CScript paymentScript;

    RegistrationParseResult() : action(ACTION_NONE) {}
};

bool IsHRegRecognitionActive(int nHeight);
void SetHRegActivationOverrideForTesting(int nHeight);
void ClearHRegActivationOverrideForTesting();
void SetHRegPrevTxHeightForTesting(const uint256& txid, int nHeight);
void ClearHRegPrevTxHeightsForTesting();

bool IsExactHRegMarkerScript(const CScript& scriptPubKey);
bool IsStandardP2PKHScript(const CScript& scriptPubKey, CKeyID* pKeyIdOut = NULL);
RegistrationState ComputePreSpendState(int registrationHeight, int spendingBlockHeight);
RegistrationParseResult EvaluateHRegRegistrationTx(const CTransaction& tx,
                                                   const MapPrevTx& mapInputs,
                                                   int nBlockHeight);

class RegistryState {
public:
    void Clear();
    bool ApplyConnectedTx(const CTransaction& tx,
                          const MapPrevTx& mapInputs,
                          int nBlockHeight,
                          std::string& strError);
    bool DisconnectTx(const CTransaction& tx,
                      const MapPrevTx& mapInputs,
                      int nBlockHeight,
                      std::string& strError);
    bool Get(const COutPoint& outpoint, RegisteredCollateralState& out) const;
    bool Exists(const COutPoint& outpoint) const;
    std::vector<RegisteredCollateralState> Snapshot() const;

private:
    std::map<COutPoint, RegisteredCollateralState> m_states;
};

extern RegistryState g_registry;

void ClearHRegStateForTesting();
std::vector<RegisteredCollateralState> GetHRegStateSnapshotForTesting();
bool ApplyHRegConnectedTx(const CTransaction& tx,
                          const MapPrevTx& mapInputs,
                          int nBlockHeight,
                          std::string& strError);
bool DisconnectHRegTx(const CTransaction& tx,
                      const MapPrevTx& mapInputs,
                      int nBlockHeight,
                      std::string& strError);

bool RebuildHRegStateFromActiveChain(std::string& strError);

// By-value active-chain rebuild (A.10.1k/D-prereq). Iterates the active chain
// by HEIGHT from `reader` (by-value coordinates), reading each block by
// nFile/nBlockPos WITHOUT the resident mapBlockIndex/pnext CBlockIndex graph.
// Produces the same registry state as the legacy resident-path rebuild.
// startHeight=0 or -1 -> start at genesis (height 0). heightLimit inclusive, or
// -1 to use the reader's active height.
bool RebuildHRegStateFromActiveChainByValue(const BlockIndexActiveChainReader& reader,
                                            int32_t startHeight,
                                            int32_t heightLimit,
                                            std::string& strError);

} // namespace hreg

#endif
