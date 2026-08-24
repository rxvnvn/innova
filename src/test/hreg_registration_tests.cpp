// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include <boost/test/unit_test.hpp>

#include "../hreg_registration.h"
#include "../script.h"
#include "../util.h"

using namespace hreg;

namespace {

static const int64_t HREG_COLLATERAL = 25000LL * COIN;

CKeyID KeyId(unsigned char fill)
{
    uint160 h = 0;
    memset(h.begin(), fill, 20);
    return CKeyID(h);
}

CScript P2PKHScript(unsigned char fill)
{
    return GetScriptForDestination(KeyId(fill));
}

CTxOut MakePrevOut(int64_t value, const CScript& spk)
{
    CTxOut out;
    out.nValue = value;
    out.scriptPubKey = spk;
    return out;
}

CTransaction MakePrevTx(const std::vector<CTxOut>& vouts, bool fCoinbase=false, bool fCoinstake=false)
{
    CTransaction tx;
    tx.nVersion = CTransaction::CURRENT_VERSION;
    tx.nTime = 1000;
    if (fCoinbase) {
        tx.vin.push_back(CTxIn(COutPoint(), CScript() << OP_1));
    } else if (fCoinstake) {
        tx.vin.push_back(CTxIn(uint256(1), 0, CScript() << OP_1));
    } else {
        tx.vin.push_back(CTxIn(uint256(2), 0, CScript() << OP_1));
    }
    tx.vout = vouts;
    return tx;
}

void PutPrev(MapPrevTx& mp, const CTransaction& tx)
{
    CTxIndex idx(CDiskTxPos(0,0,0), tx.vout.size());
    mp[tx.GetHash()] = std::make_pair(idx, tx);
}

CScript MarkerScript()
{
    CScript s;
    std::vector<unsigned char> payload;
    payload.push_back('I'); payload.push_back('N'); payload.push_back('C'); payload.push_back('N');
    payload.push_back(0x01); payload.push_back(0x01);
    s << OP_RETURN << payload;
    return s;
}

CTransaction MakeRegistrationTx(const CTransaction& prev, unsigned int prevN,
                                bool marker=true,
                                bool duplicateMarker=false,
                                bool duplicateMatchingCollateral=false,
                                bool changeScript=false,
                                int64_t recreatedValue=HREG_COLLATERAL,
                                int64_t changeValue=1*COIN,
                                bool addExtraFeeInput=false,
                                const CTransaction* prevFeeTx=NULL,
                                unsigned int prevFeeN=0)
{
    CTransaction tx;
    tx.nVersion = CTransaction::CURRENT_VERSION;
    tx.nTime = 2000;
    tx.vin.push_back(CTxIn(prev.GetHash(), prevN, CScript() << OP_1));
    if (addExtraFeeInput && prevFeeTx)
        tx.vin.push_back(CTxIn(prevFeeTx->GetHash(), prevFeeN, CScript() << OP_1));
    tx.vout.push_back(CTxOut(recreatedValue, changeScript ? P2PKHScript(0x44) : prev.vout[prevN].scriptPubKey));
    if (duplicateMatchingCollateral)
        tx.vout.push_back(CTxOut(recreatedValue, changeScript ? P2PKHScript(0x44) : prev.vout[prevN].scriptPubKey));
    if (changeValue > 0)
        tx.vout.push_back(CTxOut(changeValue, P2PKHScript(0x55)));
    if (marker)
        tx.vout.push_back(CTxOut(0, MarkerScript()));
    if (duplicateMarker)
        tx.vout.push_back(CTxOut(0, MarkerScript()));
    return tx;
}

MapPrevTx InputsFor(const CTransaction& regTx, const CTransaction& prevCollateral)
{
    MapPrevTx m;
    PutPrev(m, prevCollateral);
    SetHRegPrevTxHeightForTesting(prevCollateral.GetHash(), 80);
    return m;
}

bool HasState(const std::vector<RegisteredCollateralState>& v, const COutPoint& op, RegistrationState st)
{
    for (size_t i=0;i<v.size();++i)
        if (v[i].outpoint == op && v[i].state == st) return true;
    return false;
}

} // namespace

BOOST_AUTO_TEST_SUITE(hreg_registration_tests)

BOOST_AUTO_TEST_CASE(marker_exact_match)
{
    BOOST_CHECK(IsExactHRegMarkerScript(MarkerScript()));
    CScript wrong; std::vector<unsigned char> p; p.push_back('I'); p.push_back('N'); p.push_back('C'); p.push_back('N'); p.push_back(0x01); p.push_back(0x02); wrong << OP_RETURN << p;
    BOOST_CHECK(!IsExactHRegMarkerScript(wrong));
}

BOOST_AUTO_TEST_CASE(plain_25k_without_marker_remains_unregistered)
{
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0, false);
    MapPrevTx mp = InputsFor(tx, prev);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(tx, mp, 100, err));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(valid_registration_creates_pending)
{
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0);
    MapPrevTx mp = InputsFor(tx, prev);
    std::string err;
    RegistrationParseResult parsed = EvaluateHRegRegistrationTx(tx, mp, 100);
    BOOST_CHECK_EQUAL((int)parsed.action, (int)RegistrationParseResult::ACTION_APPLY);
    BOOST_CHECK(ApplyHRegConnectedTx(tx, mp, 100, err));
    std::vector<RegisteredCollateralState> snap = GetHRegStateSnapshotForTesting();
    BOOST_REQUIRE_EQUAL(snap.size(), 1U);
    BOOST_CHECK_EQUAL(snap[0].state, HREG_STATE_PENDING);
    BOOST_CHECK_EQUAL(snap[0].registrationHeight, 100);
    ClearHRegActivationOverrideForTesting();
}


BOOST_AUTO_TEST_CASE(valid_registration_with_extra_fee_input)
{
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction feeTx = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(2*COIN, P2PKHScript(0x22))));
    CTransaction tx = MakeRegistrationTx(prev, 0, true, false, false, false, HREG_COLLATERAL, 1*COIN, true, &feeTx, 0);
    MapPrevTx mp; PutPrev(mp, prev); PutPrev(mp, feeTx); SetHRegPrevTxHeightForTesting(prev.GetHash(), 80); SetHRegPrevTxHeightForTesting(feeTx.GetHash(), 80);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(tx, mp, 100, err));
    BOOST_REQUIRE_EQUAL(GetHRegStateSnapshotForTesting().size(), 1U);
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(wrong_value_non_registration)
{
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(24000LL*COIN, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0);
    MapPrevTx mp = InputsFor(tx, prev);
    SetHRegPrevTxHeightForTesting(prev.GetHash(), 90);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(tx, mp, 100, err));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(immature_collateral_non_registration)
{
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0);
    MapPrevTx mp = InputsFor(tx, prev);
    SetHRegPrevTxHeightForTesting(prev.GetHash(), 90);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(tx, mp, 100, err));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(non_p2pkh_non_registration)
{
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CScript bare; bare << std::vector<unsigned char>(33, 2) << OP_CHECKSIG;
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, bare)));
    CTransaction tx = MakeRegistrationTx(prev, 0);
    MapPrevTx mp = InputsFor(tx, prev);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(tx, mp, 100, err));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(changed_recreated_script_non_registration)
{
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0, true, false, false, true);
    MapPrevTx mp = InputsFor(tx, prev);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(tx, mp, 100, err));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(duplicate_matching_collateral_reject)
{
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0, true, false, true);
    MapPrevTx mp = InputsFor(tx, prev);
    std::string err;
    BOOST_CHECK(!ApplyHRegConnectedTx(tx, mp, 100, err));
    BOOST_CHECK(!err.empty());
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(multiple_markers_reject)
{
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0, true, true);
    MapPrevTx mp = InputsFor(tx, prev);
    std::string err;
    BOOST_CHECK(!ApplyHRegConnectedTx(tx, mp, 100, err));
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(malformed_marker_is_non_registration)
{
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0, false);
    tx.vout.push_back(CTxOut(0, CScript() << OP_RETURN << std::vector<unsigned char>(1, 'X')));
    MapPrevTx mp = InputsFor(tx, prev);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(tx, mp, 100, err));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    ClearHRegActivationOverrideForTesting();
}


BOOST_AUTO_TEST_CASE(unknown_marker_version_is_non_registration)
{
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0, false);
    std::vector<unsigned char> pld; pld.push_back('I'); pld.push_back('N'); pld.push_back('C'); pld.push_back('N'); pld.push_back(0x02); pld.push_back(0x01);
    tx.vout.push_back(CTxOut(0, CScript() << OP_RETURN << pld));
    MapPrevTx mp = InputsFor(tx, prev);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(tx, mp, 100, err));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(coinbase_like_tx_no_registration_effect)
{
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0);
    tx.vin.clear();
    tx.vin.push_back(CTxIn(COutPoint(), CScript() << OP_1));
    MapPrevTx mp; PutPrev(mp, prev); SetHRegPrevTxHeightForTesting(prev.GetHash(), 80);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(tx, mp, 100, err));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(coinstake_like_tx_no_registration_effect)
{
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0);
    tx.vin.clear();
    tx.vin.push_back(CTxIn(uint256(1), 0, CScript() << OP_1));
    tx.vout[0].SetEmpty();
    MapPrevTx mp; PutPrev(mp, prev); SetHRegPrevTxHeightForTesting(prev.GetHash(), 80);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(tx, mp, 100, err));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(anonymous_like_tx_no_registration_effect)
{
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0);
    tx.nVersion = ANON_TXN_VERSION;
    MapPrevTx mp; PutPrev(mp, prev); SetHRegPrevTxHeightForTesting(prev.GetHash(), 80);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(tx, mp, 100, err));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(shielded_like_tx_no_registration_effect)
{
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0);
    tx.nVersion = SHIELDED_TX_VERSION;
    MapPrevTx mp; PutPrev(mp, prev); SetHRegPrevTxHeightForTesting(prev.GetHash(), 80);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(tx, mp, 100, err));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(spend_then_attempted_registration_no_state)
{
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction spend;
    spend.nVersion = CTransaction::CURRENT_VERSION;
    spend.nTime = 1500;
    spend.vin.push_back(CTxIn(prev.GetHash(), 0, CScript() << OP_1));
    spend.vout.push_back(CTxOut(5*COIN, P2PKHScript(0x33)));
    MapPrevTx mp; PutPrev(mp, prev); SetHRegPrevTxHeightForTesting(prev.GetHash(), 80);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(spend, mp, 100, err));
    mp[prev.GetHash()].first.vSpent[0] = CDiskTxPos(9,9,9);
    CTransaction reg = MakeRegistrationTx(prev, 0);
    BOOST_CHECK(ApplyHRegConnectedTx(reg, mp, 100, err));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(maturity_boundary)
{
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0);
    MapPrevTx mp = InputsFor(tx, prev);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(tx, mp, 100, err));
    BOOST_CHECK_EQUAL(ComputePreSpendState(100, 115), HREG_STATE_PENDING);
    BOOST_CHECK_EQUAL(ComputePreSpendState(100, 116), HREG_STATE_ELIGIBLE);
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(spend_registered_without_marker_revokes)
{
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction reg = MakeRegistrationTx(prev, 0);
    MapPrevTx mp = InputsFor(reg, prev);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(reg, mp, 100, err));
    CTransaction spend;
    spend.nVersion = CTransaction::CURRENT_VERSION;
    spend.nTime = 2001;
    spend.vin.push_back(CTxIn(reg.GetHash(), 0, CScript() << OP_1));
    spend.vout.push_back(CTxOut(10*COIN, P2PKHScript(0x77)));
    MapPrevTx mp2; PutPrev(mp2, reg); SetHRegPrevTxHeightForTesting(reg.GetHash(), 100);
    BOOST_CHECK(ApplyHRegConnectedTx(spend, mp2, 101, err));
    std::vector<RegisteredCollateralState> snap = GetHRegStateSnapshotForTesting();
    BOOST_CHECK(HasState(snap, COutPoint(reg.GetHash(),0), HREG_STATE_SPENT));
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(rollover_revokes_old_new_pending)
{
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction reg = MakeRegistrationTx(prev, 0);
    MapPrevTx mp = InputsFor(reg, prev);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(reg, mp, 100, err));
    CTransaction rollover = MakeRegistrationTx(reg, 0);
    MapPrevTx mp2; PutPrev(mp2, reg); SetHRegPrevTxHeightForTesting(reg.GetHash(), 100);
    BOOST_CHECK(ApplyHRegConnectedTx(rollover, mp2, 120, err));
    std::vector<RegisteredCollateralState> snap = GetHRegStateSnapshotForTesting();
    BOOST_CHECK(HasState(snap, COutPoint(reg.GetHash(),0), HREG_STATE_SPENT));
    BOOST_CHECK(HasState(snap, COutPoint(rollover.GetHash(),0), HREG_STATE_PENDING));
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(disconnect_registration_restores_empty)
{
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction reg = MakeRegistrationTx(prev, 0);
    MapPrevTx mp = InputsFor(reg, prev);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(reg, mp, 100, err));
    BOOST_CHECK(DisconnectHRegTx(reg, mp, 100, err));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(registration_then_same_block_spend_no_phantom)
{
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction reg = MakeRegistrationTx(prev, 0);
    MapPrevTx mp = InputsFor(reg, prev);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(reg, mp, 100, err));
    CTransaction spend;
    spend.nVersion = CTransaction::CURRENT_VERSION;
    spend.nTime = 2001;
    spend.vin.push_back(CTxIn(reg.GetHash(), 0, CScript() << OP_1));
    spend.vout.push_back(CTxOut(5*COIN, P2PKHScript(0x77)));
    MapPrevTx mp2; PutPrev(mp2, reg); SetHRegPrevTxHeightForTesting(reg.GetHash(), 100);
    BOOST_CHECK(ApplyHRegConnectedTx(spend, mp2, 100, err));
    std::vector<RegisteredCollateralState> snap = GetHRegStateSnapshotForTesting();
    BOOST_CHECK(HasState(snap, COutPoint(reg.GetHash(),0), HREG_STATE_SPENT));
    BOOST_CHECK(DisconnectHRegTx(spend, mp2, 100, err));
    BOOST_CHECK(DisconnectHRegTx(reg, mp, 100, err));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(rebuild_equals_replay_for_simple_history)
{
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction reg = MakeRegistrationTx(prev, 0);
    MapPrevTx mp = InputsFor(reg, prev);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(reg, mp, 100, err));
    std::vector<RegisteredCollateralState> live = GetHRegStateSnapshotForTesting();
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    SetHRegPrevTxHeightForTesting(prev.GetHash(), 80);
    BOOST_CHECK(ApplyHRegConnectedTx(reg, mp, 100, err));
    std::vector<RegisteredCollateralState> replay = GetHRegStateSnapshotForTesting();
    BOOST_CHECK_EQUAL(live.size(), replay.size());
    BOOST_CHECK_EQUAL(live[0].outpoint.ToString(), replay[0].outpoint.ToString());
    BOOST_CHECK_EQUAL((int)live[0].state, (int)replay[0].state);
    BOOST_CHECK_EQUAL(live[0].registrationHeight, replay[0].registrationHeight);
    ClearHRegActivationOverrideForTesting();
}


BOOST_AUTO_TEST_CASE(compat_valid_registration_old_tx_valid_new_state_apply)
{
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0);
    MapPrevTx mp = InputsFor(tx, prev);
    std::string reason, err;
    BOOST_CHECK(tx.CheckTransaction());
    BOOST_CHECK(IsStandardTx(tx, reason));
    BOOST_CHECK(ApplyHRegConnectedTx(tx, mp, 100, err));
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(compat_duplicate_marker_old_valid_new_reject)
{
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0, true, true);
    MapPrevTx mp = InputsFor(tx, prev);
    std::string reason, err;
    BOOST_CHECK(tx.CheckTransaction());
    BOOST_CHECK(IsStandardTx(tx, reason));
    BOOST_CHECK(!ApplyHRegConnectedTx(tx, mp, 100, err));
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(compat_malformed_marker_old_valid_new_nonregistration)
{
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0, false);
    tx.vout.push_back(CTxOut(0, CScript() << OP_RETURN << std::vector<unsigned char>(6, 'Z')));
    MapPrevTx mp = InputsFor(tx, prev);
    std::string reason, err;
    BOOST_CHECK(tx.CheckTransaction());
    BOOST_CHECK(IsStandardTx(tx, reason));
    BOOST_CHECK(ApplyHRegConnectedTx(tx, mp, 100, err));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(hreg_inactive_before_activation)
{
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(500);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction reg = MakeRegistrationTx(prev, 0);
    MapPrevTx mp = InputsFor(reg, prev);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(reg, mp, 100, err));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_SUITE_END()
