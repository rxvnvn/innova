// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Regression tests for the collateralnode payee-enforcement guard
// (ShouldValidateCollateralnodePayments / ConnectBlock CN-payment defer).
//
// Root cause under test: the pre-rebrand upstream guard included
// !IsInitialBlockDownload(), dropped in "Rebrand Phase Two", which also added the
// unbounded FindCNPayment genesis-ward scan. Without an authoritative-CN-state
// gate, the CN payee/rank validation runs (a) during IBD and (b) whenever the
// partially-synced collateralnode list misses the block's real payee -- in both
// cases triggering FindCNPayment's ~200s cs_main hold and false DoS-rejects.
//
// The guard's authoritative predicate is: NOT in initial block download AND the
// local CN list is at least as complete as the network median (local >= mnCount).
// These tests pin that predicate:
//   - IBD                                   -> deferred (counter++);
//   - thin/partial list (local < median)    -> deferred (counter++)   [188s residual];
//   - authoritative (synced, local >= med)  -> validation runs (counter 0);
//   - fJustCheck / payments-disabled / old  -> never validate, never defer-count.
//
// End-to-end payee accept/reject is covered by the controlled canary and the full
// suite; these are the deterministic unit-level gate.

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "../checkpoints.h"
#include "../collateralnode.h"
#include "../collateral.h"
#include "../activecollateralnode.h"
#include "../innovarpc.h"
#include "../main.h"
#include "../net.h"
#include "../util.h"
#include "../wallet.h"

namespace {

CCollateralNode MakeTestCN()
{
    return CCollateralNode(
        CService("127.0.0.1", 14530),
        CTxIn(),
        CPubKey(),
        std::vector<unsigned char>(),
        (int64_t)0,
        CPubKey(),
        50000);
}

// Scoped control of the global state that drives the guard:
//   fForceIbd : fImporting=true -> IBD true (list non-authoritative).
//   median    : sets the global mnCount (network-announced median CN count).
//   localSize : sets vecCollateralnodes.size() (local CN list size).
class CScopedCNState
{
private:
    bool fRegTestSaved, fImportingSaved, fReindexSaved;
    int nBestHeightSaved;
    CBlockIndex* pindexBestSaved;
    unsigned int mnCountSaved;
    std::vector<CCollateralNode> vecCollateralnodesSaved;
    std::vector<CNode*> vNodesSaved;
    bool fForceIbd;
    CBlockIndex pindexBestPlaceholder;

public:
    CScopedCNState(bool fForceIbd, unsigned int median, size_t localSize)
        : fForceIbd(fForceIbd)
    {
        fRegTestSaved = fRegTest;
        fImportingSaved = fImporting;
        fReindexSaved = fReindex;
        nBestHeightSaved = nBestHeight;
        pindexBestSaved = pindexBest;
        {
            LOCK(cs_collateralnodes);
            mnCountSaved = mnCount;
            vecCollateralnodesSaved = vecCollateralnodes;
        }
        {
            LOCK(cs_vNodes);
            vNodesSaved = vNodes;
        }

        fRegTest = false;
        fReindex = false;
        fImporting = fForceIbd;
        nBestHeight = std::max((int)nBestHeight,
                               Checkpoints::GetTotalBlocksEstimate());

        {
            LOCK(cs_collateralnodes);
            mnCount = median;
            vecCollateralnodes.clear();
            vecCollateralnodes.reserve(localSize);
            for (size_t i = 0; i < localSize; ++i)
                vecCollateralnodes.push_back(MakeTestCN());
        }

        if (!fForceIbd)
        {
            // No peers ahead + high nBestHeight + not importing -> IBD false.
            pindexBest = &pindexBestPlaceholder;
            pindexBestPlaceholder.nHeight =
                Checkpoints::GetTotalBlocksEstimate();
            LOCK(cs_vNodes);
            vNodes.clear();
        }
    }

    ~CScopedCNState()
    {
        {
            LOCK(cs_vNodes);
            vNodes = vNodesSaved;
        }
        {
            LOCK(cs_collateralnodes);
            vecCollateralnodes = vecCollateralnodesSaved;
            mnCount = mnCountSaved;
        }
        nBestHeight = nBestHeightSaved;
        fImporting = fImportingSaved;
        fReindex = fReindexSaved;
        fRegTest = fRegTestSaved;
        pindexBest = pindexBestSaved;
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(cnpayment_guard_tests)

// Case 1 / 2 / 8: IBD (unavailable CN list) -> a recent, non-justcheck,
// payments-enabled block DEFERS validation and counts the deferral (never enters
// the expensive rank/payee path, never false-rejects).
BOOST_AUTO_TEST_CASE(defer_when_ibd)
{
    ResetCNPaymentsDeferredCountForTesting();
    CBlockIndex idx;
    idx.nTime = (unsigned)GetTime();

    {
        CScopedCNState s(true /*IBD*/, 13 /*median*/, 3 /*local*/);
        BOOST_CHECK(!ShouldValidateCollateralnodePayments(&idx, false, true));
    }
    BOOST_CHECK_EQUAL(GetCNPaymentsDeferredCount(), 1);
}

// Case (canary residual): past IBD but the CN list is still THIN (local < median)
// -> DEFER, exactly the block-7,943,674 ~188s failure mode.
BOOST_AUTO_TEST_CASE(defer_when_list_partial_even_past_ibd)
{
    ResetCNPaymentsDeferredCountForTesting();
    CBlockIndex idx;
    idx.nTime = (unsigned)GetTime();

    {
        CScopedCNState s(false /*not IBD*/, 13 /*median*/, 10 /*local*/);
        BOOST_CHECK(!ShouldValidateCollateralnodePayments(&idx, false, true));
    }
    BOOST_CHECK_EQUAL(GetCNPaymentsDeferredCount(), 1);
}

// Case 4 / 11: fully synced, authoritative CN state (local >= median) ->
// validation runs unchanged (guard true), no deferral recorded.
BOOST_AUTO_TEST_CASE(validate_when_authoritative)
{
    ResetCNPaymentsDeferredCountForTesting();
    CBlockIndex idx;
    idx.nTime = (unsigned)GetTime();

    {
        CScopedCNState s(false /*not IBD*/, 3 /*median*/, 5 /*local>=med*/);
        BOOST_CHECK(ShouldValidateCollateralnodePayments(&idx, false, true));
    }
    BOOST_CHECK_EQUAL(GetCNPaymentsDeferredCount(), 0);
}

// Transition (case 7): flips exactly at the authoritative boundary (past IBD +
// list >= median), and never increments the defer counter once authoritative.
BOOST_AUTO_TEST_CASE(transition_thin_to_authoritative)
{
    ResetCNPaymentsDeferredCountForTesting();
    CBlockIndex idx;
    idx.nTime = (unsigned)GetTime();

    {
        CScopedCNState s(false, 13, 5); // thin -> defer
        BOOST_CHECK(!ShouldValidateCollateralnodePayments(&idx, false, true));
    }
    BOOST_CHECK_EQUAL(GetCNPaymentsDeferredCount(), 1);
    {
        CScopedCNState s(false, 13, 13); // now complete -> validate
        BOOST_CHECK(ShouldValidateCollateralnodePayments(&idx, false, true));
    }
    BOOST_CHECK_EQUAL(GetCNPaymentsDeferredCount(), 1); // no additional deferral
}

// Cases 3 / 5 / 6: fJustCheck, payments-disabled, and ancient blocks never
// validate (and never defer-count), regardless of authority.
BOOST_AUTO_TEST_CASE(justcheck_disabled_old_never_validate)
{
    ResetCNPaymentsDeferredCountForTesting();
    CBlockIndex recent;
    recent.nTime = (unsigned)GetTime();
    CBlockIndex ancient;
    ancient.nTime = 0;

    CScopedCNState s(false, 3, 5); // otherwise authoritative
    BOOST_CHECK(!ShouldValidateCollateralnodePayments(&recent, true, true));  // fJustCheck
    BOOST_CHECK(!ShouldValidateCollateralnodePayments(&recent, false, false)); // disabled
    BOOST_CHECK(!ShouldValidateCollateralnodePayments(&ancient, false, true)); // old
    BOOST_CHECK_EQUAL(GetCNPaymentsDeferredCount(), 0);
}

// Diagnostic counters (AUD-002/AUD-003): guard-level outcome counters are fully
// unit-tested here. The ConnectBlock-internal counters (payee_found /
// payee_missing / findcnpayment_entered) are incremented only inside the block
// connect path and are validated end-to-end by the production discriminator;
// here we pin their plumbing and that the guard layer never false-increments them.

// 1. defer increments deferred only (thin list / IBD).
BOOST_AUTO_TEST_CASE(diagnostic_defer_increments_deferred_only)
{
    ResetCNValidationCountersForTesting();
    ResetCNPaymentsDeferredCountForTesting();
    CBlockIndex idx;
    idx.nTime = (unsigned)GetTime();
    {
        CScopedCNState s(false /*not IBD*/, 13 /*median*/, 5 /*thin*/);
        BOOST_CHECK(!ShouldValidateCollateralnodePayments(&idx, false, true));
    }
    BOOST_CHECK_EQUAL(GetCNValidationGuardPassedCount(), 0);
    BOOST_CHECK_EQUAL(GetCNPaymentsDeferredCount(), 1);
    BOOST_CHECK_EQUAL(GetCNValidationPayeeFoundCount(), 0);
    BOOST_CHECK_EQUAL(GetCNValidationPayeeMissingCount(), 0);
    BOOST_CHECK_EQUAL(GetCNValidationFindCNPaymentEnteredCount(), 0);
    BOOST_CHECK_EQUAL(GetCNValidationLastGuardReason(), 5); // thin/list/lock-busy
}

// 2. guard-pass increments passed only.
BOOST_AUTO_TEST_CASE(diagnostic_guard_pass_increments_passed)
{
    ResetCNValidationCountersForTesting();
    ResetCNPaymentsDeferredCountForTesting();
    CBlockIndex idx;
    idx.nTime = (unsigned)GetTime();
    {
        CScopedCNState s(false, 3 /*median*/, 5 /*local>=med*/);
        BOOST_CHECK(ShouldValidateCollateralnodePayments(&idx, false, true));
    }
    BOOST_CHECK_EQUAL(GetCNValidationGuardPassedCount(), 1);
    BOOST_CHECK_EQUAL(GetCNPaymentsDeferredCount(), 0);
    BOOST_CHECK_EQUAL(GetCNValidationPayeeFoundCount(), 0);
    BOOST_CHECK_EQUAL(GetCNValidationPayeeMissingCount(), 0);
    BOOST_CHECK_EQUAL(GetCNValidationFindCNPaymentEnteredCount(), 0);
    BOOST_CHECK_EQUAL(GetCNValidationLastGuardReason(), 6); // passed
}

// 6. counters do not alter the validation result (return value identical).
BOOST_AUTO_TEST_CASE(diagnostic_counters_do_not_change_result)
{
    ResetCNValidationCountersForTesting();
    ResetCNPaymentsDeferredCountForTesting();
    CBlockIndex idx;
    idx.nTime = (unsigned)GetTime();
    // Repeated authoritative calls all return true and keep incrementing passed.
    for (int i = 0; i < 3; ++i)
    {
        CScopedCNState s(false, 3, 5);
        BOOST_CHECK(ShouldValidateCollateralnodePayments(&idx, false, true));
    }
    BOOST_CHECK_EQUAL(GetCNValidationGuardPassedCount(), 3);
    BOOST_CHECK_EQUAL(GetCNPaymentsDeferredCount(), 0);
    // Repeated thin calls all return false and keep incrementing deferred.
    for (int i = 0; i < 3; ++i)
    {
        CScopedCNState s(false, 13, 5);
        BOOST_CHECK(!ShouldValidateCollateralnodePayments(&idx, false, true));
    }
    BOOST_CHECK_EQUAL(GetCNValidationGuardPassedCount(), 3);
    BOOST_CHECK_EQUAL(GetCNPaymentsDeferredCount(), 3);
    BOOST_CHECK_EQUAL(GetCNValidationLastGuardReason(), 5);
}

// Plumbing: guard-level operations never touch the ConnectBlock-internal counters,
// and last_guard_height is recorded.
BOOST_AUTO_TEST_CASE(diagnostic_plumbing_and_last_height)
{
    ResetCNValidationCountersForTesting();
    ResetCNPaymentsDeferredCountForTesting();
    CBlockIndex idx;
    idx.nHeight = 42;
    idx.nTime = (unsigned)GetTime();
    {
        CScopedCNState s(false, 13, 5);
        BOOST_CHECK(!ShouldValidateCollateralnodePayments(&idx, false, true));
    }
    BOOST_CHECK_EQUAL(GetCNValidationLastGuardHeight(), 42);
    BOOST_CHECK_EQUAL(GetCNValidationFindCNPaymentEnteredCount(), 0);
    BOOST_CHECK_EQUAL(GetCNValidationPayeeMissingCount(), 0);
    BOOST_CHECK_EQUAL(GetCNValidationPayeeFoundCount(), 0);
}

using json_spirit::find_value;

BOOST_AUTO_TEST_CASE(collateral_outpoint_diagnostic_is_registered_and_read_only)
{
    const CRPCCommand* command = tableRPC["getcollateraloutpointdiagnostics"];
    BOOST_REQUIRE(command != NULL);

    CKey key;
    key.MakeNewKey(true);
    {
        LOCK(pwalletMain->cs_wallet);
        BOOST_REQUIRE(pwalletMain->AddKey(key));
    }

    CScript scriptPubKey;
    scriptPubKey.SetDestination(key.GetPubKey().GetID());
    CTransaction tx;
    tx.vin.push_back(CTxIn(COutPoint(uint256(1), 0)));
    tx.vout.push_back(CTxOut(GetMNCollateral() * COIN, scriptPubKey));
    {
        LOCK(cs_main);
        BOOST_REQUIRE(pindexBest != NULL);
        tx.nTime = pindexBest->GetBlockTime();
    }
    const uint256 txid = tx.GetHash();
    size_t mapWalletSizeBefore;
    std::set<COutPoint> lockedCoinsBefore;
    CDataStream mapWalletBefore(SER_DISK, CLIENT_VERSION);
    CWalletTx cacheStateBefore;
    {
        LOCK(cs_main);
        BOOST_REQUIRE(mempool.addUnchecked(txid, tx));
    }
    {
        LOCK(pwalletMain->cs_wallet);
        pwalletMain->mapWallet[txid] = CWalletTx(pwalletMain, tx);
        pwalletMain->mapWallet[txid].BindWallet(pwalletMain);
        pwalletMain->mapWallet[txid].hashBlock = pindexBest->GetBlockHash();
        pwalletMain->mapWallet[txid].nIndex = -1;
        mapWalletSizeBefore = pwalletMain->mapWallet.size();
        lockedCoinsBefore = pwalletMain->setLockedCoins;
        mapWalletBefore << pwalletMain->mapWallet;
        cacheStateBefore = pwalletMain->mapWallet[txid];
    }

    json_spirit::Array params;
    params.push_back(txid.GetHex());
    params.push_back(0);
    json_spirit::Object result = command->actor(params, false).get_obj();

    BOOST_CHECK(find_value(result, "tx_exists").get_bool());
    BOOST_CHECK(find_value(result, "output_exists").get_bool());
    BOOST_CHECK_EQUAL(find_value(result, "is_mine").get_str(), "spendable");
    BOOST_CHECK(!find_value(result, "is_spent").get_bool());
    BOOST_CHECK(!find_value(result, "is_locked_coin").get_bool());
    BOOST_CHECK_EQUAL(find_value(result, "depth").get_int(), 0);
    BOOST_CHECK(find_value(result, "trusted").is_null());
    BOOST_CHECK_EQUAL(find_value(result, "trusted_reason").get_str(),
                      "unknown at zero depth because exact trust evaluation would mutate wallet caches");
    BOOST_CHECK(find_value(result, "spendable").get_bool());
    // zero-depth fixture: AvailableCoins (fOnlyConfirmed=false) includes it; AvailableCoinsMN
    // (fOnlyConfirmed=true) excludes it because it is not confirmed (depth<1).
    BOOST_CHECK(find_value(result, "in_available_coins").get_bool());
    BOOST_CHECK(!find_value(result, "in_available_coins_mn").get_bool());
    BOOST_CHECK(!find_value(result, "in_select_coins_collateralnode").get_bool());
    BOOST_CHECK(find_value(result, "in_select_coins_collateralnode_for_pubkey").get_bool());
    BOOST_CHECK(find_value(result, "get_vin_from_output_success").get_bool());
    BOOST_CHECK(find_value(result, "get_key_exists").get_bool());
    BOOST_CHECK_EQUAL(find_value(result, "available_coins_rejection").get_str(), "");
    BOOST_CHECK(find_value(result, "available_coins_mn_rejection").get_str().find("transaction is not trusted") != std::string::npos);
    BOOST_CHECK(find_value(result, "available_coins_mn_rejection").get_str().find("depth<1") != std::string::npos);
    BOOST_CHECK_EQUAL(find_value(result, "select_coins_collateralnode_rejection").get_str(),
                      find_value(result, "available_coins_mn_rejection").get_str());
    BOOST_CHECK_EQUAL(find_value(result, "select_coins_collateralnode_for_pubkey_rejection").get_str(), "");
    // Independent real-predicate observations.
    BOOST_CHECK_EQUAL(find_value(result, "tx_depth").get_int(), 0);
    BOOST_CHECK(find_value(result, "tx_hashblock_present").get_bool());
    BOOST_CHECK(find_value(result, "tx_ntime_le_besttime").get_bool());
    BOOST_CHECK(!find_value(result, "tx_trusted_real_semantics").get_bool());
    BOOST_CHECK_EQUAL(find_value(result, "get_vin_from_output_rejection").get_str(), "");
    BOOST_CHECK_EQUAL(find_value(result, "get_key_rejection").get_str(), "");

    // Strict read-only: no wallet mutation (mapWallet bytes + lock set + cache flags).
    {
        LOCK(pwalletMain->cs_wallet);
        CDataStream mapWalletAfter(SER_DISK, CLIENT_VERSION);
        mapWalletAfter << pwalletMain->mapWallet;
        BOOST_CHECK_EQUAL(pwalletMain->mapWallet.size(), mapWalletSizeBefore);
        BOOST_CHECK_EQUAL(pwalletMain->mapWallet.count(txid), 1U);
        BOOST_CHECK_EQUAL_COLLECTIONS(mapWalletBefore.begin(), mapWalletBefore.end(),
                                      mapWalletAfter.begin(), mapWalletAfter.end());
        BOOST_CHECK(pwalletMain->setLockedCoins == lockedCoinsBefore);
        const CWalletTx& cacheStateAfter = pwalletMain->mapWallet[txid];
        BOOST_CHECK_EQUAL(cacheStateAfter.fDebitCached, cacheStateBefore.fDebitCached);
        BOOST_CHECK_EQUAL(cacheStateAfter.fCreditCached, cacheStateBefore.fCreditCached);
        BOOST_CHECK_EQUAL(cacheStateAfter.fImmatureCreditCached, cacheStateBefore.fImmatureCreditCached);
        BOOST_CHECK_EQUAL(cacheStateAfter.fAvailableCreditCached, cacheStateBefore.fAvailableCreditCached);
        BOOST_CHECK_EQUAL(cacheStateAfter.fWatchDebitCached, cacheStateBefore.fWatchDebitCached);
        BOOST_CHECK_EQUAL(cacheStateAfter.fWatchCreditCached, cacheStateBefore.fWatchCreditCached);
        BOOST_CHECK_EQUAL(cacheStateAfter.fImmatureWatchCreditCached, cacheStateBefore.fImmatureWatchCreditCached);
        BOOST_CHECK_EQUAL(cacheStateAfter.fAvailableWatchCreditCached, cacheStateBefore.fAvailableWatchCreditCached);
        BOOST_CHECK_EQUAL(cacheStateAfter.fChangeCached, cacheStateBefore.fChangeCached);
        BOOST_CHECK_EQUAL(cacheStateAfter.fAvailableAnonCreditCached, cacheStateBefore.fAvailableAnonCreditCached);
        BOOST_CHECK_EQUAL(cacheStateAfter.fCreditSplitCached, cacheStateBefore.fCreditSplitCached);
        BOOST_CHECK_EQUAL(cacheStateAfter.nDebitCached, cacheStateBefore.nDebitCached);
        BOOST_CHECK_EQUAL(cacheStateAfter.nCreditCached, cacheStateBefore.nCreditCached);
        BOOST_CHECK_EQUAL(cacheStateAfter.nImmatureCreditCached, cacheStateBefore.nImmatureCreditCached);
        BOOST_CHECK_EQUAL(cacheStateAfter.nAvailableCreditCached, cacheStateBefore.nAvailableCreditCached);
        BOOST_CHECK_EQUAL(cacheStateAfter.nWatchDebitCached, cacheStateBefore.nWatchDebitCached);
        BOOST_CHECK_EQUAL(cacheStateAfter.nWatchCreditCached, cacheStateBefore.nWatchCreditCached);
        BOOST_CHECK_EQUAL(cacheStateAfter.nChangeCached, cacheStateBefore.nChangeCached);
        BOOST_CHECK_EQUAL(cacheStateAfter.nImmatureWatchCreditCached, cacheStateBefore.nImmatureWatchCreditCached);
        BOOST_CHECK_EQUAL(cacheStateAfter.nAvailableWatchCreditCached, cacheStateBefore.nAvailableWatchCreditCached);
        BOOST_CHECK_EQUAL(cacheStateAfter.nAvailableAnonCreditCached, cacheStateBefore.nAvailableAnonCreditCached);
        BOOST_CHECK_EQUAL(cacheStateAfter.nCredDCached, cacheStateBefore.nCredDCached);
        BOOST_CHECK_EQUAL(cacheStateAfter.nCredAnonCached, cacheStateBefore.nCredAnonCached);
        BOOST_CHECK_EQUAL(pwalletMain->mapWallet.erase(txid), 1U);
    }
    mempool.remove(tx);
}

BOOST_AUTO_TEST_CASE(collateral_outpoint_diagnostic_reports_missing_wallet_transaction)
{
    const CRPCCommand* command = tableRPC["getcollateraloutpointdiagnostics"];
    BOOST_REQUIRE(command != NULL);
    json_spirit::Array params;
    params.push_back(uint256(42).GetHex());
    params.push_back(0);
    json_spirit::Object result = command->actor(params, false).get_obj();

    BOOST_CHECK(!find_value(result, "tx_exists").get_bool());
    BOOST_CHECK(!find_value(result, "output_exists").get_bool());
    BOOST_CHECK(find_value(result, "amount").is_null());
    BOOST_CHECK(find_value(result, "scriptPubKey").is_null());
    BOOST_CHECK_EQUAL(find_value(result, "is_mine").get_str(), "unavailable");
    BOOST_CHECK(!find_value(result, "get_key_exists").get_bool());
    BOOST_CHECK_EQUAL(find_value(result, "available_coins_rejection").get_str(),
                      "wallet transaction not found");
    BOOST_CHECK_EQUAL(find_value(result, "rejection_stage").get_str(), "wallet_lookup");
    BOOST_CHECK_EQUAL(find_value(result, "rejection_reason").get_str(),
                      "wallet transaction not found");
}

BOOST_AUTO_TEST_CASE(memory_diagnostic_is_registered_consistent_and_read_only)
{
    const CRPCCommand* command = tableRPC["getmemorydiagnostics"];
    BOOST_REQUIRE(command != NULL);

    size_t mapWalletSizeBefore;
    std::set<COutPoint> lockedCoinsBefore;
    CDataStream mapWalletBefore(SER_DISK, CLIENT_VERSION);
    {
        LOCK2(cs_main, pwalletMain->cs_wallet);
        mapWalletSizeBefore = pwalletMain->mapWallet.size();
        lockedCoinsBefore = pwalletMain->setLockedCoins;
        mapWalletBefore << pwalletMain->mapWallet;
    }

    json_spirit::Array params;
    json_spirit::Object result = command->actor(params, false).get_obj();

    BOOST_CHECK_EQUAL(find_value(result, "mapBlockIndex_count").get_int64(),
                      (int64_t)mapBlockIndex.size());
    BOOST_CHECK_EQUAL(find_value(result, "sizeof_CBlockIndex").get_int64(),
                      (int64_t)sizeof(CBlockIndex));
    BOOST_CHECK(find_value(result, "sizeof_map_value_type").get_int64() > 0);
    BOOST_CHECK(find_value(result, "sizeof_map_node").get_int64() > 0);
    BOOST_CHECK(find_value(result, "map_node_allocated_bytes").get_int64() > 0);
    BOOST_CHECK(find_value(result, "sizeof_CBlockIndex_allocated_bytes").get_int64() >= 0);
    BOOST_CHECK_EQUAL(find_value(result, "estimated_payload_bytes").get_int64(),
                      (int64_t)mapBlockIndex.size() * (int64_t)sizeof(CBlockIndex));
    BOOST_CHECK(find_value(result, "estimated_container_bytes").get_int64() >= 0);
    BOOST_CHECK(find_value(result, "estimated_allocated_bytes").get_int64() >=
                find_value(result, "estimated_payload_bytes").get_int64());
    BOOST_CHECK_EQUAL(find_value(result, "setStakeSeen_count").get_int64(),
                      (int64_t)setStakeSeen.size());
    BOOST_CHECK(find_value(result, "setStakeSeen_estimated_bytes").get_int64() >= 0);
    BOOST_CHECK(find_value(result, "setStakeSeenOrphan_count").get_int64() >= 0);
    BOOST_CHECK(find_value(result, "setInvalidBlockHash_count").get_int64() >= 0);
    BOOST_CHECK(find_value(result, "setpwalletRegistered_count").get_int64() >= 0);
    BOOST_CHECK_EQUAL(find_value(result, "vecCollateralnodes_count").get_int64(),
                      (int64_t)vecCollateralnodes.size());
    BOOST_CHECK(find_value(result, "vecCollateralnodes_estimated_bytes").get_int64() >= 0);
    BOOST_CHECK(find_value(result, "vecCollateralnodeRanks_count").get_int64() >= 0);
    BOOST_CHECK(find_value(result, "mapSeenCollateralnodeVotes_count").get_int64() >= 0);
    BOOST_CHECK(find_value(result, "mapCacheBlockHashes_count").get_int64() >= 0);
    BOOST_CHECK(find_value(result, "mempool_count").get_int64() >= 0);
    BOOST_CHECK_EQUAL(find_value(result, "mapWallet_count").get_int64(),
                      (int64_t)mapWalletSizeBefore);
    BOOST_CHECK(find_value(result, "setLockedCoins_count").get_int64() >= 0);

    // Strict read-only: no wallet mutation.
    {
        LOCK2(cs_main, pwalletMain->cs_wallet);
        CDataStream mapWalletAfter(SER_DISK, CLIENT_VERSION);
        mapWalletAfter << pwalletMain->mapWallet;
        BOOST_CHECK_EQUAL_COLLECTIONS(mapWalletBefore.begin(), mapWalletBefore.end(),
                                      mapWalletAfter.begin(), mapWalletAfter.end());
        BOOST_CHECK_EQUAL(pwalletMain->mapWallet.size(), mapWalletSizeBefore);
        BOOST_CHECK(pwalletMain->setLockedCoins == lockedCoinsBefore);
    }
}BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_CASE(parse_collateralnode_outpoint_valid)
{
    const std::string goodTx = "92da2832505942f0f86d23c0a1ccafe3ac02f5b7344a70e18eea673008eebf2c";
    std::string txid, err;
    unsigned int vout = 999;
    BOOST_CHECK(ParseCollateralNodeOutpoint(goodTx + "-0", txid, vout, err));
    BOOST_CHECK_EQUAL(txid, goodTx);
    BOOST_CHECK_EQUAL(vout, 0u);
    BOOST_CHECK(err.empty());

    BOOST_CHECK(ParseCollateralNodeOutpoint(goodTx + "-7", txid, vout, err));
    BOOST_CHECK_EQUAL(vout, 7u);

    // vout at the signed-int max boundary is valid (consumer uses lexical_cast<int>).
    BOOST_CHECK(ParseCollateralNodeOutpoint(goodTx + "-2147483647", txid, vout, err));
    BOOST_CHECK_EQUAL(vout, 2147483647u);
}

BOOST_AUTO_TEST_CASE(parse_collateralnode_outpoint_malformed)
{
    const std::string goodTx = "92da2832505942f0f86d23c0a1ccafe3ac02f5b7344a70e18eea673008eebf2c";
    std::string txid, err;
    unsigned int vout = 0;

    // No separator.
    BOOST_CHECK(!ParseCollateralNodeOutpoint(goodTx, txid, vout, err));
    BOOST_CHECK(!err.empty());

    // Wrong txid length.
    BOOST_CHECK(!ParseCollateralNodeOutpoint("abcd-0", txid, vout, err));
    BOOST_CHECK(err.find("txid") != std::string::npos);

    // Non-hex txid (correct length).
    std::string notHex(64, 'g');
    BOOST_CHECK(!ParseCollateralNodeOutpoint(notHex + "-0", txid, vout, err));
    BOOST_CHECK(err.find("txid") != std::string::npos);

    // Non-integer vout.
    BOOST_CHECK(!ParseCollateralNodeOutpoint(goodTx + "-abc", txid, vout, err));
    BOOST_CHECK(err.find("vout") != std::string::npos);

    // Negative vout cannot be cleanly expressed in "<txid>-<vout>" because a second
    // dash makes the txid segment longer than 64 characters; such input must still
    // be rejected. (Real vout range/format errors are covered by the cases below.)
    BOOST_CHECK(!ParseCollateralNodeOutpoint(goodTx + "--1", txid, vout, err));

    // Empty vout.
    BOOST_CHECK(!ParseCollateralNodeOutpoint(goodTx + "-", txid, vout, err));

    // vout above the signed-int range (lexical_cast<int> consumer would throw).
    BOOST_CHECK(!ParseCollateralNodeOutpoint(goodTx + "-4294967296", txid, vout, err));
    BOOST_CHECK(!ParseCollateralNodeOutpoint(goodTx + "-2147483648", txid, vout, err));
    BOOST_CHECK(err.find("vout") != std::string::npos);

    // A second dash makes the txid segment invalid (must be exactly 64 hex).
    BOOST_CHECK(!ParseCollateralNodeOutpoint(goodTx + "-1-2", txid, vout, err));
}
