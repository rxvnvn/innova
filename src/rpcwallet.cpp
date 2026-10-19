// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2017-2020 Denarius developers
// Copyright (c) 2019-2020 Innova Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "wallet.h"
#include "walletdb.h"
#include "innovarpc.h"
#include "init.h"
#include "base58.h"
#include "main.h"
#include "ibdmetrics.h"
#include "ibdsemantic.h"
#include "stealth.h"
#include "collateral.h"
#include "blockindex_manager.h"

#include <openssl/crypto.h>
#include <chrono>

static int64_t RPCPerfTimeMicros(bool fEnabled)
{
    if (!fEnabled)
        return 0;
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

#define RPCPERF_LOG(...) do { try { printf(__VA_ARGS__); } catch (...) {} } while (0)

#include <sstream>
#include <fstream>

using namespace json_spirit;
using namespace std;

namespace fs = boost::filesystem;

int64_t nWalletUnlockTime;
static CCriticalSection cs_nWalletUnlockTime;

static fs::path SanitizeWalletPath(const std::string& strPath, const std::string& strSubDir = "backups")
{
    fs::path dataDir = GetDataDir();
    fs::path targetDir = dataDir / strSubDir;

    if (!fs::exists(targetDir))
        fs::create_directories(targetDir);

    fs::path inputPath(strPath);
    std::string filename = inputPath.filename().string();

    if (filename.empty())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid filename");

    for (size_t i = 0; i < filename.size(); i++)
    {
        unsigned char c = (unsigned char)filename[i];
        if (c < 0x20 || c == 0x7F)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Filename contains control characters");
    }

    if (filename.find("..") != std::string::npos ||
        filename.find("/") != std::string::npos ||
        filename.find("\\") != std::string::npos)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid characters in filename");

    fs::path safePath = targetDir / filename;

    fs::path canonicalPath = fs::absolute(safePath);
    fs::path canonicalDataDir = fs::absolute(dataDir);

    std::string pathStr = canonicalPath.string();
    std::string dataDirStr = canonicalDataDir.string();

    if (pathStr.substr(0, dataDirStr.length()) != dataDirStr)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Path traversal attempt detected");

    return safePath;
}

extern void TxToJSON(const CTransaction& tx, const uint256 hashBlock, json_spirit::Object& entry);

static void accountingDeprecationCheck()
{
    if (!GetBoolArg("-enableaccounts", false))
        throw runtime_error(
            "The Accounting API will be updated in the future.\n"
            "It can easily result in negative or odd balances if misused or misunderstood, which has happened in the field.\n"
            "If you still want to enable it, add to your config file enableaccounts=1\n");

    if (GetBoolArg("-staking", true))
        throw runtime_error("If you want to use accounting API, staking must be disabled, add to your config file staking=0\n");
}

std::string HelpRequiringPassphrase()
{
    return pwalletMain->IsCrypted()
        ? "\nrequires wallet passphrase to be set with walletpassphrase first"
        : "";
}

void EnsureWalletIsUnlocked()
{
    if (pwalletMain->IsLocked())
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");
    if (fWalletUnlockStakingOnly)
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Wallet is unlocked for staking only.");
}

// Extract passphrase to SecureString and cleanse the source Value
SecureString SecureExtractPassphrase(const Value& val)
{
    const std::string& strRef = val.get_str();

    SecureString result;
    result.reserve(strRef.size() + 1);
    result = strRef.c_str();

    std::string& strMutable = const_cast<std::string&>(strRef);
    if (!strMutable.empty())
    {
        OPENSSL_cleanse(&strMutable[0], strMutable.size());
        strMutable.clear();
    }

    return result;
}

void WalletTxToJSON(const CWalletTx& wtx, Object& entry)
{
    entry.push_back(Pair("version", wtx.nVersion));
    int confirms = wtx.GetDepthInMainChain();
    entry.push_back(Pair("confirmations", confirms));
    if (wtx.IsCoinBase() || wtx.IsCoinStake())
        entry.push_back(Pair("generated", true));
    if (confirms > 0)
    {
        entry.push_back(Pair("blockhash", wtx.hashBlock.GetHex()));
        entry.push_back(Pair("blockindex", wtx.nIndex));
        int64_t nTime = 0;
        if (g_fAuthoritativeStartup)
        {
            BlockIndexSnapshot snapshot;
            std::string error;
            if (ResolveAuthoritativeActiveBlock(wtx.hashBlock, &snapshot, &error))
                nTime = snapshot.nTime;
            else
                nTime = -1; // authority failure: never consult historical mapBlockIndex
        }
        else
            nTime = mapBlockIndex[wtx.hashBlock]->nTime;

        if (nTime >= 0)
            entry.push_back(Pair("blocktime", nTime));
    };
    entry.push_back(Pair("txid", wtx.GetHash().GetHex()));
    entry.push_back(Pair("time", (int64_t)wtx.GetTxTime()));
    entry.push_back(Pair("timereceived", (int64_t)wtx.nTimeReceived));

    BOOST_FOREACH(const PAIRTYPE(string,string)& item, wtx.mapValue)
        entry.push_back(Pair(item.first, item.second));
}

string AccountFromValue(const Value& value)
{
    string strAccount = value.get_str();
    if (strAccount == "*")
        throw JSONRPCError(RPC_WALLET_INVALID_ACCOUNT_NAME, "Invalid account name");
    return strAccount;
}

// RPC-SPLIT: single authoritative IBD-metrics JSON builder.
// Shared by getinfo(extended=true) and getibdmetrics() so the diagnostic
// schema and counter-gathering logic exist in exactly ONE place.
static Object BuildIBDMetricsJson(int nHeight)
{
    IBDMetricsSnapshot metrics;
    ibdmetrics::SnapshotAll(metrics);
    Object ibd;
    ibd.push_back(Pair("start_time_unix", metrics.start_time_unix));
    ibd.push_back(Pair("now_unix", metrics.now_unix));
    ibd.push_back(Pair("uptime_seconds", metrics.uptime_seconds));
    ibd.push_back(Pair("height", nHeight));

    ibd.push_back(Pair("deferred_budget_calls", metrics.deferred_budget_calls));
    ibd.push_back(Pair("deferred_budget_positive", metrics.deferred_budget_positive));
    ibd.push_back(Pair("deferred_budget_zero", metrics.deferred_budget_zero));
    ibd.push_back(Pair("deferred_budget_zero_peer_pressure", metrics.deferred_budget_zero_peer_pressure));
    ibd.push_back(Pair("deferred_budget_zero_global_pressure", metrics.deferred_budget_zero_global_pressure));
    ibd.push_back(Pair("deferred_budget_zero_vnodes_lock_failed", metrics.deferred_budget_zero_vnodes_lock_failed));

    ibd.push_back(Pair("block_inv_unknown_total", metrics.block_inv_unknown_total));
    ibd.push_back(Pair("block_inv_admitted", metrics.block_inv_admitted));
    ibd.push_back(Pair("block_inv_deferred", metrics.block_inv_deferred));
    ibd.push_back(Pair("block_inv_deferred_no_budget", metrics.block_inv_deferred_no_budget));
    ibd.push_back(Pair("block_inv_deferred_overflow", metrics.block_inv_deferred_overflow));
    ibd.push_back(Pair("frontier_exemption_admitted", metrics.frontier_exemption_admitted));

    ibd.push_back(Pair("global_active_current", metrics.global_active_current));
    ibd.push_back(Pair("global_active_max", metrics.global_active_max));
    ibd.push_back(Pair("peer_pressure_max", metrics.peer_pressure_max));
    ibd.push_back(Pair("orphan_pressure_max", metrics.orphan_pressure_max));
    ibd.push_back(Pair("peers_at_zero_budget_current", metrics.peers_at_zero_budget_current));
    ibd.push_back(Pair("peers_at_zero_budget_max", metrics.peers_at_zero_budget_max));

    ibd.push_back(Pair("refill_calls", metrics.refill_calls));
    ibd.push_back(Pair("refill_calls_zero_budget", metrics.refill_calls_zero_budget));
    ibd.push_back(Pair("refill_items_examined", metrics.refill_items_examined));
    ibd.push_back(Pair("refill_items_admitted", metrics.refill_items_admitted));
    ibd.push_back(Pair("refill_items_already_have", metrics.refill_items_already_have));
    ibd.push_back(Pair("refill_items_active_owner", metrics.refill_items_active_owner));
    ibd.push_back(Pair("refill_work_limit_hit", metrics.refill_work_limit_hit));
    ibd.push_back(Pair("refill_txdb_opens", metrics.refill_txdb_opens));
    ibd.push_back(Pair("refill_alreadyhave_checks", metrics.refill_alreadyhave_checks));

    ibd.push_back(Pair("block_receive_total", metrics.block_receive_total));
    ibd.push_back(Pair("block_result_accepted_active", metrics.block_result_accepted_active));
    ibd.push_back(Pair("block_result_orphan_new", metrics.block_result_orphan_new));
    ibd.push_back(Pair("block_result_orphan_limit_rejected", metrics.block_result_orphan_limit_rejected));
    ibd.push_back(Pair("block_result_rejected_total", metrics.block_result_rejected_total));
    ibd.push_back(Pair("setbestchain_commits", metrics.setbestchain_commits));
    ibd.push_back(Pair("stalled_recovery_attempts", metrics.stalled_recovery_attempts));
    ibd.push_back(Pair("stall_watchdog_ticks", metrics.stall_watchdog_ticks));
    ibd.push_back(Pair("stall_watchdog_liveness_bypass", metrics.stall_watchdog_liveness_bypass));
    ibd.push_back(Pair("stall_watchdog_peer_rotations", metrics.stall_watchdog_peer_rotations));
    ibd.push_back(Pair("stall_watchdog_solo_recycles", metrics.stall_watchdog_solo_recycles));
    ibd.push_back(Pair("stall_watchdog_forced_reissues", metrics.stall_watchdog_forced_reissues));

    ibd.push_back(Pair("active_decrement_receive_clear_inflight", metrics.active_decrement_receive_clear_inflight));
    ibd.push_back(Pair("active_decrement_inflight_timeout", metrics.active_decrement_inflight_timeout));
    ibd.push_back(Pair("active_decrement_askfor_sent_transition", metrics.active_decrement_askfor_sent_transition));
    ibd.push_back(Pair("active_decrement_askfor_removed_already_have", metrics.active_decrement_askfor_removed_already_have));
    ibd.push_back(Pair("active_decrement_askfor_removed_owner_conflict", metrics.active_decrement_askfor_removed_owner_conflict));
    ibd.push_back(Pair("active_decrement_clear_askfor", metrics.active_decrement_clear_askfor));
    ibd.push_back(Pair("active_decrement_disconnect_cleanup", metrics.active_decrement_disconnect_cleanup));
    ibd.push_back(Pair("active_decrement_other", metrics.active_decrement_other));

    ibd.push_back(Pair("global_active_zero_transitions", metrics.global_active_zero_transitions));
    ibd.push_back(Pair("zero_with_total_deferred_nonempty", metrics.zero_with_total_deferred_nonempty));
    ibd.push_back(Pair("zero_with_total_deferred_empty", metrics.zero_with_total_deferred_empty));
    ibd.push_back(Pair("zero_with_eligible_ahead_peer", metrics.zero_with_eligible_ahead_peer));
    ibd.push_back(Pair("zero_without_eligible_ahead_peer", metrics.zero_without_eligible_ahead_peer));
    ibd.push_back(Pair("zero_duration_total_ms", metrics.zero_duration_total_ms));
    ibd.push_back(Pair("zero_duration_max_ms", metrics.zero_duration_max_ms));
    ibd.push_back(Pair("zero_duration_current_ms", metrics.zero_duration_current_ms));

    ibd.push_back(Pair("refill_opportunity_slot_freed", metrics.refill_opportunity_slot_freed));
    ibd.push_back(Pair("refill_sendmessages_passes", metrics.refill_sendmessages_passes));
    ibd.push_back(Pair("refill_skipped_cs_main_trylock_failed", metrics.refill_skipped_cs_main_trylock_failed));
    ibd.push_back(Pair("refill_called_deferred_empty", metrics.refill_called_deferred_empty));
    ibd.push_back(Pair("refill_called_positive_budget_nonempty", metrics.refill_called_positive_budget_nonempty));
    ibd.push_back(Pair("refill_positive_budget_admitted_zero", metrics.refill_positive_budget_admitted_zero));
    ibd.push_back(Pair("refill_positive_budget_admitted_count", metrics.refill_positive_budget_admitted_count));

    ibd.push_back(Pair("pipeline_drained_checks", metrics.pipeline_drained_checks));
    ibd.push_back(Pair("pipeline_drained_getblocks_queued", metrics.pipeline_drained_getblocks_queued));
    ibd.push_back(Pair("pipeline_drained_skip_not_ahead", metrics.pipeline_drained_skip_not_ahead));
    ibd.push_back(Pair("pipeline_drained_skip_getblocks_10s_cooldown", metrics.pipeline_drained_skip_getblocks_10s_cooldown));
    ibd.push_back(Pair("pipeline_drained_skip_other_condition", metrics.pipeline_drained_skip_other_condition));
    ibd.push_back(Pair("stake_source_ancestry_calls", metrics.stake_source_ancestry_calls));
    ibd.push_back(Pair("stake_source_ancestry_steps", metrics.stake_source_ancestry_steps));
    ibd.push_back(Pair("pushgetblocks_dedup_5s_skips", metrics.pushgetblocks_dedup_5s_skips));

    ibd.push_back(Pair("getblocks_decision_attempts_other", metrics.getblocks_decision_attempts_other));
    ibd.push_back(Pair("getblocks_decision_attempts_initial", metrics.getblocks_decision_attempts_initial));
    ibd.push_back(Pair("getblocks_decision_attempts_continuation", metrics.getblocks_decision_attempts_continuation));
    ibd.push_back(Pair("getblocks_decision_attempts_recovery", metrics.getblocks_decision_attempts_recovery));
    ibd.push_back(Pair("getblocks_decision_attempts_prefetch", metrics.getblocks_decision_attempts_prefetch));
    ibd.push_back(Pair("getblocks_decision_attempts_inv_continuation", metrics.getblocks_decision_attempts_inv_continuation));
    ibd.push_back(Pair("getblocks_decision_attempts_version", metrics.getblocks_decision_attempts_version));
    ibd.push_back(Pair("getblocks_decision_attempts_headers", metrics.getblocks_decision_attempts_headers));
    ibd.push_back(Pair("getblocks_decision_attempts_checkpoint", metrics.getblocks_decision_attempts_checkpoint));
    ibd.push_back(Pair("getblocks_decision_attempts_wallet_rescan", metrics.getblocks_decision_attempts_wallet_rescan));
    ibd.push_back(Pair("getblocks_decision_attempts_orphan_limit", metrics.getblocks_decision_attempts_orphan_limit));
    ibd.push_back(Pair("getblocks_decision_attempts_empty_pipeline_wake", metrics.getblocks_decision_attempts_empty_pipeline_wake));
    ibd.push_back(Pair("getblocks_queue_success_other", metrics.getblocks_queue_success_other));
    ibd.push_back(Pair("getblocks_queue_success_initial", metrics.getblocks_queue_success_initial));
    ibd.push_back(Pair("getblocks_queue_success_continuation", metrics.getblocks_queue_success_continuation));
    ibd.push_back(Pair("getblocks_queue_success_recovery", metrics.getblocks_queue_success_recovery));
    ibd.push_back(Pair("getblocks_queue_success_prefetch", metrics.getblocks_queue_success_prefetch));
    ibd.push_back(Pair("getblocks_queue_success_inv_continuation", metrics.getblocks_queue_success_inv_continuation));
    ibd.push_back(Pair("getblocks_queue_success_version", metrics.getblocks_queue_success_version));
    ibd.push_back(Pair("getblocks_queue_success_headers", metrics.getblocks_queue_success_headers));
    ibd.push_back(Pair("getblocks_queue_success_checkpoint", metrics.getblocks_queue_success_checkpoint));
    ibd.push_back(Pair("getblocks_queue_success_wallet_rescan", metrics.getblocks_queue_success_wallet_rescan));
    ibd.push_back(Pair("getblocks_queue_success_orphan_limit", metrics.getblocks_queue_success_orphan_limit));
    ibd.push_back(Pair("getblocks_queue_success_empty_pipeline_wake", metrics.getblocks_queue_success_empty_pipeline_wake));
    ibd.push_back(Pair("getblocks_wire_sent_other", metrics.getblocks_wire_sent_other));
    ibd.push_back(Pair("getblocks_wire_sent_initial", metrics.getblocks_wire_sent_initial));
    ibd.push_back(Pair("getblocks_wire_sent_continuation", metrics.getblocks_wire_sent_continuation));
    ibd.push_back(Pair("getblocks_wire_sent_recovery", metrics.getblocks_wire_sent_recovery));
    ibd.push_back(Pair("getblocks_wire_sent_prefetch", metrics.getblocks_wire_sent_prefetch));
    ibd.push_back(Pair("getblocks_wire_sent_inv_continuation", metrics.getblocks_wire_sent_inv_continuation));
    ibd.push_back(Pair("getblocks_wire_sent_version", metrics.getblocks_wire_sent_version));
    ibd.push_back(Pair("getblocks_wire_sent_headers", metrics.getblocks_wire_sent_headers));
    ibd.push_back(Pair("getblocks_wire_sent_checkpoint", metrics.getblocks_wire_sent_checkpoint));
    ibd.push_back(Pair("getblocks_wire_sent_wallet_rescan", metrics.getblocks_wire_sent_wallet_rescan));
    ibd.push_back(Pair("getblocks_wire_sent_orphan_limit", metrics.getblocks_wire_sent_orphan_limit));
    ibd.push_back(Pair("getblocks_wire_sent_empty_pipeline_wake", metrics.getblocks_wire_sent_empty_pipeline_wake));
    ibd.push_back(Pair("getblocks_dedup_skips", metrics.getblocks_dedup_skips));
    ibd.push_back(Pair("getblocks_identical_to_last_sent", metrics.getblocks_identical_to_last_sent));
    ibd.push_back(Pair("getblocks_response_inv_messages", metrics.getblocks_response_inv_messages));
    ibd.push_back(Pair("getblocks_response_block_inv_count", metrics.getblocks_response_block_inv_count));
    ibd.push_back(Pair("getblocks_response_unknown_count", metrics.getblocks_response_unknown_count));
    ibd.push_back(Pair("getblocks_response_zero_unknown", metrics.getblocks_response_zero_unknown));
    ibd.push_back(Pair("getblocks_response_inv_zero_unknown", metrics.getblocks_response_inv_zero_unknown));
    ibd.push_back(Pair("getblocks_served_inv_items", metrics.getblocks_served_inv_items));
    ibd.push_back(Pair("getblocks_served_inv_bytes", metrics.getblocks_served_inv_bytes));
    ibd.push_back(Pair("getblocks_consumption_getdata_matches", metrics.getblocks_consumption_getdata_matches));
    ibd.push_back(Pair("getblocks_suppressed_inv_replies", metrics.getblocks_suppressed_inv_replies));
    ibd.push_back(Pair("getblocks_suppressed_inv_items", metrics.getblocks_suppressed_inv_items));
    ibd.push_back(Pair("getblocks_suppressed_inv_bytes_avoided", metrics.getblocks_suppressed_inv_bytes_avoided));
    ibd.push_back(Pair("getblocks_zero_consume_windows", metrics.getblocks_zero_consume_windows));
    ibd.push_back(Pair("getblocks_low_nonzero_windows", metrics.getblocks_low_nonzero_windows));
    ibd.push_back(Pair("getblocks_release_latch_events", metrics.getblocks_release_latch_events));
    ibd.push_back(Pair("getblocks_reconnect_debt_entries", metrics.getblocks_reconnect_debt_entries));
    ibd.push_back(Pair("getblocks_reconnect_debt_transferred", metrics.getblocks_reconnect_debt_transferred));
    ibd.push_back(Pair("getblocks_reconnect_debt_evicted", metrics.getblocks_reconnect_debt_evicted));
    ibd.push_back(Pair("getblocks_reconnect_debt_cleared_by_consumption", metrics.getblocks_reconnect_debt_cleared_by_consumption));
    ibd.push_back(Pair("recovery_outcome_useful", metrics.recovery_outcome_useful));
    ibd.push_back(Pair("recovery_outcome_known_only", metrics.recovery_outcome_known_only));
    ibd.push_back(Pair("recovery_outcome_no_response", metrics.recovery_outcome_no_response));
    ibd.push_back(Pair("zero_to_first_getblocks_wire_send_ms_total", metrics.zero_to_first_getblocks_wire_send_ms_total));
    ibd.push_back(Pair("zero_to_first_getblocks_wire_send_ms_max", metrics.zero_to_first_getblocks_wire_send_ms_max));
    ibd.push_back(Pair("zero_to_first_inv_ms_total", metrics.zero_to_first_inv_ms_total));
    ibd.push_back(Pair("zero_to_first_inv_ms_max", metrics.zero_to_first_inv_ms_max));
    ibd.push_back(Pair("zero_to_first_unknown_inv_ms_total", metrics.zero_to_first_unknown_inv_ms_total));
    ibd.push_back(Pair("zero_to_first_unknown_inv_ms_max", metrics.zero_to_first_unknown_inv_ms_max));
    ibd.push_back(Pair("zero_to_first_askfor_ms_total", metrics.zero_to_first_askfor_ms_total));
    ibd.push_back(Pair("zero_to_first_askfor_ms_max", metrics.zero_to_first_askfor_ms_max));
    ibd.push_back(Pair("zero_to_active_nonzero_ms_total", metrics.zero_to_active_nonzero_ms_total));
    ibd.push_back(Pair("zero_to_active_nonzero_ms_max", metrics.zero_to_active_nonzero_ms_max));
    ibd.push_back(Pair("inv_unknown_during_zero_global", metrics.inv_unknown_during_zero_global));
    ibd.push_back(Pair("sync_peer_change_while_pipeline_empty", metrics.sync_peer_change_while_pipeline_empty));
    ibd.push_back(Pair("getblocks_outstanding_current", metrics.getblocks_outstanding_current));
    ibd.push_back(Pair("getblocks_outstanding_max", metrics.getblocks_outstanding_max));
    ibd.push_back(Pair("getblocks_outstanding_timeout", metrics.getblocks_outstanding_timeout));
    ibd.push_back(Pair("getblocks_no_response_disconnect_cleanup", metrics.getblocks_no_response_disconnect_cleanup));
    ibd.push_back(Pair("getblocks_pending_coalesce", metrics.getblocks_pending_coalesce));
    ibd.push_back(Pair("getblocks_pending_replaced", metrics.getblocks_pending_replaced));
    ibd.push_back(Pair("getblocks_pending_drop", metrics.getblocks_pending_drop));
    ibd.push_back(Pair("outstanding_gauge_mismatch", metrics.outstanding_gauge_mismatch));
    ibd.push_back(Pair("getblocks_queued_unsent_cleanup", metrics.getblocks_queued_unsent_cleanup));
    ibd.push_back(Pair("peers_with_queued_getblocks_current", metrics.peers_with_queued_getblocks_current));
    ibd.push_back(Pair("peers_with_queued_getblocks_max", metrics.peers_with_queued_getblocks_max));
    ibd.push_back(Pair("total_getblocks_queued_requests_current", metrics.total_getblocks_queued_requests_current));
    ibd.push_back(Pair("total_getblocks_queued_requests_max", metrics.total_getblocks_queued_requests_max));
    ibd.push_back(Pair("pipeline_active_due_to_getblocks_current", metrics.pipeline_active_due_to_getblocks_current));
    ibd.push_back(Pair("pipeline_active_due_to_getblocks_max", metrics.pipeline_active_due_to_getblocks_max));
    ibd.push_back(Pair("frontier_response_armed", metrics.frontier_response_armed));
    ibd.push_back(Pair("frontier_response_consumed", metrics.frontier_response_consumed));
    ibd.push_back(Pair("frontier_response_pending_current", metrics.frontier_response_pending_current));
    ibd.push_back(Pair("frontier_response_pending_max", metrics.frontier_response_pending_max));
    ibd.push_back(Pair("frontier_reject_locator_stale", metrics.frontier_reject_locator_stale));
    ibd.push_back(Pair("frontier_reject_slot_busy", metrics.frontier_reject_slot_busy));
    ibd.push_back(Pair("frontier_reject_already_admitted", metrics.frontier_reject_already_admitted));
    ibd.push_back(Pair("frontier_reject_other", metrics.frontier_reject_other));
    ibd.push_back(Pair("ibd_state_current", metrics.ibd_state_current));
    ibd.push_back(Pair("ibd_state_transitions", metrics.ibd_state_transitions));

    Object pipeline_wake;
    pipeline_wake.push_back(Pair("signals", metrics.pipeline_wake_signals));
    pipeline_wake.push_back(Pair("signal_clear_inflight", metrics.pipeline_wake_signal_clear_inflight));
    pipeline_wake.push_back(Pair("signal_inflight_timeout", metrics.pipeline_wake_signal_inflight_timeout));
    pipeline_wake.push_back(Pair("signal_askfor_already_have", metrics.pipeline_wake_signal_askfor_already_have));
    pipeline_wake.push_back(Pair("signal_askfor_owner_conflict", metrics.pipeline_wake_signal_askfor_owner_conflict));
    pipeline_wake.push_back(Pair("signal_queue_removal", metrics.pipeline_wake_signal_queue_removal));
    pipeline_wake.push_back(Pair("signal_clear_askfor", metrics.pipeline_wake_signal_clear_askfor));
    pipeline_wake.push_back(Pair("signal_disconnect_cleanup", metrics.pipeline_wake_signal_disconnect_cleanup));
    pipeline_wake.push_back(Pair("signal_getblocks_outstanding_cleared", metrics.pipeline_wake_signal_getblocks_outstanding_cleared));
    pipeline_wake.push_back(Pair("signal_getblocks_outstanding_timeout", metrics.pipeline_wake_signal_getblocks_outstanding_timeout));
    pipeline_wake.push_back(Pair("signal_other", metrics.pipeline_wake_signal_other));
    pipeline_wake.push_back(Pair("coalesced", metrics.pipeline_wake_coalesced));
    pipeline_wake.push_back(Pair("handler_runs", metrics.pipeline_wake_handler_runs));
    pipeline_wake.push_back(Pair("transient_cs_main_trylock_failed", metrics.pipeline_wake_transient_cs_main_trylock_failed));
    pipeline_wake.push_back(Pair("transient_cs_vnodes_trylock_failed", metrics.pipeline_wake_transient_cs_vnodes_trylock_failed));
    pipeline_wake.push_back(Pair("transient_cooldown_active", metrics.pipeline_wake_transient_cooldown_active));
    pipeline_wake.push_back(Pair("transient_dedup_all", metrics.pipeline_wake_transient_dedup_all));
    pipeline_wake.push_back(Pair("transient_incomplete_peer_scan", metrics.pipeline_wake_transient_incomplete_peer_scan));
    pipeline_wake.push_back(Pair("transient_shutdown", metrics.pipeline_wake_transient_shutdown));
    pipeline_wake.push_back(Pair("terminal_not_ibd", metrics.pipeline_wake_terminal_not_ibd));
    pipeline_wake.push_back(Pair("terminal_pipeline_not_empty", metrics.pipeline_wake_terminal_pipeline_not_empty));
    pipeline_wake.push_back(Pair("terminal_deferred_refill_created_work", metrics.pipeline_wake_terminal_deferred_refill_created_work));
    pipeline_wake.push_back(Pair("terminal_getblocks_queued", metrics.pipeline_wake_terminal_getblocks_queued));
    pipeline_wake.push_back(Pair("terminal_no_eligible_ahead_peer", metrics.pipeline_wake_terminal_no_eligible_ahead_peer));
    pipeline_wake.push_back(Pair("terminal_existing_queued_getblocks", metrics.pipeline_wake_terminal_existing_queued_getblocks));
    pipeline_wake.push_back(Pair("terminal_outstanding_getblocks_present", metrics.pipeline_wake_terminal_outstanding_getblocks_present));
    pipeline_wake.push_back(Pair("refill_attempts", metrics.pipeline_wake_refill_attempts));
    pipeline_wake.push_back(Pair("refill_admitted", metrics.pipeline_wake_refill_admitted));
    pipeline_wake.push_back(Pair("getblocks_attempted", metrics.pipeline_wake_getblocks_attempted));
    pipeline_wake.push_back(Pair("getblocks_queued", metrics.pipeline_wake_getblocks_queued));
    pipeline_wake.push_back(Pair("getblocks_dedup", metrics.pipeline_wake_getblocks_dedup));
    pipeline_wake.push_back(Pair("active_restored", metrics.pipeline_wake_active_restored));
    pipeline_wake.push_back(Pair("signal_to_active_ms_total", metrics.pipeline_wake_signal_to_active_ms_total));
    pipeline_wake.push_back(Pair("signal_to_active_ms_max", metrics.pipeline_wake_signal_to_active_ms_max));
    ibd.push_back(Pair("pipeline_wake", pipeline_wake));

    ibd.push_back(Pair("diversify_candidates", metrics.diversify_candidates));
    ibd.push_back(Pair("diversify_picked_other_lane", metrics.diversify_picked_other_lane));
    ibd.push_back(Pair("diversify_picked_announcer", metrics.diversify_picked_announcer));
    ibd.push_back(Pair("diversify_snapshot_skip_lock", metrics.diversify_snapshot_skip_lock));
    ibd.push_back(Pair("diversify_no_other_lane", metrics.diversify_no_other_lane));
    ibd.push_back(Pair("diversify_other_lane_timeout", metrics.diversify_other_lane_timeout));

    ibd.push_back(Pair("peer_quality_requests_observed", metrics.peer_quality_requests_observed));
    ibd.push_back(Pair("peer_quality_receive_outcomes", metrics.peer_quality_receive_outcomes));
    ibd.push_back(Pair("peer_quality_timeout_outcomes", metrics.peer_quality_timeout_outcomes));
    ibd.push_back(Pair("peer_quality_late_outcomes", metrics.peer_quality_late_outcomes));
    ibd.push_back(Pair("peer_quality_redirects", metrics.peer_quality_redirects));
    ibd.push_back(Pair("peer_concentration_redirects", metrics.peer_concentration_redirects));
    ibd.push_back(Pair("peer_quality_no_alternative", metrics.peer_quality_no_alternative));
    ibd.push_back(Pair("peer_concentration_no_alternative", metrics.peer_concentration_no_alternative));
    ibd.push_back(Pair("same_peer_reissue_after_timeout", metrics.same_peer_reissue_after_timeout));
    ibd.push_back(Pair("cross_peer_reissue_after_timeout", metrics.cross_peer_reissue_after_timeout));
    ibd.push_back(Pair("timeout_reissue_no_alternative", metrics.timeout_reissue_no_alternative));
    ibd.push_back(Pair("quality_unknown_selected", metrics.quality_unknown_selected));
    ibd.push_back(Pair("quality_good_selected", metrics.quality_good_selected));
    ibd.push_back(Pair("quality_degraded_selected", metrics.quality_degraded_selected));
    ibd.push_back(Pair("alternate_announcer_recorded", metrics.alternate_announcer_recorded));
    ibd.push_back(Pair("alternate_announcer_duplicate", metrics.alternate_announcer_duplicate));
    ibd.push_back(Pair("alternate_announcer_evicted", metrics.alternate_announcer_evicted));
    ibd.push_back(Pair("alternate_announcer_expired", metrics.alternate_announcer_expired));
    ibd.push_back(Pair("alternate_announcer_cleanup", metrics.alternate_announcer_cleanup));
    ibd.push_back(Pair("dominant_peer_active_share_current_pct", metrics.dominant_peer_active_share_current_pct));
    ibd.push_back(Pair("dominant_peer_active_share_max_pct", metrics.dominant_peer_active_share_max_pct));
    ibd.push_back(Pair("degraded_peers_current", metrics.degraded_peers_current));
    ibd.push_back(Pair("alternate_hashes_current", metrics.alternate_hashes_current));
    ibd.push_back(Pair("alternate_hashes_max", metrics.alternate_hashes_max));

    ibd.push_back(Pair("askfor_skip_orphan_limit_cooldown", metrics.askfor_skip_orphan_limit_cooldown));
    ibd.push_back(Pair("orphan_limit_cooldown_recorded", metrics.orphan_limit_cooldown_recorded));
    ibd.push_back(Pair("orphan_limit_cross_peer_admitted", metrics.orphan_limit_cross_peer_admitted));
    ibd.push_back(Pair("orphan_limit_frontier_retry_queued", metrics.orphan_limit_frontier_retry_queued));
    ibd.push_back(Pair("orphan_limit_frontier_retry_pending", metrics.orphan_limit_frontier_retry_pending));
    ibd.push_back(Pair("askfor_skip_mapalreadyasked_cap", metrics.askfor_skip_mapalreadyasked_cap));
    ibd.push_back(Pair("askfor_skip_other_peer_owner", metrics.askfor_skip_other_peer_owner));
    ibd.push_back(Pair("askfor_skip_already_queued", metrics.askfor_skip_already_queued));
    ibd.push_back(Pair("askfor_skip_inflight", metrics.askfor_skip_inflight));
    ibd.push_back(Pair("block_owner_receive_mismatch_preserved", metrics.block_owner_receive_mismatch_preserved));

    ibd.push_back(Pair("total_deferred_current", metrics.total_deferred_current));
    ibd.push_back(Pair("total_deferred_max", metrics.total_deferred_max));
    ibd.push_back(Pair("total_queued_current", metrics.total_queued_current));
    ibd.push_back(Pair("total_queued_max", metrics.total_queued_max));
    ibd.push_back(Pair("total_inflight_current", metrics.total_inflight_current));
    ibd.push_back(Pair("total_inflight_max", metrics.total_inflight_max));
    ibd.push_back(Pair("eligible_ahead_peers_current", metrics.eligible_ahead_peers_current));
    ibd.push_back(Pair("eligible_ahead_peers_max", metrics.eligible_ahead_peers_max));

    ibdsemantic::IBDSemanticSnapshot sem;
    ibdsemantic::SnapshotAll(sem);
    Object semantic_health;
    semantic_health.push_back(Pair("enabled", sem.nEnabled));
    semantic_health.push_back(Pair("sample_count", sem.nSampleCount));
    semantic_health.push_back(Pair("window_ready", sem.fWindowReady));
    semantic_health.push_back(Pair("local_height", sem.nLocalHeight));
    semantic_health.push_back(Pair("tip_delta_1m", sem.tip_delta_1m));
    semantic_health.push_back(Pair("tip_delta_5m", sem.tip_delta_5m));
    semantic_health.push_back(Pair("progress_rate_1m", sem.progress_rate_1m));
    semantic_health.push_back(Pair("progress_rate_5m", sem.progress_rate_5m));
    semantic_health.push_back(Pair("progress_median_5m", sem.progress_median_5m));
    semantic_health.push_back(Pair("receive_active", sem.fReceiveActive));
    semantic_health.push_back(Pair("received_1m", sem.received_1m));
    semantic_health.push_back(Pair("orphan_pressure_1m", sem.orphan_pressure_1m));
    semantic_health.push_back(Pair("reject_futility_1m", sem.reject_futility_1m));
    semantic_health.push_back(Pair("pipeline_events_1m", sem.pipeline_events_1m));
    semantic_health.push_back(Pair("eligible_ahead_peers", sem.eligible_ahead_peers));
    semantic_health.push_back(Pair("peer_height_max", sem.peer_height_max));
    semantic_health.push_back(Pair("peer_height_confirmed", sem.peer_height_confirmed));
    semantic_health.push_back(Pair("confirmed_gap_available", sem.fConfirmedGapAvailable));
    semantic_health.push_back(Pair("peer_gap_max", sem.peer_gap_max));
    semantic_health.push_back(Pair("peer_gap_confirmed", sem.peer_gap_confirmed));
    semantic_health.push_back(Pair("peer_gap_change_1m", sem.peer_gap_change_1m));
    semantic_health.push_back(Pair("peer_gap_change_5m", sem.peer_gap_change_5m));
    semantic_health.push_back(Pair("oldest_live_inflight_age_ms", sem.oldest_live_inflight_age_ms));
    semantic_health.push_back(Pair("inflight_gt1s", sem.inflight_gt1s));
    semantic_health.push_back(Pair("inflight_gt4s", sem.inflight_gt4s));
    semantic_health.push_back(Pair("a1_low_progress", sem.a1_low_progress));
    semantic_health.push_back(Pair("a2_receiving", sem.a2_receiving));
    semantic_health.push_back(Pair("a3_orphan_dominant", sem.a3_orphan_dominant));
    semantic_health.push_back(Pair("a3b_not_futile", sem.a3b_not_futile));
    semantic_health.push_back(Pair("a4_pipeline_busy", sem.a4_pipeline_busy));
    semantic_health.push_back(Pair("a5_large_confirmed_gap", sem.a5_large_confirmed_gap));
    semantic_health.push_back(Pair("a6_multiple_ahead_peers", sem.a6_multiple_ahead_peers));
    semantic_health.push_back(Pair("arm_candidate", sem.arm_candidate));
    semantic_health.push_back(Pair("checks_total", sem.semantic_checks_total));
    semantic_health.push_back(Pair("arm_candidate_total", sem.semantic_arm_candidate_total));
    semantic_health.push_back(Pair("a1_true_total", sem.semantic_a1_true_total));
    semantic_health.push_back(Pair("a2_true_total", sem.semantic_a2_true_total));
    semantic_health.push_back(Pair("a3_true_total", sem.semantic_a3_true_total));
    semantic_health.push_back(Pair("a3b_true_total", sem.semantic_a3b_true_total));
    semantic_health.push_back(Pair("a4_true_total", sem.semantic_a4_true_total));
    semantic_health.push_back(Pair("a5_true_total", sem.semantic_a5_true_total));
    semantic_health.push_back(Pair("a6_true_total", sem.semantic_a6_true_total));
    semantic_health.push_back(Pair("all_but_a1_total", sem.semantic_all_but_a1_total));
    semantic_health.push_back(Pair("all_but_a2_total", sem.semantic_all_but_a2_total));
    semantic_health.push_back(Pair("all_but_a3_total", sem.semantic_all_but_a3_total));
    semantic_health.push_back(Pair("all_but_a3b_total", sem.semantic_all_but_a3b_total));
    semantic_health.push_back(Pair("all_but_a4_total", sem.semantic_all_but_a4_total));
    semantic_health.push_back(Pair("all_but_a5_total", sem.semantic_all_but_a5_total));
    semantic_health.push_back(Pair("all_but_a6_total", sem.semantic_all_but_a6_total));
    semantic_health.push_back(Pair("recovery_checks_total", sem.recovery_checks_total));
    semantic_health.push_back(Pair("recovery_skip_not_armed", sem.recovery_skip_not_armed));
    semantic_health.push_back(Pair("recovery_skip_height_changed", sem.recovery_skip_height_changed));
    semantic_health.push_back(Pair("recovery_skip_peer_not_ahead", sem.recovery_skip_peer_not_ahead));
    semantic_health.push_back(Pair("recovery_skip_pipeline_active", sem.recovery_skip_pipeline_active));
    semantic_health.push_back(Pair("recovery_skip_pipeline_active_after_timeout", sem.recovery_skip_pipeline_active_after_timeout));
    semantic_health.push_back(Pair("recovery_skip_timeout_not_reached", sem.recovery_skip_timeout_not_reached));
    semantic_health.push_back(Pair("recovery_skip_cooldown", sem.recovery_skip_cooldown));
    semantic_health.push_back(Pair("recovery_triggered", sem.recovery_triggered));
    semantic_health.push_back(Pair("recovery_armed", sem.recovery_armed));
    semantic_health.push_back(Pair("recovery_last_observed_height", sem.recovery_last_observed_height));
    semantic_health.push_back(Pair("recovery_stall_age_seconds", sem.recovery_stall_age_seconds));
    semantic_health.push_back(Pair("recovery_longest_stall_seconds", sem.recovery_longest_stall_seconds));
    semantic_health.push_back(Pair("recovery_max_peer_height", sem.recovery_max_peer_height));
    semantic_health.push_back(Pair("recovery_peer_gap", sem.recovery_peer_gap));
    semantic_health.push_back(Pair("recovery_pipeline_active", sem.recovery_pipeline_active));
    ibd.push_back(Pair("semantic_health", semantic_health));
    return ibd;
}


// RPC-SPLIT: dedicated IBD diagnostics endpoint. Returns exactly the object
// that getinfo(extended=true) embeds under "ibdmetrics", with no surrounding
// getinfo fields. Reuses the shared builder above.
Value getibdmetrics(const Array& params, bool fHelp)
{
    if (fHelp)
        throw runtime_error(
            "getibdmetrics\n"
            "\nReturns the extended IBD (initial block download) scheduler diagnostics\n"
            "that getinfo with the 'extended' argument exposes under the \"ibdmetrics\" key.\n"
            "Intended for engineering diagnostics, automated monitoring and investigations.\n"
            "\nArguments: none\n"
            "\nResult: (object) the IBD metrics snapshot\n");
    if (params.size() != 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "getibdmetrics: this command takes no arguments");
    return BuildIBDMetricsJson((int)nBestHeight);
}

Value getinfo(const Array& params, bool fHelp)
{
    // RPC-SPLIT: optional boolean "extended" argument (default false). The
    // concise, backward-compatible response omits the large "ibdmetrics"
    // diagnostic object; it is only built/serialized when extended=true.
    if (fHelp)
        throw runtime_error(
            "getinfo ( extended )\n"
            "\nReturns an object containing various state info.\n"
            "\nArguments:\n"
            "1. extended   (boolean, optional, default=false)  when true, include the\n"
            "              complete extended IBD diagnostic object under \"ibdmetrics\"\n"
            "\nResult:\n"
            "{                          (object) concise node state (standard fields)\n"
            "  \"ibdmetrics\"             (object) extended IBD diagnostics; only when extended=true\n"
            "}\n"
            "\nExamples:\n"
            "> innovad getinfo\n"
            "> innovad getinfo true\n");
    if (params.size() > 1)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "getinfo: too many arguments");
    if (params.size() == 1 && params[0].type() != bool_type)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "getinfo: 'extended' argument must be a boolean");
    const bool fExtended = (params.size() == 1) && params[0].get_bool();

    const bool fRPCPerfTrace = GetBoolArg("-rpcperftrace", false);
    const int64_t nRPCStartTime = RPCPerfTimeMicros(fRPCPerfTrace);
    if (fRPCPerfTrace)
        RPCPERF_LOG("RPCPERF rpc=getinfo event=start\n");

    proxyType proxy;
    GetProxy(NET_IPV4, proxy);

    int64_t nMeasuredMicros = 0;
    int64_t nSectionStart = RPCPerfTimeMicros(fRPCPerfTrace);
    uint64_t nMinWeight = 0, nMaxWeight = 0, nWeight = 0;
    pwalletMain->GetStakeWeight(*pwalletMain, nMinWeight, nMaxWeight, nWeight);
    int64_t nSectionMicros = RPCPerfTimeMicros(fRPCPerfTrace) - nSectionStart;
    nMeasuredMicros += nSectionMicros;
    if (fRPCPerfTrace)
        RPCPERF_LOG("RPCPERF rpc=getinfo section=stake_weight duration_us=%lld\n",
               (long long)nSectionMicros);

    int nConnections = 0;
    nSectionStart = RPCPerfTimeMicros(fRPCPerfTrace);
    {
        CSyncLockDiagnostics lockDiagnostics("getinfo", "cs_vNodes");
        LOCK(cs_vNodes);
        lockDiagnostics.Acquired();
        nConnections = (int)vNodes.size();
    }
    nSectionMicros = RPCPerfTimeMicros(fRPCPerfTrace) - nSectionStart;
    nMeasuredMicros += nSectionMicros;
    if (fRPCPerfTrace)
        RPCPERF_LOG("RPCPERF rpc=getinfo section=network_snapshot duration_us=%lld peer_count=%d\n",
               (long long)nSectionMicros, nConnections);

    Object obj, diff;
    obj.push_back(Pair("version",       FormatFullVersion()));
    obj.push_back(Pair("buildcommit",    CLIENT_BUILD_COMMIT));
    obj.push_back(Pair("builddirty",     CLIENT_BUILD_DIRTY));
    obj.push_back(Pair("protocolversion",(int)PROTOCOL_VERSION));
    obj.push_back(Pair("walletversion", pwalletMain->GetVersion()));

    nSectionStart = RPCPerfTimeMicros(fRPCPerfTrace);
    int64_t nBalance = pwalletMain->GetBalance();
    nSectionMicros = RPCPerfTimeMicros(fRPCPerfTrace) - nSectionStart;
    nMeasuredMicros += nSectionMicros;
    if (fRPCPerfTrace)
        RPCPERF_LOG("RPCPERF rpc=getinfo section=balance duration_us=%lld\n", (long long)nSectionMicros);
    obj.push_back(Pair("balance", ValueFromAmount(nBalance)));

    obj.push_back(Pair("reserve", ValueFromAmount(nReserveBalance)));

    nSectionStart = RPCPerfTimeMicros(fRPCPerfTrace);
    int64_t nNewMint = pwalletMain->GetNewMint();
    nSectionMicros = RPCPerfTimeMicros(fRPCPerfTrace) - nSectionStart;
    nMeasuredMicros += nSectionMicros;
    if (fRPCPerfTrace)
        RPCPERF_LOG("RPCPERF rpc=getinfo section=newmint duration_us=%lld\n", (long long)nSectionMicros);
    obj.push_back(Pair("newmint", ValueFromAmount(nNewMint)));

    nSectionStart = RPCPerfTimeMicros(fRPCPerfTrace);
    int64_t nStake = pwalletMain->GetStake();
    nSectionMicros = RPCPerfTimeMicros(fRPCPerfTrace) - nSectionStart;
    nMeasuredMicros += nSectionMicros;
    if (fRPCPerfTrace)
        RPCPERF_LOG("RPCPERF rpc=getinfo section=stake duration_us=%lld\n", (long long)nSectionMicros);
    obj.push_back(Pair("stake", ValueFromAmount(nStake)));

    nSectionStart = RPCPerfTimeMicros(fRPCPerfTrace);
    int64_t nUnconfirmed = pwalletMain->GetUnconfirmedBalance();
    nSectionMicros = RPCPerfTimeMicros(fRPCPerfTrace) - nSectionStart;
    nMeasuredMicros += nSectionMicros;
    if (fRPCPerfTrace)
        RPCPERF_LOG("RPCPERF rpc=getinfo section=unconfirmed duration_us=%lld\n", (long long)nSectionMicros);
    obj.push_back(Pair("unconfirmed", ValueFromAmount(nUnconfirmed)));

    nSectionStart = RPCPerfTimeMicros(fRPCPerfTrace);
    int64_t nImmature = pwalletMain->GetImmatureBalance();
    nSectionMicros = RPCPerfTimeMicros(fRPCPerfTrace) - nSectionStart;
    nMeasuredMicros += nSectionMicros;
    if (fRPCPerfTrace)
        RPCPERF_LOG("RPCPERF rpc=getinfo section=immature duration_us=%lld\n", (long long)nSectionMicros);
    obj.push_back(Pair("immature", ValueFromAmount(nImmature)));

    obj.push_back(Pair("blocks",        (int)nBestHeight));
    obj.push_back(Pair("timeoffset",    (int64_t)GetTimeOffset()));
    // R1 slice-1: best-tip money supply read BY VALUE from the authoritative
    // block-index manager. The historical pindexBest object is NOT dereferenced
    // on the authoritative path (a failure fails closed rather than falling back).
    {
        BlockIndexSnapshot tipSnapshot; std::string tipError;
        if (GetBlockIndexManager().GetTip(&tipSnapshot, &tipError) == BLOCK_INDEX_MANAGER_OK)
            obj.push_back(Pair("moneysupply", ValueFromAmount(tipSnapshot.nMoneySupply)));
        else if (g_fAuthoritativeStartup)
            throw JSONRPCError(RPC_MISC_ERROR, tipError.empty() ? "Money supply unavailable" : tipError);
        else
            obj.push_back(Pair("moneysupply", ValueFromAmount(pindexBest->nMoneySupply)));
    }
    obj.push_back(Pair("connections",   nConnections));
    obj.push_back(Pair("datareceived",  bytesReadable(CNode::GetTotalBytesRecv())));
    obj.push_back(Pair("datasent",      bytesReadable(CNode::GetTotalBytesSent())));
    obj.push_back(Pair("proxy",         (proxy.first.IsValid() ? proxy.first.ToStringIPPort() : string())));
    if(fNativeTor)
    {
        string automatic_onion;
        fs::path const hostname_path = GetDefaultDataDir() / "onion" / "hostname";
        if (!fs::exists(hostname_path))
            printf("No external address found.");
        ifstream file(hostname_path.string().c_str());
        file >> automatic_onion;
        obj.push_back(Pair("ip", automatic_onion));
    }
    if(!fNativeTor)
        obj.push_back(Pair("ip", addrSeenByPeer.ToStringIP()));

    extern bool ManagerDifficultyPair(double* dPoW, double* dPoS);
    double dPoWsi, dPoSsi;
    if (!ManagerDifficultyPair(&dPoWsi, &dPoSsi))
    {
        dPoWsi = GetDifficulty();
        dPoSsi = GetDifficulty(GetLastBlockIndex(pindexBest, true));
    }
    diff.push_back(Pair("proof-of-work", dPoWsi));
    diff.push_back(Pair("proof-of-stake", dPoSsi));
    obj.push_back(Pair("difficulty", diff));

    nSectionStart = RPCPerfTimeMicros(fRPCPerfTrace);
    double dPoWMHashPS = GetPoWMHashPS();
    nSectionMicros = RPCPerfTimeMicros(fRPCPerfTrace) - nSectionStart;
    nMeasuredMicros += nSectionMicros;
    if (fRPCPerfTrace)
        RPCPERF_LOG("RPCPERF rpc=getinfo section=pow_hashrate duration_us=%lld\n", (long long)nSectionMicros);
    obj.push_back(Pair("netmhashps", dPoWMHashPS));

    nSectionStart = RPCPerfTimeMicros(fRPCPerfTrace);
    double dPoSKernelPS = GetPoSKernelPS();
    nSectionMicros = RPCPerfTimeMicros(fRPCPerfTrace) - nSectionStart;
    nMeasuredMicros += nSectionMicros;
    if (fRPCPerfTrace)
        RPCPERF_LOG("RPCPERF rpc=getinfo section=pos_kernel_rate duration_us=%lld\n", (long long)nSectionMicros);
    obj.push_back(Pair("netstakeweight", dPoSKernelPS));
    obj.push_back(Pair("weight", (uint64_t)nWeight));

    obj.push_back(Pair("testnet",       fTestNet));
    obj.push_back(Pair("collateralnode",  fCollateralNode));
    obj.push_back(Pair("cnlock",        fCNLock));
    obj.push_back(Pair("nativetor",     fNativeTor));

    nSectionStart = RPCPerfTimeMicros(fRPCPerfTrace);
    int64_t nKeyPoolOldest = pwalletMain->GetOldestKeyPoolTime();
    nSectionMicros = RPCPerfTimeMicros(fRPCPerfTrace) - nSectionStart;
    nMeasuredMicros += nSectionMicros;
    if (fRPCPerfTrace)
        RPCPERF_LOG("RPCPERF rpc=getinfo section=keypool_oldest duration_us=%lld\n", (long long)nSectionMicros);
    obj.push_back(Pair("keypoololdest", nKeyPoolOldest));

    obj.push_back(Pair("keypoolsize",   (int)pwalletMain->GetKeyPoolSize()));
    obj.push_back(Pair("paytxfee",      ValueFromAmount(nTransactionFee)));
    obj.push_back(Pair("mininput",      ValueFromAmount(nMinimumInputValue)));
    obj.push_back(Pair("datadir",       GetDataDir().string()));
    obj.push_back(Pair("initialblockdownload", IsInitialBlockDownload()));
    if(fDebug)
    {
        obj.push_back(Pair("debug",             fDebug));
        obj.push_back(Pair("debugnet",          fDebugNet));
        obj.push_back(Pair("debugchain",        fDebugChain));
    }
    if (pwalletMain->IsCrypted())
    {
        LOCK(cs_nWalletUnlockTime);
        obj.push_back(Pair("unlocked_until", (int64_t)nWalletUnlockTime / 1000));
    }
    if (!pwalletMain->IsCrypted())
        obj.push_back(Pair("wallet_status", "unencrypted"));
    if (!pwalletMain->IsLocked() && pwalletMain->IsCrypted() && !fWalletUnlockStakingOnly)
        obj.push_back(Pair("wallet_status", "unlocked"));
    if (!pwalletMain->IsLocked() && pwalletMain->IsCrypted() && fWalletUnlockStakingOnly)
        obj.push_back(Pair("wallet_status", "stakingonly"));
    if (pwalletMain->IsLocked() && pwalletMain->IsCrypted())
        obj.push_back(Pair("wallet_status", "locked"));
    obj.push_back(Pair("errors", GetWarnings("statusbar")));

    if (fExtended)
        obj.push_back(Pair("ibdmetrics", BuildIBDMetricsJson((int)nBestHeight)));

    if (fRPCPerfTrace)
    {
        const int64_t nTotalMicros = RPCPerfTimeMicros(true) - nRPCStartTime;
        RPCPERF_LOG("RPCPERF rpc=getinfo section=remaining_handler duration_us=%lld\n",
                    (long long)std::max<int64_t>(0, nTotalMicros - nMeasuredMicros));
        RPCPERF_LOG("RPCPERF rpc=getinfo event=end total_us=%lld\n",
                    (long long)nTotalMicros);
    }
    return obj;
}


// Innova (INN) Wallet Lock Status RPC
// Credit to Carsenk - Original Legatus Legionis - Q0FSU0VOIEtMT0NL
// Q0lSQ1VJVEJSRUFLRVI=
Value walletstatus(const Array& params, bool fHelp)
{
	if (fHelp || params.size() != 0)
        throw runtime_error(
            "walletstatus\n"
			             "Returns the current wallet lock and encryption status.");

	Object obj;
  if (pwalletMain->IsCrypted())
    {
        LOCK(cs_nWalletUnlockTime);
        obj.push_back(Pair("unlocked_until", (int64_t)nWalletUnlockTime / 1000));
    }
	if (!pwalletMain->IsCrypted())
        obj.push_back(Pair("wallet_status", "unencrypted"));
	if (!pwalletMain->IsLocked() && pwalletMain->IsCrypted() && !fWalletUnlockStakingOnly)
		obj.push_back(Pair("wallet_status", "unlocked"));
	if (!pwalletMain->IsLocked() && pwalletMain->IsCrypted() && fWalletUnlockStakingOnly)
		obj.push_back(Pair("wallet_status", "stakingonly"));
	if (pwalletMain->IsLocked() && pwalletMain->IsCrypted())
		obj.push_back(Pair("wallet_status", "locked"));

	return obj;
}

Value getnewpubkey(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "getnewpubkey [account]\n"
            "Returns new public key for coinbase generation.");

    // Parse the account first so we don't generate a key if there's an error
    string strAccount;
    if (params.size() > 0)
        strAccount = AccountFromValue(params[0]);

    if (!pwalletMain->IsLocked())
        pwalletMain->TopUpKeyPool();

    // Generate a new key that is added to wallet
    CPubKey newKey;
    if (!pwalletMain->GetKeyFromPool(newKey, false))
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");
    CKeyID keyID = newKey.GetID();

    pwalletMain->SetAddressBookName(keyID, strAccount);
    vector<unsigned char> vchPubKey = newKey.Raw();

    return HexStr(vchPubKey.begin(), vchPubKey.end());
}


Value getnewaddress(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "getnewaddress [account]\n"
            "Returns a new Innova address for receiving payments.  "
            "If [account] is specified, it is added to the address book "
            "so payments received with the address will be credited to [account].");

    // Parse the account first so we don't generate a key if there's an error
    string strAccount;
    if (params.size() > 0)
        strAccount = AccountFromValue(params[0]);

    if (!pwalletMain->IsLocked())
        pwalletMain->TopUpKeyPool();

    // Generate a new key that is added to wallet
    CPubKey newKey;
    if (!pwalletMain->GetKeyFromPool(newKey, false))
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");
    CKeyID keyID = newKey.GetID();

    pwalletMain->SetAddressBookName(keyID, strAccount);

    return CBitcoinAddress(keyID).ToString();
}


CBitcoinAddress GetAccountAddress(string strAccount, bool bForceNew=false)
{
    CWalletDB walletdb(pwalletMain->strWalletFile);

    CAccount account;
    walletdb.ReadAccount(strAccount, account);

    bool bKeyUsed = false;

    // Check if the current key has been used
    if (account.vchPubKey.IsValid())
    {
        CScript scriptPubKey;
        scriptPubKey.SetDestination(account.vchPubKey.GetID());
        for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin();
             it != pwalletMain->mapWallet.end() && account.vchPubKey.IsValid();
             ++it)
        {
            const CWalletTx& wtx = (*it).second;
            BOOST_FOREACH(const CTxOut& txout, wtx.vout)
                if (txout.scriptPubKey == scriptPubKey)
                    bKeyUsed = true;
        }
    }

    // Generate a new key
    if (!account.vchPubKey.IsValid() || bForceNew || bKeyUsed)
    {
        if (!pwalletMain->GetKeyFromPool(account.vchPubKey, false))
            throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");

        pwalletMain->SetAddressBookName(account.vchPubKey.GetID(), strAccount);
        walletdb.WriteAccount(strAccount, account);
    }

    return CBitcoinAddress(account.vchPubKey.GetID());
}

Value getaccountaddress(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "getaccountaddress <account>\n"
            "Returns the current Innova address for receiving payments to this account.");

    // Parse the account first so we don't generate a key if there's an error
    string strAccount = AccountFromValue(params[0]);

    Value ret;

    ret = GetAccountAddress(strAccount).ToString();

    return ret;
}



Value setaccount(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "setaccount <innovaaddress> <account>\n"
            "Sets the account associated with the given address.");

    CBitcoinAddress address(params[0].get_str());
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Innova address");


    string strAccount;
    if (params.size() > 1)
        strAccount = AccountFromValue(params[1]);

    // Detect when changing the account of an address that is the 'unused current key' of another account:
    if (pwalletMain->mapAddressBook.count(address.Get()))
    {
        string strOldAccount = pwalletMain->mapAddressBook[address.Get()];
        if (address == GetAccountAddress(strOldAccount))
            GetAccountAddress(strOldAccount, true);
    }

    pwalletMain->SetAddressBookName(address.Get(), strAccount);

    return Value::null;
}


Value getaccount(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "getaccount <innovaaddress>\n"
            "Returns the account associated with the given address.");

    CBitcoinAddress address(params[0].get_str());
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Innova address");

    string strAccount;
    map<CTxDestination, string>::iterator mi = pwalletMain->mapAddressBook.find(address.Get());
    if (mi != pwalletMain->mapAddressBook.end() && !(*mi).second.empty())
        strAccount = (*mi).second;
    return strAccount;
}


Value getaddressesbyaccount(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "getaddressesbyaccount <account>\n"
            "Returns the list of addresses for the given account.");

    string strAccount = AccountFromValue(params[0]);

    // Find all addresses that have the given account
    Array ret;
    BOOST_FOREACH(const PAIRTYPE(CBitcoinAddress, string)& item, pwalletMain->mapAddressBook)
    {
        const CBitcoinAddress& address = item.first;
        const string& strName = item.second;
        if (strName == strAccount)
            ret.push_back(address.ToString());
    }
    return ret;
}

Value sendtoaddress(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 5)
        throw runtime_error(
            "sendtoaddress <innovaaddress> <amount> [comment] [comment-to] [narration]\n" // Exchanges use the comments internally...
            "sendtoaddress <innovaaddress> <amount> [narration]\n"
            "<amount> is a real and is rounded to the nearest 0.000001"
            + HelpRequiringPassphrase());

    EnsureWalletIsUnlocked();

    CBitcoinAddress address(params[0].get_str());
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Innova address");

    // Amount
    int64_t nAmount = AmountFromValue(params[1]);

    CWalletTx wtx;

    std::string sNarr;
    if (params.size() > 2 && params[2].type() != null_type && !params[2].get_str().empty())
        sNarr = params[2].get_str();

    if (sNarr.length() > 24)
        throw runtime_error("Narration must be 24 characters or less.");

    // Wallet comments
    if (params.size() > 2 && params[2].type() != null_type && !params[2].get_str().empty())
        wtx.mapValue["comment"] = params[2].get_str();
    if (params.size() > 3 && params[3].type() != null_type && !params[3].get_str().empty())
        wtx.mapValue["to"]      = params[3].get_str();
    if (params.size() > 4 && params[4].type() != null_type && !params[4].get_str().empty())
        sNarr                   = params[4].get_str();
    if (sNarr.length() > 24)
        throw std::runtime_error("Narration must be 24 characters or less.");

    if (pwalletMain->IsLocked())
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");

    string strError = pwalletMain->SendMoneyToDestination(address.Get(), nAmount, sNarr, wtx);
    if (strError != "")
        throw JSONRPCError(RPC_WALLET_ERROR, strError);

    return wtx.GetHash().GetHex();
}

Value burn(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw std::runtime_error(
            "burn amount ( \"comment\" )\n"
            "\nBurn an amount of coins. The amount is a real and is rounded to the nearest 0.00000001\n" +
            HelpRequiringPassphrase() + "\n"

            "\nArguments:\n"
            "1. \"amount\"      (numeric, required) The amount in INN to burn. eg 0.1\n"
            "2. \"comment\"     (string, optional) A comment embedded in the transaction on the blockchain.\n"

            "\nResult:\n"
            "\"transactionid\"  (string) The transaction id.\n"

            "\nExamples:\n" +

            ("burn", "0.1") +
            ("burn", "0.1 \"burn test\"") +
            ("burn", "0.1, \"burn test\""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    // Amount
    CAmount nAmount = AmountFromValue(params[0]);
    if (nAmount > pwalletMain->GetBalance())
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds");

    // Comment
    CWalletTx wtx;
    CScript burnScript = CScript() << OP_RETURN;
    if (params.size() > 1 && !params[1].is_null() && !params[1].get_str().empty()) {
        if (params[1].get_str().length() > MAX_OP_RETURN_RELAY - 3) // Max 45 Bytes
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Comment cannot be longer than %u characters", MAX_OP_RETURN_RELAY - 3));
        burnScript << ToByteVector(params[1].get_str());
    }

    EnsureWalletIsUnlocked();
    CReserveKey reservekey(pwalletMain);
    int64_t nFeeRequired;
    std::string strError = "CreateTransaction() failed.";
    std::string sNarr = "";

    if (!pwalletMain->CreateTransaction(burnScript, nAmount, sNarr, wtx, reservekey, nFeeRequired, nullptr)) {
        if (nAmount + nFeeRequired > pwalletMain->GetBalance())
            strError = "Error: This transaction requires a transaction fee of at least " + FormatMoney(nFeeRequired) + " because of its amount, complexity, or use of recently received funds!";
        printf("BurnCoins() : %s\n", strError.c_str());
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }
    if (!pwalletMain->CommitTransaction(wtx, reservekey))
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: The transaction was rejected! This might happen if some of the coins in your wallet were already spent, such as if you used a copy of wallet.dat and coins were spent in the copy but not marked as spent here.");

    return wtx.GetHash().GetHex();
}


Value listaddressgroupings(const Array& params, bool fHelp)
{
    if (fHelp)
        throw runtime_error(
            "listaddressgroupings\n"
            "Lists groups of addresses which have had their common ownership\n"
            "made public by common use as inputs or as the resulting change\n"
            "in past transactions");

    Array jsonGroupings;
    map<CTxDestination, int64_t> balances = pwalletMain->GetAddressBalances();
    BOOST_FOREACH(set<CTxDestination> grouping, pwalletMain->GetAddressGroupings())
    {
        Array jsonGrouping;
        BOOST_FOREACH(CTxDestination address, grouping)
        {
            Array addressInfo;
            addressInfo.push_back(CBitcoinAddress(address).ToString());
            addressInfo.push_back(ValueFromAmount(balances[address]));
            {
                LOCK(pwalletMain->cs_wallet);
                if (pwalletMain->mapAddressBook.find(CBitcoinAddress(address).Get()) != pwalletMain->mapAddressBook.end())
                    addressInfo.push_back(pwalletMain->mapAddressBook.find(CBitcoinAddress(address).Get())->second);
            }
            jsonGrouping.push_back(addressInfo);
        }
        jsonGroupings.push_back(jsonGrouping);
    }
    return jsonGroupings;
}

Value listaddressgroups(const Array& params, bool fHelp)
{
    if (fHelp)
        throw runtime_error(
            "listaddressgroups\n"
            "Lists groups of addresses which have had their common ownership\n"
            "made public by common use as inputs or as the resulting change\n"
            "in past transactions");


	Object obj, addr;
	Array ret;
    map<CTxDestination, int64_t> balances = pwalletMain->GetAddressBalances();
    BOOST_FOREACH(set<CTxDestination> grouping, pwalletMain->GetAddressGroupings())
    {
        BOOST_FOREACH(CTxDestination address, grouping)
        {
			obj.push_back(Pair("address", CBitcoinAddress(address).ToString()));
			obj.push_back(Pair("amount", ValueFromAmount(balances[address])));
        }
    }
	ret.push_back(obj);
    return ret;
}

Value signmessage(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 2)
        throw runtime_error(
            "signmessage <innovaaddress> <message>\n"
            "Sign a message with the private key of an address");

    EnsureWalletIsUnlocked();

    string strAddress = params[0].get_str();
    string strMessage = params[1].get_str();

    CBitcoinAddress addr(strAddress);
    if (!addr.IsValid())
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");

    CKeyID keyID;
    if (!addr.GetKeyID(keyID))
        throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");

    CKey key;
    if (!pwalletMain->GetKey(keyID, key))
        throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");

    CDataStream ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    vector<unsigned char> vchSig;
    if (!key.SignCompact(Hash(ss.begin(), ss.end()), vchSig))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Sign failed");

    return EncodeBase64(&vchSig[0], vchSig.size());
}

Value verifymessage(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 3)
        throw runtime_error(
            "verifymessage <innovaaddress> <signature> <message>\n"
            "Verify a signed message");

    string strAddress  = params[0].get_str();
    string strSign     = params[1].get_str();
    string strMessage  = params[2].get_str();

    CBitcoinAddress addr(strAddress);
    if (!addr.IsValid())
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");

    CKeyID keyID;
    if (!addr.GetKeyID(keyID))
        throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");

    bool fInvalid = false;
    vector<unsigned char> vchSig = DecodeBase64(strSign.c_str(), &fInvalid);

    if (fInvalid)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Malformed base64 encoding");

    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    CPubKey pubkey;
    if (!pubkey.RecoverCompact(ss.GetHash(), vchSig))
        return false;

    return (pubkey.GetID() == keyID);
}


Value getreceivedbyaddress(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "getreceivedbyaddress <innovaaddress> [minconf=1]\n"
            "Returns the total amount received by <innovaaddress> in transactions with at least [minconf] confirmations.");

    // Bitcoin address
    CBitcoinAddress address = CBitcoinAddress(params[0].get_str());
    CScript scriptPubKey;
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Innova address");
    scriptPubKey.SetDestination(address.Get());
    if (!IsMine(*pwalletMain,scriptPubKey))
        return (double)0.0;

    // Minimum confirmations
    int nMinDepth = 1;
    if (params.size() > 1)
        nMinDepth = params[1].get_int();

    // Tally
    int64_t nAmount = 0;
    for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it)
    {
        const CWalletTx& wtx = (*it).second;
        if (wtx.IsCoinBase() || wtx.IsCoinStake() || !wtx.IsFinal())
            continue;

        BOOST_FOREACH(const CTxOut& txout, wtx.vout)
            if (txout.scriptPubKey == scriptPubKey)
                if (wtx.GetDepthInMainChain() >= nMinDepth)
                    nAmount += txout.nValue;
    }

    return  ValueFromAmount(nAmount);
}


void GetAccountAddresses(string strAccount, set<CTxDestination>& setAddress)
{
    BOOST_FOREACH(const PAIRTYPE(CTxDestination, string)& item, pwalletMain->mapAddressBook)
    {
        const CTxDestination& address = item.first;
        const string& strName = item.second;
        if (strName == strAccount)
            setAddress.insert(address);
    }
}

Value getreceivedbyaccount(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "getreceivedbyaccount <account> [minconf=1]\n"
            "Returns the total amount received by addresses with <account> in transactions with at least [minconf] confirmations.");

    accountingDeprecationCheck();

    // Minimum confirmations
    int nMinDepth = 1;
    if (params.size() > 1)
        nMinDepth = params[1].get_int();

    // Get the set of pub keys assigned to account
    string strAccount = AccountFromValue(params[0]);
    set<CTxDestination> setAddress;
    GetAccountAddresses(strAccount, setAddress);

    // Tally
    int64_t nAmount = 0;
    for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it)
    {
        const CWalletTx& wtx = (*it).second;
        if (wtx.IsCoinBase() || wtx.IsCoinStake() || !wtx.IsFinal())
            continue;

        BOOST_FOREACH(const CTxOut& txout, wtx.vout)
        {
            CTxDestination address;
            if (ExtractDestination(txout.scriptPubKey, address) && IsMine(*pwalletMain, address) && setAddress.count(address))
                if (wtx.GetDepthInMainChain() >= nMinDepth)
                {
                    nAmount += txout.nValue;
                }
        }
    }

    return (double)nAmount / (double)COIN;
}


int64_t GetAccountBalance(CWalletDB& walletdb, const string& strAccount, int nMinDepth, const isminefilter& filter)
{
    int64_t nBalance = 0;

    // Tally wallet transactions
    for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it)
    {
        const CWalletTx& wtx = (*it).second;
        if (!wtx.IsFinal() || wtx.GetDepthInMainChain() < 0)
            continue;

        int64_t nReceived, nSent, nFee;
        wtx.GetAccountAmounts(strAccount, nReceived, nSent, nFee, filter);

        if (nReceived != 0 && wtx.GetDepthInMainChain() >= nMinDepth && wtx.GetBlocksToMaturity() == 0)
            nBalance += nReceived;
        nBalance -= nSent + nFee;
    }

    // Tally internal accounting entries
    nBalance += walletdb.GetAccountCreditDebit(strAccount);

    return nBalance;
}

int64_t GetAccountBalance(const string& strAccount, int nMinDepth, const isminefilter& filter)
{
    CWalletDB walletdb(pwalletMain->strWalletFile);
    return GetAccountBalance(walletdb, strAccount, nMinDepth, filter);
}

//I n n o v a v2.5.2 fetchbalance RPC Command
Value fetchbalance(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "fetchbalance\n"
            "Returns an object containing various wallet balance info.");
    Object obj, watchonly;
    obj.push_back(Pair("totalbalance",  ValueFromAmount(pwalletMain->GetBalance())));
    obj.push_back(Pair("locked",        ValueFromAmount(pwalletMain->GetLockedBalance())));
    obj.push_back(Pair("unlocked",      ValueFromAmount(pwalletMain->GetUnlockedBalance())));
    obj.push_back(Pair("newmint",       ValueFromAmount(pwalletMain->GetNewMint())));
    obj.push_back(Pair("stake",         ValueFromAmount(pwalletMain->GetStake())));
    obj.push_back(Pair("stakeable",     ValueFromAmount(pwalletMain->GetStakeAmount())));
    obj.push_back(Pair("immature",      ValueFromAmount(pwalletMain->GetImmatureBalance())));
    obj.push_back(Pair("unconfirmed",   ValueFromAmount(pwalletMain->GetUnconfirmedBalance())));

    watchonly.push_back(Pair("balance",     ValueFromAmount(pwalletMain->GetWatchOnlyBalance())));
    watchonly.push_back(Pair("unconfirmed", ValueFromAmount(pwalletMain->GetUnconfirmedWatchOnlyBalance())));
    watchonly.push_back(Pair("immature",     ValueFromAmount(pwalletMain->GetImmatureWatchOnlyBalance())));
    obj.push_back(Pair("watchonly",    watchonly));

    return obj;
}

Value getbalance(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 3)
        throw runtime_error(
            "getbalance ( \"account\" minconf includeWatchonly )\n"
            "\nIf account is not specified, returns the server's total available balance.\n"
            "If account is specified, returns the balance in the account.\n"
            "Note that the account \"\" is not the same as leaving the parameter out.\n"
            "The server total may be different to the balance in the default \"\" account.\n"
            "\nArguments:\n"
            "1. \"account\"      (string, optional) The selected account, or \"*\" for entire wallet. It may be the default account using \"\".\n"
            "2. minconf          (numeric, optional, default=1) Only include transactions confirmed at least this many times.\n"
            "3. includeWatchonly (bool, optional, default=false) Also include balance in watchonly addresses (see 'importaddress')\n"
            "\nResult:\n"
            "amount              (numeric) The total amount in btc received for this account.\n");

    if (params.size() == 0)
        return  ValueFromAmount(pwalletMain->GetBalance());

    int nMinDepth = 1;
    if (params.size() > 1)
        nMinDepth = params[1].get_int();

    isminefilter filter = ISMINE_SPENDABLE;

    if(params.size() > 2)
        if(params[2].get_bool())
            filter = filter | ISMINE_WATCH_ONLY;

    if (params[0].get_str() == "*") {
        // Calculate total balance a different way from GetBalance()
        // (GetBalance() sums up all unspent TxOuts)
        // getbalance and "getbalance * 1 true" should return the same number
        int64_t nBalance = 0;
        for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it)
        {
            const CWalletTx& wtx = (*it).second;
            if (!wtx.IsTrusted())
                continue;

            int64_t allFee;
            string strSentAccount;
            list<COutputEntry> listReceived;
            list<COutputEntry> listSent;
            wtx.GetAmounts(listReceived, listSent, allFee, strSentAccount, filter);
            if (wtx.GetDepthInMainChain() >= nMinDepth && wtx.GetBlocksToMaturity() == 0)
            {
                BOOST_FOREACH(const COutputEntry& r, listReceived)
                    nBalance += r.amount;
            }
            BOOST_FOREACH(const COutputEntry& s, listSent)
                nBalance -= s.amount;
            nBalance -= allFee;
        }
        return  ValueFromAmount(nBalance);
    }

    accountingDeprecationCheck();

    string strAccount = AccountFromValue(params[0]);

    int64_t nBalance = GetAccountBalance(strAccount, nMinDepth, filter);

    return ValueFromAmount(nBalance);
}


Value movecmd(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 3 || params.size() > 5)
        throw runtime_error(
            "move <fromaccount> <toaccount> <amount> [minconf=1] [comment]\n"
            "Move from one account in your wallet to another.");

    accountingDeprecationCheck();

    string strFrom = AccountFromValue(params[0]);
    string strTo = AccountFromValue(params[1]);
    int64_t nAmount = AmountFromValue(params[2]);

    if (params.size() > 3)
        // unused parameter, used to be nMinDepth, keep type-checking it though
        (void)params[3].get_int();
    string strComment;
    if (params.size() > 4)
        strComment = params[4].get_str();

    CWalletDB walletdb(pwalletMain->strWalletFile);
    if (!walletdb.TxnBegin())
        throw JSONRPCError(RPC_DATABASE_ERROR, "database error");

    int64_t nNow = GetAdjustedTime();

    // Debit
    CAccountingEntry debit;
    debit.nOrderPos = pwalletMain->IncOrderPosNext(&walletdb);
    debit.strAccount = strFrom;
    debit.nCreditDebit = -nAmount;
    debit.nTime = nNow;
    debit.strOtherAccount = strTo;
    debit.strComment = strComment;
    walletdb.WriteAccountingEntry(debit);

    // Credit
    CAccountingEntry credit;
    credit.nOrderPos = pwalletMain->IncOrderPosNext(&walletdb);
    credit.strAccount = strTo;
    credit.nCreditDebit = nAmount;
    credit.nTime = nNow;
    credit.strOtherAccount = strFrom;
    credit.strComment = strComment;
    walletdb.WriteAccountingEntry(credit);

    if (!walletdb.TxnCommit())
        throw JSONRPCError(RPC_DATABASE_ERROR, "database error");

    return true;
}


Value sendfrom(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 3 || params.size() > 7)
        throw runtime_error(
            "sendfrom <fromaccount> <toinnovaaddress> <amount> [minconf=1] [comment] [comment-to] [narration] \n"
            "<amount> is a real and is rounded to the nearest 0.000001"
            + HelpRequiringPassphrase());

    EnsureWalletIsUnlocked();

    string strAccount = AccountFromValue(params[0]);
    CBitcoinAddress address(params[1].get_str());
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Innova address");
    int64_t nAmount = AmountFromValue(params[2]);

    int nMinDepth = 1;
    if (params.size() > 3)
        nMinDepth = params[3].get_int();

    CWalletTx wtx;
    wtx.strFromAccount = strAccount;

    if (params.size() > 4 && params[4].type() != null_type && !params[4].get_str().empty())
        wtx.mapValue["comment"] = params[4].get_str();
    if (params.size() > 5 && params[5].type() != null_type && !params[5].get_str().empty())
        wtx.mapValue["to"]      = params[5].get_str();

    std::string sNarr;
    if (params.size() > 6 && params[6].type() != null_type && !params[6].get_str().empty())
        sNarr = params[6].get_str();

    if (sNarr.length() > 24)
        throw std::runtime_error("Narration must be 24 characters or less.");

    // Check funds
    int64_t nBalance = GetAccountBalance(strAccount, nMinDepth, ISMINE_SPENDABLE);
    if (nAmount > nBalance)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Account has insufficient funds");

    // Send
    string strError = pwalletMain->SendMoneyToDestination(address.Get(), nAmount, sNarr, wtx);
    if (strError != "")
        throw JSONRPCError(RPC_WALLET_ERROR, strError);

    return wtx.GetHash().GetHex();
}


Value sendmany(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 4)
        throw runtime_error(
            "sendmany <fromaccount> {address:amount,...} [minconf=1] [comment]\n"
            "amounts are double-precision floating point numbers"
            + HelpRequiringPassphrase());

    string strAccount = AccountFromValue(params[0]);
    Object sendTo = params[1].get_obj();
    int nMinDepth = 1;
    if (params.size() > 2)
        nMinDepth = params[2].get_int();

    CWalletTx wtx;
    wtx.strFromAccount = strAccount;
    if (params.size() > 3 && params[3].type() != null_type && !params[3].get_str().empty())
        wtx.mapValue["comment"] = params[3].get_str();

    set<CBitcoinAddress> setAddress;
    vector<pair<CScript, int64_t> > vecSend;

    int64_t totalAmount = 0;
    BOOST_FOREACH(const Pair& s, sendTo)
    {
        CBitcoinAddress address(s.name_);
        if (!address.IsValid())
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Innova address");

        if (setAddress.count(address))
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, duplicated address");
        setAddress.insert(address);

        CScript scriptPubKey;
        scriptPubKey.SetDestination(address.Get());
        int64_t nAmount = AmountFromValue(s.value_);

        totalAmount += nAmount;

        vecSend.push_back(make_pair(scriptPubKey, nAmount));
    }

    EnsureWalletIsUnlocked();

    // Check funds
    int64_t nBalance = GetAccountBalance(strAccount, nMinDepth, ISMINE_SPENDABLE);
    if (totalAmount > nBalance)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Account has insufficient funds");

    // Send
    CReserveKey keyChange(pwalletMain);
    int64_t nFeeRequired = 0;
    int nChangePos;

    bool fCreated = pwalletMain->CreateTransaction(vecSend, wtx, keyChange, nFeeRequired, nChangePos);
    if (!fCreated)
    {
        if (totalAmount + nFeeRequired > pwalletMain->GetBalance())
            throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds");
        throw JSONRPCError(RPC_WALLET_ERROR, "Transaction creation failed");
    }
    if (!pwalletMain->CommitTransaction(wtx, keyChange))
        throw JSONRPCError(RPC_WALLET_ERROR, "Transaction commit failed");

    return wtx.GetHash().GetHex();
}

Value addmultisigaddress(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 3)
    {
        string msg = "addmultisigaddress <nrequired> <'[\"key\",\"key\"]'> [account]\n"
            "Add a nrequired-to-sign multisignature address to the wallet\"\n"
            "each key is a Innova address or hex-encoded public key\n"
            "If [account] is specified, assign address to [account].";
        throw runtime_error(msg);
    }

    int nRequired = params[0].get_int();
    const Array& keys = params[1].get_array();
    string strAccount;
    if (params.size() > 2)
        strAccount = AccountFromValue(params[2]);

    // Gather public keys
    if (nRequired < 1)
        throw runtime_error("a multisignature address must require at least one key to redeem");
    if ((int)keys.size() < nRequired)
        throw runtime_error(
            strprintf("not enough keys supplied "
                      "(got %" PRIszu" keys, but need at least %d to redeem)", keys.size(), nRequired));
    std::vector<CKey> pubkeys;
    pubkeys.resize(keys.size());
    for (unsigned int i = 0; i < keys.size(); i++)
    {
        const std::string& ks = keys[i].get_str();

        // Case 1: Bitcoin address and we have full public key:
        CBitcoinAddress address(ks);
        if (address.IsValid())
        {
            CKeyID keyID;
            if (!address.GetKeyID(keyID))
                throw runtime_error(
                    strprintf("%s does not refer to a key",ks.c_str()));
            CPubKey vchPubKey;
            if (!pwalletMain->GetPubKey(keyID, vchPubKey))
                throw runtime_error(
                    strprintf("no full public key for address %s",ks.c_str()));
             if (!vchPubKey.IsFullyValid())
                throw runtime_error(" Invalid public key: "+ks);
        }

        // Case 2: hex public key
        else if (IsHex(ks))
        {
            CPubKey vchPubKey(ParseHex(ks));
            if (!vchPubKey.IsFullyValid())
                throw runtime_error(" Invalid public key: "+ks);
        }
        else
        {
            throw runtime_error(" Invalid public key: "+ks);
        }
    }

    // Construct using pay-to-script-hash:
    CScript inner;
    inner.SetMultisig(nRequired, pubkeys);
    CScriptID innerID = inner.GetID();
    if (!pwalletMain->AddCScript(inner))
        throw runtime_error("AddCScript() failed");

    pwalletMain->SetAddressBookName(innerID, strAccount);
    return CBitcoinAddress(innerID).ToString();
}

Value addredeemscript(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
    {
        string msg = "addredeemscript <redeemScript> [account]\n"
            "Add a P2SH address with a specified redeemScript to the wallet.\n"
            "If [account] is specified, assign address to [account].";
        throw runtime_error(msg);
    }

    string strAccount;
    if (params.size() > 1)
        strAccount = AccountFromValue(params[1]);

    // Construct using pay-to-script-hash:
    vector<unsigned char> innerData = ParseHexV(params[0], "redeemScript");
    CScript inner(innerData.begin(), innerData.end());
    CScriptID innerID = inner.GetID();
    if (!pwalletMain->AddCScript(inner))
        throw runtime_error("AddCScript() failed");

    pwalletMain->SetAddressBookName(innerID, strAccount);
    return CBitcoinAddress(innerID).ToString();
}

struct tallyitem
{
    int64_t nAmount;
    int nConf;
    vector<uint256> txids;
    bool fIsWatchonly;
    tallyitem()
    {
        nAmount = 0;
        nConf = std::numeric_limits<int>::max();
        fIsWatchonly = false;
    }
};

Value ListReceived(const Array& params, bool fByAccounts)
{
    // Minimum confirmations
    int nMinDepth = 1;
    if (params.size() > 0)
        nMinDepth = params[0].get_int();

    // Whether to include empty accounts
    bool fIncludeEmpty = false;
    if (params.size() > 1)
        fIncludeEmpty = params[1].get_bool();

    isminefilter filter = ISMINE_SPENDABLE;
    if(params.size() > 2)
        if(params[2].get_bool())
            filter = filter | ISMINE_WATCH_ONLY;

    // Tally
    map<CBitcoinAddress, tallyitem> mapTally;
    for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it)
    {
        const CWalletTx& wtx = (*it).second;

        if (wtx.IsCoinBase() || wtx.IsCoinStake() || !IsFinalTx(wtx))
            continue;

        int nDepth = wtx.GetDepthInMainChain();
        if (nDepth < nMinDepth)
            continue;

        BOOST_FOREACH(const CTxOut& txout, wtx.vout)
        {
            CTxDestination address;
            if (!ExtractDestination(txout.scriptPubKey, address))
                continue;

            isminefilter mine = IsMine(*pwalletMain, address);
            if(!(mine & filter))
                continue;

            tallyitem& item = mapTally[address];
            item.nAmount += txout.nValue;
            item.nConf = min(item.nConf, nDepth);
            item.txids.push_back(wtx.GetHash());
            if (mine & MINE_WATCH_ONLY)
                item.fIsWatchonly = true;
        }
    }

    // Reply
    Array ret;
    map<string, tallyitem> mapAccountTally;
    BOOST_FOREACH(const PAIRTYPE(CBitcoinAddress, string)& item, pwalletMain->mapAddressBook)
    {
        const CBitcoinAddress& address = item.first;
        const string& strAccount = item.second;
        map<CBitcoinAddress, tallyitem>::iterator it = mapTally.find(address);
        if (it == mapTally.end() && !fIncludeEmpty)
            continue;

        int64_t nAmount = 0;
        int nConf = std::numeric_limits<int>::max();
        bool fIsWatchonly = false;
        if (it != mapTally.end())
        {
            nAmount = (*it).second.nAmount;
            nConf = (*it).second.nConf;
            fIsWatchonly = (*it).second.fIsWatchonly;
        }

        if (fByAccounts)
        {
            tallyitem& item = mapAccountTally[strAccount];
            item.nAmount += nAmount;
            item.nConf = min(item.nConf, nConf);
            item.fIsWatchonly = fIsWatchonly;
        }
        else
        {
            Object obj;
            if(fIsWatchonly)
                obj.push_back(Pair("involvesWatchonly", true));
            obj.push_back(Pair("address",       address.ToString()));
            obj.push_back(Pair("account",       strAccount));
            obj.push_back(Pair("amount",        ValueFromAmount(nAmount)));
            obj.push_back(Pair("confirmations", (nConf == std::numeric_limits<int>::max() ? 0 : nConf)));
            Array transactions;
            if (it != mapTally.end())
            {
                BOOST_FOREACH(const uint256& item, (*it).second.txids)
                {
                    transactions.push_back(item.GetHex());
                }
            }
            obj.push_back(Pair("txids", transactions));
            ret.push_back(obj);
        }
    }

    if (fByAccounts)
    {
        for (map<string, tallyitem>::iterator it = mapAccountTally.begin(); it != mapAccountTally.end(); ++it)
        {
            int64_t nAmount = (*it).second.nAmount;
            int nConf = (*it).second.nConf;
            Object obj;
            if((*it).second.fIsWatchonly)
                obj.push_back(Pair("involvesWatchonly", true));
            obj.push_back(Pair("account",       (*it).first));
            obj.push_back(Pair("amount",        ValueFromAmount(nAmount)));
            obj.push_back(Pair("confirmations", (nConf == std::numeric_limits<int>::max() ? 0 : nConf)));
            ret.push_back(obj);
        }
    }

    return ret;
}

Value listreceivedbyaddress(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 3)
        throw runtime_error(
            "listreceivedbyaddress ( minconf includeempty includeWatchonly)\n"
            "\nList balances by receiving address.\n"
            "\nArguments:\n"
            "1. minconf       (numeric, optional, default=1) The minimum number of confirmations before payments are included.\n"
            "2. includeempty  (numeric, optional, dafault=false) Whether to include addresses that haven't received any payments.\n"
            "3. includeWatchonly (bool, optional, default=false) Whether to include watchonly addresses (see 'importaddress').\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"involvesWatchonly\" : \"true\",    (bool) Only returned if imported addresses were involved in transaction\n"
            "    \"address\" : \"receivingaddress\",  (string) The receiving address\n"
            "    \"account\" : \"accountname\",       (string) The account of the receiving address. The default account is \"\".\n"
            "    \"amount\" : x.xxx,                  (numeric) The total amount in btc received by the address\n"
            "    \"confirmations\" : n                (numeric) The number of confirmations of the most recent transaction included\n"
            "  }\n"
            "  ,...\n"
            "]\n");

    return ListReceived(params, false);
}

Value listreceivedbyaccount(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 2)
        throw runtime_error(
            "listreceivedbyaccount [minconf=1] [includeempty=false]\n"
            "[minconf] is the minimum number of confirmations before payments are included.\n"
            "[includeempty] whether to include accounts that haven't received any payments.\n"
            "Returns an array of objects containing:\n"
            "  \"account\" : the account of the receiving addresses\n"
            "  \"amount\" : total amount received by addresses with this account\n"
            "  \"confirmations\" : number of confirmations of the most recent transaction included");

    accountingDeprecationCheck();

    return ListReceived(params, true);
}

Value deletetransaction(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
                "deletetransaction <txid>\nNormally used when a transaction cannot be confirmed due to a double spend.\n"
                );

    if (params.size() != 1)
        throw runtime_error("missing txid");

    LOCK2(cs_main, pwalletMain->cs_wallet);
    uint256 hash;
    hash.SetHex(params[0].get_str());
    if (!pwalletMain->mapWallet.count(hash))
        throw runtime_error("transaction not in wallet");

    if (!mempool.exists(hash))
    {
        CTransaction tx;
        uint256 hashBlock = 0;
        if (GetTransaction(hash, tx, hashBlock) && hashBlock != 0)
            throw runtime_error("transaction is already in blockchain");
    }
    CWalletTx wtx = pwalletMain->mapWallet[hash];

    Object result;
    bool ret;

    ret = mempool.remove(wtx);
    result.push_back(Pair("removing tx from memory pool", ret));

    ret = pwalletMain->EraseFromWallet(wtx.GetHash());
    result.push_back(Pair("erasing tx from wallet.dat", ret));

    int nMismatchSpent;
    int64_t nBalanceInQuestion;
    pwalletMain->FixSpentCoins(nMismatchSpent, nBalanceInQuestion);

    if (nMismatchSpent != 0)
    {
        result.push_back(Pair("mismatched spent coins", nMismatchSpent));
        result.push_back(Pair("amount affected by repair", ValueFromAmount(nBalanceInQuestion)));
    }
    result.push_back(Pair("done", "true"));

#ifdef QT_GUI
    // notify GUI
    pwalletMain->UpdatedTransaction(wtx.GetHash());
#endif

    return result;
}

static void MaybePushAddress(Object & entry, const CTxDestination &dest)
{
    CBitcoinAddress addr;
    if (addr.Set(dest))
        entry.push_back(Pair("address", addr.ToString()));
}

void ListTransactions(const CWalletTx& wtx, const string& strAccount, int nMinDepth, bool fLong, Array& ret, const isminefilter& filter)
{
    int64_t nFee;
    string strSentAccount;
    list<COutputEntry> listReceived;
    list<COutputEntry> listSent;

    wtx.GetAmounts(listReceived, listSent, nFee, strSentAccount, filter);

    bool fAllAccounts = (strAccount == string("*"));
    bool involvesWatchonly = wtx.IsFromMe(ISMINE_WATCH_ONLY);

    // Sent
    if ((!wtx.IsCoinStake()) && (!listSent.empty() || nFee != 0) && (fAllAccounts || strAccount == strSentAccount))
    {
        BOOST_FOREACH(const COutputEntry& s, listSent)
        {
            Object entry;
            if(involvesWatchonly || (::IsMine(*pwalletMain, s.destination) & ISMINE_WATCH_ONLY))
                entry.push_back(Pair("involvesWatchonly", true));
            entry.push_back(Pair("account", strSentAccount));
            MaybePushAddress(entry, s.destination);
            entry.push_back(Pair("category", "send"));
            entry.push_back(Pair("amount", ValueFromAmount(-s.amount)));
            entry.push_back(Pair("vout", s.vout));
			if (!wtx.IsCoinStake())
				entry.push_back(Pair("fee", ValueFromAmount(-nFee)));
			else if (s.amount == 0 && s.vout == 0)
				continue;
            if (fLong)
                WalletTxToJSON(wtx, entry);
            ret.push_back(entry);
        }
    }

    // Received
    if (listReceived.size() > 0 && wtx.GetDepthInMainChain() >= nMinDepth)
    {
        bool stop = false;
        BOOST_FOREACH(const COutputEntry& r, listReceived)
        {
			std::string account;
            if (pwalletMain->mapAddressBook.count(r.destination))
                account = pwalletMain->mapAddressBook[r.destination];

            if (fAllAccounts || (account == strAccount))
            {
                Object entry;
                if(involvesWatchonly || (::IsMine(*pwalletMain, r.destination) & ISMINE_WATCH_ONLY))
                    entry.push_back(Pair("involvesWatchonly", true));
                entry.push_back(Pair("account", account));
                MaybePushAddress(entry, r.destination);
                if (wtx.IsCoinBase() || wtx.IsCoinStake())
                {
                    if (wtx.GetDepthInMainChain() < 1)
                        entry.push_back(Pair("category", "orphan"));
                    else if (wtx.GetBlocksToMaturity() > 0)
                        entry.push_back(Pair("category", "immature"));
                    else
                        entry.push_back(Pair("category", "generate"));
                }
                else
                {
                    entry.push_back(Pair("category", "receive"));
                }

				// PoW Amount
				if (!wtx.IsCoinStake())
				{
					entry.push_back(Pair("amount", ValueFromAmount(r.amount)));
				}

				// PoS Reward and Amount
                if (wtx.IsCoinStake() && nFee != 0)
                {
					entry.push_back(Pair("amount", ValueFromAmount(r.amount)));
					entry.push_back(Pair("reward", ValueFromAmount(-nFee)));
                    stop = true;
				//CollateralNode PoS Reward - I n n o v a
                } else if (wtx.IsCoinStake() && nFee == 0) {
					entry.push_back(Pair("reward", ValueFromAmount(r.amount)));
					stop = true;
				}

				entry.push_back(Pair("vout", r.vout));

                if (pwalletMain->mapAddressBook.count(r.destination)) {
                    entry.push_back(Pair("label", account));
                }

                if (fLong)
                    WalletTxToJSON(wtx, entry);
                ret.push_back(entry);
            }
            if (stop)
                break;
        }
    }
}

void AcentryToJSON(const CAccountingEntry& acentry, const string& strAccount, Array& ret)
{
    bool fAllAccounts = (strAccount == string("*"));

    if (fAllAccounts || acentry.strAccount == strAccount)
    {
        Object entry;
        entry.push_back(Pair("account", acentry.strAccount));
        entry.push_back(Pair("category", "move"));
        entry.push_back(Pair("time", (int64_t)acentry.nTime));
        entry.push_back(Pair("amount", ValueFromAmount(acentry.nCreditDebit)));
        entry.push_back(Pair("otheraccount", acentry.strOtherAccount));
        entry.push_back(Pair("comment", acentry.strComment));
        ret.push_back(entry);
    }
}

Value listtransactions(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 5)
        throw runtime_error(
            "listtransactions [account] [count=10] [from=0] [watchonly=false] [show_coinstake=1]\n"
            "Returns up to [count] most recent transactions skipping the first [from] transactions for account [account].");

    string strAccount = "*";
    if (params.size() > 0)
        strAccount = params[0].get_str();
    int nCount = 10;
    if (params.size() > 1)
        nCount = params[1].get_int();
    int nFrom = 0;
    if (params.size() > 2)
        nFrom = params[2].get_int();

    isminefilter filter = ISMINE_SPENDABLE;
    if(params.size() > 3)
        if(params[3].get_bool())
            filter = filter | ISMINE_WATCH_ONLY;

	bool fShowCoinstake = true;
    if (params.size() > 4)
    {
        std::string value   = params[4].get_str();
        if (IsStringBoolNegative(value))
            fShowCoinstake = false;
    };

    if (nCount < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative count");
    if (nFrom < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative from");

    Array ret;

    std::list<CAccountingEntry> acentries;
    CWallet::TxItems txOrdered = pwalletMain->OrderedTxItems(acentries, strAccount, fShowCoinstake);

    // iterate backwards until we have nCount items to return:
    for (CWallet::TxItems::reverse_iterator it = txOrdered.rbegin(); it != txOrdered.rend(); ++it)
    {
        CWalletTx *const pwtx = (*it).second.first;
        if (pwtx != 0)
            ListTransactions(*pwtx, strAccount, 0, true, ret, filter);
        CAccountingEntry *const pacentry = (*it).second.second;
        if (pacentry != 0)
            AcentryToJSON(*pacentry, strAccount, ret);

        if ((int)ret.size() >= (nCount+nFrom)) break;
    }
    // ret is newest to oldest

    if (nFrom > (int)ret.size())
        nFrom = ret.size();
    if ((nFrom + nCount) > (int)ret.size())
        nCount = ret.size() - nFrom;
    Array::iterator first = ret.begin();
    std::advance(first, nFrom);
    Array::iterator last = ret.begin();
    std::advance(last, nFrom+nCount);

    if (last != ret.end()) ret.erase(last, ret.end());
    if (first != ret.begin()) ret.erase(ret.begin(), first);

    std::reverse(ret.begin(), ret.end()); // Return oldest to newest

    return ret;
}

Value listaccounts(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "listaccounts [minconf=1]\n"
            "Returns Object that has account names as keys, account balances as values.");

    accountingDeprecationCheck();

    int nMinDepth = 1;
    if (params.size() > 0)
        nMinDepth = params[0].get_int();
    isminefilter includeWatchonly = ISMINE_SPENDABLE;
    if(params.size() > 1)
        if(params[1].get_bool())
            includeWatchonly = includeWatchonly | ISMINE_WATCH_ONLY;

    map<string, int64_t> mapAccountBalances;
    BOOST_FOREACH(const PAIRTYPE(CTxDestination, string)& entry, pwalletMain->mapAddressBook) {
        if (IsMine(*pwalletMain, entry.first) & includeWatchonly) // This address belongs to me
            mapAccountBalances[entry.second] = 0;
    }

    for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it)
    {
        const CWalletTx& wtx = (*it).second;
        int64_t nFee;
        string strSentAccount;
        list<COutputEntry> listReceived;
        list<COutputEntry> listSent;
        int nDepth = wtx.GetDepthInMainChain();
        if (nDepth < 0)
            continue;
        wtx.GetAmounts(listReceived, listSent, nFee, strSentAccount, includeWatchonly);
        mapAccountBalances[strSentAccount] -= nFee;
        BOOST_FOREACH(const COutputEntry& s, listSent)
            mapAccountBalances[strSentAccount] -= s.amount;
        if (nDepth >= nMinDepth && wtx.GetBlocksToMaturity() == 0)
        {
            BOOST_FOREACH(const COutputEntry& r, listReceived)
                if (pwalletMain->mapAddressBook.count(r.destination))
                    mapAccountBalances[pwalletMain->mapAddressBook[r.destination]] += r.amount;
                else
                    mapAccountBalances[""] += r.amount;
        }
    }

    list<CAccountingEntry> acentries;
    CWalletDB(pwalletMain->strWalletFile).ListAccountCreditDebit("*", acentries);
    BOOST_FOREACH(const CAccountingEntry& entry, acentries)
        mapAccountBalances[entry.strAccount] += entry.nCreditDebit;

    Object ret;
    BOOST_FOREACH(const PAIRTYPE(string, int64_t)& accountBalance, mapAccountBalances) {
        ret.push_back(Pair(accountBalance.first, ValueFromAmount(accountBalance.second)));
    }
    return ret;
}

Value listsinceblock(const Array& params, bool fHelp)
{
    if (fHelp)
        throw runtime_error(
            "listsinceblock [blockhash] [target-confirmations]\n"
            "Get all transactions in blocks since block [blockhash], or all transactions if omitted");

    CBlockIndex *pindex = NULL;
    int target_confirms = 1;
    isminefilter filter = ISMINE_SPENDABLE;

    if (params.size() > 0)
    {
        uint256 blockId = 0;

        blockId.SetHex(params[0].get_str());
        pindex = CBlockLocator(blockId).GetBlockIndex();
    }

    if (params.size() > 1)
    {
        target_confirms = params[1].get_int();

        if (target_confirms < 1)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter");
    }

    if(params.size() > 2)
    if(params[2].get_bool())
        filter = filter | ISMINE_WATCH_ONLY;

    int depth = pindex ? (1 + nBestHeight - pindex->nHeight) : -1;

    Array transactions;

    for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); it++)
    {
        CWalletTx tx = (*it).second;

        if (depth == -1 || tx.GetDepthInMainChain() < depth)
            ListTransactions(tx, "*", 0, true, transactions, filter);
    }

    uint256 lastblock;

    if (target_confirms == 1)
    {
        lastblock = hashBestChain;
    }
    else
    {
        int target_height = pindexBest->nHeight + 1 - target_confirms;

        CBlockIndex *block;
        for (block = pindexBest;
             block && block->nHeight > target_height;
             block = block->pprev)  { }

        lastblock = block ? block->GetBlockHash() : 0;
    }

    Object ret;
    ret.push_back(Pair("transactions", transactions));
    ret.push_back(Pair("lastblock", lastblock.GetHex()));

    return ret;
}

Value gettransaction(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "gettransaction <txid>\n"
            "Get detailed information about <txid>");

    uint256 hash;
    hash.SetHex(params[0].get_str());

    // Watch Only
    isminefilter filter = ISMINE_SPENDABLE;
    if(params.size() > 1)
        if(params[1].get_bool())
            filter = filter | ISMINE_WATCH_ONLY;

    Object entry;

    if (pwalletMain->mapWallet.count(hash))
    {
        const CWalletTx& wtx = pwalletMain->mapWallet[hash];

        TxToJSON(wtx, 0, entry);

        int64_t nCredit = wtx.GetCredit(filter);
        int64_t nDebit = wtx.GetDebit(filter);
        int64_t nNet = nCredit - nDebit;
        int64_t nFee = (wtx.IsFromMe(filter) ? wtx.GetValueOut() - nDebit : 0);

        entry.push_back(Pair("amount", ValueFromAmount(nNet - nFee)));
        if (wtx.IsFromMe(filter))
            entry.push_back(Pair("fee", ValueFromAmount(nFee)));

        WalletTxToJSON(wtx, entry);

        Array details;
        ListTransactions(pwalletMain->mapWallet[hash], "*", 0, false, details, filter);
        entry.push_back(Pair("details", details));
    }
    else
    {
        CTransaction tx;
        uint256 hashBlock = 0;
        if (GetTransaction(hash, tx, hashBlock))
        {
            TxToJSON(tx, 0, entry);
            if (hashBlock == 0)
                entry.push_back(Pair("confirmations", 0));
            else
            {
                entry.push_back(Pair("blockhash", hashBlock.GetHex()));
                map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
                if (mi != mapBlockIndex.end() && (*mi).second)
                {
                    CBlockIndex* pindex = (*mi).second;
                    if (pindex->IsInMainChain())
                        entry.push_back(Pair("confirmations", 1 + nBestHeight - pindex->nHeight));
                    else
                        entry.push_back(Pair("confirmations", 0));
                }
            }
        }
        else
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available about transaction");
    }

    return entry;
}


Value backupwallet(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "backupwallet <filename>\n"
            "Safely copies wallet.dat to the backups subdirectory.\n"
            "Only filename is accepted (no paths) for security.\n"
            "Example: backupwallet \"my_backup.dat\"");

    string strFilename = params[0].get_str();
    fs::path safePath = SanitizeWalletPath(strFilename, "backups");

    // Lock wallet during backup to avoid capturing in-flight key material
    bool fWasLocked = true;
    bool fWasEncrypted = pwalletMain->IsCrypted();
    if (fWasEncrypted)
    {
        fWasLocked = pwalletMain->IsLocked();
        if (!fWasLocked)
        {
            pwalletMain->Lock();
        }
    }

    bool fBackupSuccess = BackupWallet(*pwalletMain, safePath.string());

    if (!fBackupSuccess)
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: Wallet backup failed!");

    Object result;
    result.push_back(Pair("path", safePath.string()));
    if (fWasEncrypted && !fWasLocked)
        result.push_back(Pair("warning", "Wallet was locked for backup. Please unlock with walletpassphrase to resume operations."));
    return result;
}


Value keypoolrefill(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "keypoolrefill [new-size]\n"
            "Fills the keypool."
            + HelpRequiringPassphrase());

    unsigned int nSize = max(GetArg("-keypool", 100), (int64_t)0);
    if (params.size() > 0) {
        if (params[0].get_int() < 0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected valid size");
        nSize = (unsigned int) params[0].get_int();
    }

    EnsureWalletIsUnlocked();

    pwalletMain->TopUpKeyPool(nSize);

    if (pwalletMain->GetKeyPoolSize() < nSize)
        throw JSONRPCError(RPC_WALLET_ERROR, "Error refreshing keypool.");

    return Value::null;
}


void ThreadTopUpKeyPool(void* parg)
{
    // Make this thread recognisable as the key-topping-up thread
    RenameThread("innova-key-top");

    pwalletMain->TopUpKeyPool();
}

void ThreadCleanWalletPassphrase(void* parg)
{
    // Make this thread recognisable as the wallet relocking thread
    RenameThread("innova-lock-wa");

    int64_t nMyWakeTime = GetTimeMillis() + *((int64_t*)parg) * 1000;

    ENTER_CRITICAL_SECTION(cs_nWalletUnlockTime);

    if (nWalletUnlockTime == 0)
    {
        nWalletUnlockTime = nMyWakeTime;

        do
        {
            if (nWalletUnlockTime==0)
                break;
            int64_t nToSleep = nWalletUnlockTime - GetTimeMillis();
            if (nToSleep <= 0)
                break;

            LEAVE_CRITICAL_SECTION(cs_nWalletUnlockTime);
            MilliSleep(nToSleep);
            ENTER_CRITICAL_SECTION(cs_nWalletUnlockTime);

        } while(1);

        if (nWalletUnlockTime)
        {
            nWalletUnlockTime = 0;
            pwalletMain->Lock();
        }
    }
    else
    {
        if (nWalletUnlockTime < nMyWakeTime)
            nWalletUnlockTime = nMyWakeTime;
    }

    LEAVE_CRITICAL_SECTION(cs_nWalletUnlockTime);

    delete (int64_t*)parg;
}

Value walletpassphrase(const Array& params, bool fHelp)
{
    if (pwalletMain->IsCrypted() && (fHelp || params.size() < 2 || params.size() > 3))
        throw runtime_error(
            "walletpassphrase <passphrase> <timeout> [stakingonly]\n"
            "Stores the wallet decryption key in memory for <timeout> seconds.\n"
            "if [stakingonly] is true sending functions are disabled.");
    if (fHelp)
        return true;
    if (!pwalletMain->IsCrypted())
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletpassphrase was called.");

    if (!pwalletMain->IsLocked())
        throw JSONRPCError(RPC_WALLET_ALREADY_UNLOCKED, "Error: Wallet is already unlocked, use walletlock first if need to change unlock settings.");

    SecureString strWalletPass = SecureExtractPassphrase(params[0]);

    if (strWalletPass.length() > 0)
    {
        if (!pwalletMain->Unlock(strWalletPass))
            throw JSONRPCError(RPC_WALLET_PASSPHRASE_INCORRECT, "Error: The wallet passphrase entered was incorrect.");
    }
    else
        throw runtime_error(
            "walletpassphrase <passphrase> <timeout>\n"
            "Stores the wallet decryption key in memory for <timeout> seconds.");

    NewThread(ThreadTopUpKeyPool, NULL);
    int64_t* pnSleepTime = new int64_t(params[1].get_int64());
	//LOCK(cs_nWalletUnlockTime);
	//nWalletUnlockTime = GetTime() + pnSleepTime;
    NewThread(ThreadCleanWalletPassphrase, pnSleepTime);

    //fWalletUnlockStakingOnly = false;

    // Innova: if user OS account compromised prevent trivial sendmoney commands
    // if (params.size() > 2 && params[2].get_bool() == true)
        // fWalletUnlockStakingOnly = true;
	if (params.size() > 2)
        fWalletUnlockStakingOnly = params[2].get_bool();
    else
        fWalletUnlockStakingOnly = false;

    return Value::null;
}


Value walletpassphrasechange(const Array& params, bool fHelp)
{
    if (pwalletMain->IsCrypted() && (fHelp || params.size() != 2))
        throw runtime_error(
            "walletpassphrasechange <oldpassphrase> <newpassphrase>\n"
            "Changes the wallet passphrase from <oldpassphrase> to <newpassphrase>.");
    if (fHelp)
        return true;
    if (!pwalletMain->IsCrypted())
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletpassphrasechange was called.");

    SecureString strOldWalletPass = SecureExtractPassphrase(params[0]);
    SecureString strNewWalletPass = SecureExtractPassphrase(params[1]);

    if (strOldWalletPass.length() < 1 || strNewWalletPass.length() < 1)
        throw runtime_error(
            "walletpassphrasechange <oldpassphrase> <newpassphrase>\n"
            "Changes the wallet passphrase from <oldpassphrase> to <newpassphrase>.");

    if (!pwalletMain->ChangeWalletPassphrase(strOldWalletPass, strNewWalletPass))
        throw JSONRPCError(RPC_WALLET_PASSPHRASE_INCORRECT, "Error: The wallet passphrase entered was incorrect.");

    return Value::null;
}


Value walletlock(const Array& params, bool fHelp)
{
    if (pwalletMain->IsCrypted() && (fHelp || params.size() != 0))
        throw runtime_error(
            "walletlock\n"
            "Removes the wallet encryption key from memory, locking the wallet.\n"
            "After calling this method, you will need to call walletpassphrase again\n"
            "before being able to call any methods which require the wallet to be unlocked.");
    if (fHelp)
        return true;
    if (!pwalletMain->IsCrypted())
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletlock was called.");

    {
        LOCK(cs_nWalletUnlockTime);
        pwalletMain->Lock();
        nWalletUnlockTime = 0;
    }

    return Value::null;
}


Value encryptwallet(const Array& params, bool fHelp)
{
    if (!pwalletMain->IsCrypted() && (fHelp || params.size() != 1))
        throw runtime_error(
            "encryptwallet <passphrase>\n"
            "Encrypts the wallet with <passphrase>.");
    if (fHelp)
        return true;
    if (pwalletMain->IsCrypted())
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an encrypted wallet, but encryptwallet was called.");

    SecureString strWalletPass = SecureExtractPassphrase(params[0]);

    if (strWalletPass.length() < 1)
        throw runtime_error(
            "encryptwallet <passphrase>\n"
            "Encrypts the wallet with <passphrase>.");

    if (!pwalletMain->EncryptWallet(strWalletPass))
        throw JSONRPCError(RPC_WALLET_ENCRYPTION_FAILED, "Error: Failed to encrypt the wallet.");

    // BDB seems to have a bad habit of writing old data into
    // slack space in .dat files; that is bad if the old data is
    // unencrypted private keys. So:
    StartShutdown();
    return "wallet encrypted; Innova server stopping, restart to run with encrypted wallet.  The keypool has been flushed, you need to make a new backup.";
}

class DescribeAddressVisitor : public boost::static_visitor<Object>
{
private:
    isminetype mine;
public:
    DescribeAddressVisitor(isminetype mineIn) : mine(mineIn) {}

    Object operator()(const CNoDestination &dest) const { return Object(); }

    Object operator()(const CKeyID &keyID) const {
        Object obj;
        CPubKey vchPubKey;
        //pwalletMain->GetPubKey(keyID, vchPubKey);
        obj.push_back(Pair("isscript", false));
        //obj.push_back(Pair("pubkey", HexStr(vchPubKey.Raw())));
        //obj.push_back(Pair("iscompressed", vchPubKey.IsCompressed()));
        if (mine == MINE_SPENDABLE) {
            pwalletMain->GetPubKey(keyID, vchPubKey);
            obj.push_back(Pair("pubkey", HexStr(vchPubKey.Raw())));
            obj.push_back(Pair("iscompressed", vchPubKey.IsCompressed()));
        }
        return obj;
    }

    Object operator()(const CScriptID &scriptID) const {
        Object obj;
        obj.push_back(Pair("isscript", true));
        if (mine == MINE_SPENDABLE) {
            CScript subscript;
            pwalletMain->GetCScript(scriptID, subscript);
            std::vector<CTxDestination> addresses;
            txnouttype whichType;
            int nRequired;
            ExtractDestinations(subscript, whichType, addresses, nRequired);
            obj.push_back(Pair("script", GetTxnOutputType(whichType)));
            obj.push_back(Pair("hex", HexStr(subscript.begin(), subscript.end())));
            Array a;
            BOOST_FOREACH(const CTxDestination& addr, addresses)
                a.push_back(CBitcoinAddress(addr).ToString());
            obj.push_back(Pair("addresses", a));
            if (whichType == TX_MULTISIG)
                obj.push_back(Pair("sigsrequired", nRequired));
        }
        return obj;
    }

    Object operator()(const CStealthAddress &stxAddr) const {
        Object obj;
        obj.push_back(Pair("todo", true));
        return obj;
    }
};

Value validateaddress(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "validateaddress <innovaaddress>\n"
            "Return information about <innovaaddress>.");

    CBitcoinAddress address(params[0].get_str());
    bool isValid = address.IsValid();

    Object ret;
    ret.push_back(Pair("isvalid", isValid));
    if (isValid)
    {
        CTxDestination dest = address.Get();
        string currentAddress = address.ToString();
        ret.push_back(Pair("address", currentAddress));
        isminetype mine = pwalletMain ? IsMine(*pwalletMain, dest) : MINE_NO;
        ret.push_back(Pair("ismine", mine != MINE_NO));
        if (mine != MINE_NO) {
            ret.push_back(Pair("watchonly", mine == MINE_WATCH_ONLY));
            Object detail = boost::apply_visitor(DescribeAddressVisitor(mine), dest);
            ret.insert(ret.end(), detail.begin(), detail.end());
        }
        if (pwalletMain && pwalletMain->mapAddressBook.count(dest))
            ret.push_back(Pair("account", pwalletMain->mapAddressBook[dest]));
    }
    return ret;
}

Value validatepubkey(const Array& params, bool fHelp)
{
    if (fHelp || !params.size() || params.size() > 2)
        throw runtime_error(
            "validatepubkey <innovapubkey>\n"
            "Return information about <innovapubkey>.");

    std::vector<unsigned char> vchPubKey = ParseHex(params[0].get_str());
    CPubKey pubKey(vchPubKey);

    bool isValid = pubKey.IsValid();
    bool isCompressed = pubKey.IsCompressed();
    CKeyID keyID = pubKey.GetID();

    CBitcoinAddress address;
    address.Set(keyID);

    Object ret;
    ret.push_back(Pair("isvalid", isValid));
    if (isValid)
    {
        CTxDestination dest = address.Get();
        string currentAddress = address.ToString();
        ret.push_back(Pair("address", currentAddress));
        isminetype mine = pwalletMain ? IsMine(*pwalletMain, dest) : MINE_NO;
        ret.push_back(Pair("ismine", mine != MINE_NO));
        ret.push_back(Pair("iscompressed", isCompressed));
        if (mine != MINE_NO) {
            Object detail = boost::apply_visitor(DescribeAddressVisitor(mine), dest);
            ret.insert(ret.end(), detail.begin(), detail.end());
        }
        if (pwalletMain->mapAddressBook.count(dest))
            ret.push_back(Pair("account", pwalletMain->mapAddressBook[dest]));
    }
    return ret;
}

// ppcoin: reserve balance from being staked for network protection
Value reservebalance(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 2)
        throw runtime_error(
            "reservebalance [<reserve> [amount]]\n"
            "<reserve> is true or false to turn balance reserve on or off.\n"
            "<amount> is a real and rounded to cent.\n"
            "Set reserve amount not participating in network protection.\n"
            "If no parameters provided current setting is printed.\n");

    if (params.size() > 0)
    {
        bool fReserve = params[0].get_bool();
        if (fReserve)
        {
            if (params.size() == 1)
                throw runtime_error("must provide amount to reserve balance.\n");
            int64_t nAmount = AmountFromValue(params[1]);
            nAmount = (nAmount / CENT) * CENT;  // round to cent
            if (nAmount < 0)
                throw runtime_error("amount cannot be negative.\n");
            nReserveBalance = nAmount;
        }
        else
        {
            if (params.size() > 1)
                throw runtime_error("cannot specify amount to turn off reserve.\n");
            nReserveBalance = 0;
        }
    }

    Object result;
    result.push_back(Pair("reserve", (nReserveBalance > 0)));
    result.push_back(Pair("amount", ValueFromAmount(nReserveBalance)));
    return result;
}


// ppcoin: check wallet integrity
Value checkwallet(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 0)
        throw runtime_error(
            "checkwallet\n"
            "Check wallet for integrity.\n");

    int nMismatchSpent;
    int64_t nBalanceInQuestion;
    pwalletMain->FixSpentCoins(nMismatchSpent, nBalanceInQuestion, true);
    Object result;
    if (nMismatchSpent == 0)
        result.push_back(Pair("wallet check passed", true));
    else
    {
        result.push_back(Pair("mismatched spent coins", nMismatchSpent));
        result.push_back(Pair("amount in question", ValueFromAmount(nBalanceInQuestion)));
    }
    return result;
}


// ppcoin: repair wallet
Value repairwallet(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 0)
        throw runtime_error(
            "repairwallet\n"
            "Repair wallet if checkwallet reports any problem.\n");

    int nMismatchSpent;
    int64_t nBalanceInQuestion;
    pwalletMain->FixSpentCoins(nMismatchSpent, nBalanceInQuestion);
    Object result;
    if (nMismatchSpent == 0)
        result.push_back(Pair("wallet check passed", true));
    else
    {
        result.push_back(Pair("mismatched spent coins", nMismatchSpent));
        result.push_back(Pair("amount affected by repair", ValueFromAmount(nBalanceInQuestion)));
    }
    return result;
}

// NovaCoin: resend unconfirmed wallet transactions
Value resendtx(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "resendtx\n"
            "Re-send unconfirmed transactions.\n"
        );

    ResendWalletTransactions(true);

    return Value::null;
}

// ppcoin: make a public-private key pair
Value makekeypair(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "makekeypair [prefix]\n"
            "Make a public/private key pair.\n"
            "[prefix] is optional preferred prefix for the public key.\n");

    string strPrefix = "";
    if (params.size() > 0)
        strPrefix = params[0].get_str();

    CKey key;
    key.MakeNewKey(false);

    CPrivKey vchPrivKey = key.GetPrivKey();
    Object result;
    result.push_back(Pair("PrivateKey", HexStr<CPrivKey::iterator>(vchPrivKey.begin(), vchPrivKey.end())));
    result.push_back(Pair("PublicKey", HexStr(key.GetPubKey().Raw())));
    return result;
}



Value getnewstealthaddress(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "getnewstealthaddress [label]\n"
            "Returns a new Innova stealth address for receiving payments anonymously.  ");

    if (pwalletMain->IsLocked())
        throw runtime_error("Failed: Wallet must be unlocked.");

    std::string sLabel;
    if (params.size() > 0)
        sLabel = params[0].get_str();

    CStealthAddress sxAddr;
    std::string sError;
    if (!pwalletMain->NewStealthAddress(sError, sLabel, sxAddr))
        throw runtime_error(std::string("Could get new stealth address: ") + sError);

    if (!pwalletMain->AddStealthAddress(sxAddr))
        throw runtime_error("Could not save to wallet.");

    return sxAddr.Encoded();
}

Value liststealthaddresses(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "liststealthaddresses [show_secrets=0]\n"
            "List owned stealth addresses.");

    bool fShowSecrets = false;

    if (params.size() > 0)
    {
        std::string str = params[0].get_str();

        if (str == "0" || str == "n" || str == "no" || str == "-" || str == "false")
            fShowSecrets = false;
        else
            fShowSecrets = true;
    };

    if (fShowSecrets)
    {
        if (pwalletMain->IsLocked())
            throw runtime_error("Failed: Wallet must be unlocked.");
    };

    Object result;

    std::set<CStealthAddress>::iterator it;
    for (it = pwalletMain->stealthAddresses.begin(); it != pwalletMain->stealthAddresses.end(); ++it)
    {
        if (it->scan_secret.size() < 1)
            continue; // stealth address is not owned

        if (fShowSecrets)
        {
            Object objA;
            objA.push_back(Pair("Label        ", it->label));
            objA.push_back(Pair("Address      ", it->Encoded()));
            objA.push_back(Pair("Scan Secret  ", HexStr(it->scan_secret.begin(), it->scan_secret.end())));
            objA.push_back(Pair("Spend Secret ", HexStr(it->spend_secret.begin(), it->spend_secret.end())));
            result.push_back(Pair("Stealth Address", objA));
        } else
        {
            result.push_back(Pair("Stealth Address", it->Encoded() + " - " + it->label));
        };
    };

    return result;
}

Value importstealthaddress(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 2)
        throw runtime_error(
            "importstealthaddress <scan_secret> <spend_secret> [label]\n"
            "Import an owned stealth addresses.");

    std::string sScanSecret  = params[0].get_str();
    std::string sSpendSecret = params[1].get_str();
    std::string sLabel;

    if (pwalletMain->IsLocked())
    {
        throw runtime_error("Failed: Wallet must be unlocked.");
    }

    if (params.size() > 2)
    {
        sLabel = params[2].get_str();
    };

    std::vector<uint8_t> vchScanSecret;
    std::vector<uint8_t> vchSpendSecret;

    if (IsHex(sScanSecret))
    {
        vchScanSecret = ParseHex(sScanSecret);
    } else
    {
        if (!DecodeBase58(sScanSecret, vchScanSecret))
            throw runtime_error("Could not decode scan secret as hex or base58.");
    };

    if (IsHex(sSpendSecret))
    {
        vchSpendSecret = ParseHex(sSpendSecret);
    } else
    {
        if (!DecodeBase58(sSpendSecret, vchSpendSecret))
            throw runtime_error("Could not decode spend secret as hex or base58.");
    };

    if (vchScanSecret.size() != 32)
        throw runtime_error("Scan secret is not 32 bytes.");
    if (vchSpendSecret.size() != 32)
        throw runtime_error("Spend secret is not 32 bytes.");


    ec_secret scan_secret;
    ec_secret spend_secret;

    memcpy(&scan_secret.e[0], &vchScanSecret[0], 32);
    memcpy(&spend_secret.e[0], &vchSpendSecret[0], 32);

    ec_point scan_pubkey, spend_pubkey;
    if (SecretToPublicKey(scan_secret, scan_pubkey) != 0)
        throw runtime_error("Could not get scan public key.");

    if (SecretToPublicKey(spend_secret, spend_pubkey) != 0)
        throw runtime_error("Could not get spend public key.");


    CStealthAddress sxAddr;
    sxAddr.label = sLabel;
    sxAddr.scan_pubkey = scan_pubkey;
    sxAddr.spend_pubkey = spend_pubkey;

    sxAddr.scan_secret = vchScanSecret;
    sxAddr.spend_secret = vchSpendSecret;

    Object result;
    bool fFound = false;
    // -- find if address already exists
    std::set<CStealthAddress>::iterator it;
    for (it = pwalletMain->stealthAddresses.begin(); it != pwalletMain->stealthAddresses.end(); ++it)
    {
        CStealthAddress &sxAddrIt = const_cast<CStealthAddress&>(*it);
        if (sxAddrIt.scan_pubkey == sxAddr.scan_pubkey
            && sxAddrIt.spend_pubkey == sxAddr.spend_pubkey)
        {
            if (sxAddrIt.scan_secret.size() < 1)
            {
                sxAddrIt.scan_secret = sxAddr.scan_secret;
                sxAddrIt.spend_secret = sxAddr.spend_secret;
                fFound = true; // update stealth address with secrets
                break;
            };

            result.push_back(Pair("result", "Import failed - stealth address exists."));
            return result;
        };
    };

    if (fFound)
    {
        result.push_back(Pair("result", "Success, updated " + sxAddr.Encoded()));
    } else
    {
        pwalletMain->stealthAddresses.insert(sxAddr);
        result.push_back(Pair("result", "Success, imported " + sxAddr.Encoded()));
    };


    if (!pwalletMain->AddStealthAddress(sxAddr))
        throw runtime_error("Could not save to wallet.");

    return result;
}

Value clearwallettransactions(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 0)
        throw runtime_error(
            "clearwallettransactions \n"
            "delete all transactions from wallet - reload with scanforalltxns\n"
            "Warning: Backup your wallet first!");



    Object result;

    uint32_t nTransactions = 0;

    char cbuf[256];

    {
        LOCK2(cs_main, pwalletMain->cs_wallet);

        CWalletDB walletdb(pwalletMain->strWalletFile);
        walletdb.TxnBegin();
        Dbc* pcursor = walletdb.GetTxnCursor();
        if (!pcursor)
            throw runtime_error("Cannot get wallet DB cursor");

        Dbt datKey;
        Dbt datValue;

        datKey.set_flags(DB_DBT_USERMEM);
        datValue.set_flags(DB_DBT_USERMEM);

        std::vector<unsigned char> vchKey;
        std::vector<unsigned char> vchType;
        std::vector<unsigned char> vchKeyData;
        std::vector<unsigned char> vchValueData;

        vchKeyData.resize(100);
        vchValueData.resize(100);

        datKey.set_ulen(vchKeyData.size());
        datKey.set_data(&vchKeyData[0]);

        datValue.set_ulen(vchValueData.size());
        datValue.set_data(&vchValueData[0]);

        unsigned int fFlags = DB_NEXT; // same as using DB_FIRST for new cursor
        while (true)
        {
            int ret = pcursor->get(&datKey, &datValue, fFlags);

            if (ret == ENOMEM
                || ret == DB_BUFFER_SMALL)
            {
                if (datKey.get_size() > datKey.get_ulen())
                {
                    vchKeyData.resize(datKey.get_size());
                    datKey.set_ulen(vchKeyData.size());
                    datKey.set_data(&vchKeyData[0]);
                };

                if (datValue.get_size() > datValue.get_ulen())
                {
                    vchValueData.resize(datValue.get_size());
                    datValue.set_ulen(vchValueData.size());
                    datValue.set_data(&vchValueData[0]);
                };
                // -- try once more, when DB_BUFFER_SMALL cursor is not expected to move
                ret = pcursor->get(&datKey, &datValue, fFlags);
            };

            if (ret == DB_NOTFOUND)
                break;
            else
            if (datKey.get_data() == NULL || datValue.get_data() == NULL
                || ret != 0)
            {
                snprintf(cbuf, sizeof(cbuf), "wallet DB error %d, %s", ret, db_strerror(ret));
                throw runtime_error(cbuf);
            };

            CDataStream ssValue(SER_DISK, CLIENT_VERSION);
            ssValue.SetType(SER_DISK);
            ssValue.clear();
            ssValue.write((char*)datKey.get_data(), datKey.get_size());

            ssValue >> vchType;


            std::string strType(vchType.begin(), vchType.end());

            //printf("strType %s\n", strType.c_str());

            if (strType == "tx")
            {
                uint256 hash;
                ssValue >> hash;

                if ((ret = pcursor->del(0)) != 0)
                {
                    printf("Delete transaction failed %d, %s\n", ret, db_strerror(ret));
                    continue;
                };

                pwalletMain->mapWallet.erase(hash);
                pwalletMain->NotifyTransactionChanged(pwalletMain, hash, CT_DELETED);

                nTransactions++;
            };
        };
        pcursor->close();
        walletdb.TxnCommit();


        //pwalletMain->mapWallet.clear();
    }

    snprintf(cbuf, sizeof(cbuf), "Removed %u transactions.", nTransactions);
    result.push_back(Pair("complete", std::string(cbuf)));
    result.push_back(Pair("", "Reload the wallet or re-download the blockchain."));


    return result;
}

Value scanforalltxns(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "scanforalltxns [fromHeight]\n"
            "Scan blockchain for owned transactions.");

    Object result;
    int32_t nFromHeight = 0;

    CBlockIndex *pindex = pindexGenesisBlock;


    if (params.size() > 0)
        nFromHeight = params[0].get_int();


    if (nFromHeight > 0)
    {
        pindex = mapBlockIndex[hashBestChain];
        while (pindex->nHeight > nFromHeight
            && pindex->pprev)
            pindex = pindex->pprev;
    };

    if (pindex == NULL)
        throw runtime_error("Genesis Block is not set.");

    {
        LOCK2(cs_main, pwalletMain->cs_wallet);

        pwalletMain->MarkDirty();

        pwalletMain->ScanForWalletTransactions(pindex, true);
        pwalletMain->ReacceptWalletTransactions();
    }

    result.push_back(Pair("result", "Scan complete."));

    return result;
}

static bool compareTxnTime(const CWalletTx* pa, const CWalletTx* pb)
{
    return pa->nTime < pb->nTime;
};

namespace {
// Generic local-time formatter. Formerly provided by the retired SMSG/NYX
// smessage.cpp; rehomed here as the only surviving consumer is txnreport().
std::string getTimeString(int64_t timestamp, char *buffer, size_t nBuffer)
{
    struct tm* dt;
    time_t t = timestamp;
    dt = localtime(&t);
    strftime(buffer, nBuffer, "%Y-%m-%d %H:%M:%S %z", dt);
    return std::string(buffer);
}
}


Value getnewstakingaddress(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 0)
        throw runtime_error(
            "getnewstakingaddress\n"
            "Returns a new Innova staking address for receiving cold staking delegations.\n");

    if (pwalletMain->IsLocked())
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");

    CPubKey newKey;
    if (!pwalletMain->GetKeyFromPool(newKey, false))
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");

    CKeyID keyID = newKey.GetID();

    CBitcoinAddress stakingAddr;
    stakingAddr.SetStaking(keyID);

    // Register in address book so it persists and shows in receive list
    pwalletMain->SetAddressBookName(keyID, "Staking Address");

    return stakingAddr.ToString();
}

Value delegatestake(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 3)
        throw runtime_error(
            "delegatestake <stakeraddress> <amount> [owneraddress]\n"
            "Delegate coins to a staker for cold staking.\n"
            "<stakeraddress> is the staking address of the hot node.\n"
            "<amount> is the amount to delegate.\n"
            "[owneraddress] optional, defaults to a new address from this wallet.\n"
            "Returns the transaction ID.\n");

    if (pwalletMain->IsLocked())
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");

    {
        LOCK(cs_main);
        if (!pindexBest || pindexBest->nHeight < FORK_HEIGHT_COLD_STAKING)
            throw JSONRPCError(RPC_MISC_ERROR, "Cold staking is not yet active on this chain");
    }

    CBitcoinAddress stakerAddr(params[0].get_str());
    if (!stakerAddr.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid staker address");

    CKeyID stakerKeyID;
    if (!stakerAddr.GetKeyID(stakerKeyID))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Staker address must be a key address, not a script address");

    int64_t nAmount = AmountFromValue(params[1]);
    if (nAmount <= 0)
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount");

    CKeyID ownerKeyID;
    if (params.size() > 2)
    {
        CBitcoinAddress ownerAddr(params[2].get_str());
        if (!ownerAddr.IsValid())
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid owner address");
        if (!ownerAddr.GetKeyID(ownerKeyID))
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Owner address must be a key address");
    }
    else
    {
        CPubKey newKey;
        if (!pwalletMain->GetKeyFromPool(newKey, false))
            throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");
        ownerKeyID = newKey.GetID();
    }

    if (stakerKeyID == ownerKeyID)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Staker address and owner address must be different");

    CWalletTx wtx;
    string strError;
    if (!pwalletMain->CreateColdStakeDelegation(stakerKeyID, ownerKeyID, nAmount, wtx, strError))
        throw JSONRPCError(RPC_WALLET_ERROR, strError);

    Object result;
    result.push_back(Pair("txid", wtx.GetHash().GetHex()));

    CBitcoinAddress ownerAddrOut;
    ownerAddrOut.Set(ownerKeyID);
    result.push_back(Pair("owner_address", ownerAddrOut.ToString()));

    CBitcoinAddress stakerAddrOut;
    stakerAddrOut.SetStaking(stakerKeyID);
    result.push_back(Pair("staker_address", stakerAddrOut.ToString()));

    return result;
}

Value listcoldutxos(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "listcoldutxos [onlystaker]\n"
            "List cold staking UTXOs in the wallet.\n"
            "[onlystaker] if true, only show UTXOs where this wallet is the staker (default: false)\n");

    bool fOnlyStaker = false;
    if (params.size() > 0)
        fOnlyStaker = params[0].get_bool();

    Array results;
    {
        LOCK2(cs_main, pwalletMain->cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = pwalletMain->mapWallet.begin();
             it != pwalletMain->mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;
            if (!pcoin->IsFinal() || !pcoin->IsTrusted())
                continue;

            for (unsigned int i = 0; i < pcoin->vout.size(); i++)
            {
                if (pcoin->IsSpent(i))
                    continue;

                if (!IsPayToColdStaking(pcoin->vout[i].scriptPubKey))
                    continue;

                CKeyID stakerKeyID, ownerKeyID;
                if (!ExtractColdStakeKeys(pcoin->vout[i].scriptPubKey, stakerKeyID, ownerKeyID))
                    continue;

                bool fIsStaker = pwalletMain->HaveKey(stakerKeyID);
                bool fIsOwner = pwalletMain->HaveKey(ownerKeyID);

                if (fOnlyStaker && !fIsStaker)
                    continue;

                if (!fIsStaker && !fIsOwner)
                    continue;

                Object entry;
                entry.push_back(Pair("txid", pcoin->GetHash().GetHex()));
                entry.push_back(Pair("vout", (int)i));
                entry.push_back(Pair("amount", ValueFromAmount(pcoin->vout[i].nValue)));

                CBitcoinAddress stakerAddr;
                stakerAddr.SetStaking(stakerKeyID);
                entry.push_back(Pair("staker_address", stakerAddr.ToString()));

                CBitcoinAddress ownerAddr;
                ownerAddr.Set(ownerKeyID);
                entry.push_back(Pair("owner_address", ownerAddr.ToString()));

                entry.push_back(Pair("is_staker", fIsStaker));
                entry.push_back(Pair("is_owner", fIsOwner));

                int nDepth = pcoin->GetDepthInMainChain();
                entry.push_back(Pair("confirmations", nDepth));

                results.push_back(entry);
            }
        }
    }
    return results;
}

Value getcoldstakinginfo(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getcoldstakinginfo\n"
            "Returns information about cold staking.\n");

    Object result;
    result.push_back(Pair("enabled", (int)nBestHeight >= FORK_HEIGHT_COLD_STAKING));
    result.push_back(Pair("fork_height", FORK_HEIGHT_COLD_STAKING));
    result.push_back(Pair("current_height", (int)nBestHeight));

    int64_t nColdStakingBalance = pwalletMain->GetColdStakingBalance();
    result.push_back(Pair("cold_staking_balance", ValueFromAmount(nColdStakingBalance)));

    int nStakerCount = 0;
    int nOwnerCount = 0;
    {
        LOCK2(cs_main, pwalletMain->cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = pwalletMain->mapWallet.begin();
             it != pwalletMain->mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;
            if (!pcoin->IsFinal() || !pcoin->IsTrusted())
                continue;

            for (unsigned int i = 0; i < pcoin->vout.size(); i++)
            {
                if (pcoin->IsSpent(i))
                    continue;
                if (!IsPayToColdStaking(pcoin->vout[i].scriptPubKey))
                    continue;

                CKeyID stakerKeyID, ownerKeyID;
                if (!ExtractColdStakeKeys(pcoin->vout[i].scriptPubKey, stakerKeyID, ownerKeyID))
                    continue;

                if (pwalletMain->HaveKey(stakerKeyID))
                    nStakerCount++;
                if (pwalletMain->HaveKey(ownerKeyID))
                    nOwnerCount++;
            }
        }
    }

    result.push_back(Pair("staker_utxo_count", nStakerCount));
    result.push_back(Pair("owner_utxo_count", nOwnerCount));

    return result;
}

Value revokecoldstaking(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 3)
        throw runtime_error(
            "revokecoldstaking <txid> <vout> [toaddress]\n"
            "Revoke a cold staking delegation and send funds back to owner.\n"
            "<txid> is the transaction ID of the P2CS UTXO.\n"
            "<vout> is the output index of the P2CS UTXO.\n"
            "[toaddress] optional destination address (default: new address from wallet).\n");

    if (pwalletMain->IsLocked())
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");

    uint256 txid;
    txid.SetHex(params[0].get_str());
    unsigned int nVout = params[1].get_int();

    CWalletTx wtxPrev;
    if (!pwalletMain->GetTransaction(txid, wtxPrev))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction not found in wallet");

    if (nVout >= wtxPrev.vout.size())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid vout index");

    const CTxOut& prevOut = wtxPrev.vout[nVout];
    if (!IsPayToColdStaking(prevOut.scriptPubKey))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Output is not a cold staking (P2CS) output");

    if (wtxPrev.IsSpent(nVout))
        throw JSONRPCError(RPC_WALLET_ERROR, "Output is already spent");

    CKeyID stakerKeyID, ownerKeyID;
    if (!ExtractColdStakeKeys(prevOut.scriptPubKey, stakerKeyID, ownerKeyID))
        throw JSONRPCError(RPC_WALLET_ERROR, "Failed to extract cold stake keys");

    if (!pwalletMain->HaveKey(ownerKeyID))
        throw JSONRPCError(RPC_WALLET_ERROR, "This wallet does not own the cold stake owner key");

    CScript scriptDest;
    if (params.size() > 2)
    {
        CBitcoinAddress destAddr(params[2].get_str());
        if (!destAddr.IsValid())
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid destination address");
        scriptDest = GetScriptForDestination(destAddr.Get());
    }
    else
    {
        CPubKey newKey;
        if (!pwalletMain->GetKeyFromPool(newKey, false))
            throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");
        scriptDest = GetScriptForDestination(newKey.GetID());
    }

    CTransaction txNew;
    txNew.vin.push_back(CTxIn(txid, nVout));

    {
        LOCK(cs_main);
        if (pindexBest)
            txNew.nLockTime = pindexBest->nHeight;
    }

    int64_t nFee = MIN_TX_FEE;
    int64_t nValue = prevOut.nValue - nFee;
    if (nValue <= 0)
        throw JSONRPCError(RPC_WALLET_ERROR, "Output value too small to cover fee");

    txNew.vout.push_back(CTxOut(nValue, scriptDest));

    if (!SignSignature(*pwalletMain, wtxPrev, txNew, 0))
        throw JSONRPCError(RPC_WALLET_ERROR, "Failed to sign revocation transaction");

    if (!VerifyScript(txNew.vin[0].scriptSig, prevOut.scriptPubKey, txNew, 0, STANDARD_SCRIPT_VERIFY_FLAGS, 0))
        throw JSONRPCError(RPC_WALLET_ERROR, "Revocation transaction signature verification failed");

    CWalletTx wtxNew;
    wtxNew.fFromMe = true;
    *static_cast<CTransaction*>(&wtxNew) = txNew;

    CReserveKey reservekey(pwalletMain);
    if (!pwalletMain->CommitTransaction(wtxNew, reservekey))
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: The revocation transaction was rejected");

    Object result;
    result.push_back(Pair("txid", wtxNew.GetHash().GetHex()));

    CBitcoinAddress ownerAddr;
    ownerAddr.Set(ownerKeyID);
    result.push_back(Pair("owner_address", ownerAddr.ToString()));
    result.push_back(Pair("revoked_amount", ValueFromAmount(prevOut.nValue)));
    result.push_back(Pair("fee", ValueFromAmount(nFee)));

    return result;
}
