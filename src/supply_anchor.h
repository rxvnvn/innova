// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef INNOVA_SUPPLY_ANCHOR_H
#define INNOVA_SUPPLY_ANCHOR_H

// ===========================================================================
// Money-supply anchors (accounting-only, never consensus).
//
// WHY THIS EXISTS
// ---------------
// nMoneySupply is a DERIVED accumulator: ConnectBlock computes it from the
// parent's cumulative supply plus the block's own (valueOut - valueIn - burned).
// That carrier has no absolute reference anywhere in the node, so a single
// wrong carry (a released parent edge read as zero, a stale/duplicate in-memory
// index object, a re-materialized side snapshot) rebases the whole active-chain
// accumulator and is then internally SELF-CONSISTENT forever: every later
// per-block delta identity holds, so the publication-time invariant gate
// (SUPPLY-INVARIANT, MS-FIX3) cannot see it -- it anchors on a fork record read
// from the same poisoned store.
//
// The anchors below are an INDEPENDENT reference: satoshi-exact cumulative
// supply per canonical block, computed from the chain's own block bytes
// (valueOut/valueIn from prevouts resolved through the Store3A undo stream and
// the block window), NOT from wallet balances and NOT from the node's own
// nMoneySupply field.
//
// Provenance of the table (2026-11-08 pass):
//   tool  : bi-supply-audit v2 (read-only, block-data replay)
//   report: moneysupply-regression-20261108 (this pass, ~/data/reports/innova)
//   rows  : bi-supply-audit.rows.csv (52,607 active heights, 0 unresolved
//           prevouts, pre-fork recurrence validated on 36,552 heights)
//   Each entry is bound to BOTH the height AND the canonical block hash, so a
//   legitimate reorganisation at that height simply makes the anchor
//   INAPPLICABLE (skip) instead of raising a false violation.
//
// An anchor NEVER participates in block acceptance, validation or consensus.
// It only lets the node (a) detect that its persisted accumulator has already
// diverged and (b) refuse to report an unverified number as a success.
// ===========================================================================

#include <stdint.h>
#include <stddef.h>
#include <string>

struct MoneySupplyAnchor
{
    int32_t     height;     // canonical height the anchor describes
    const char* hashHex;    // canonical block hash at that height (display hex)
    int64_t     supply;     // expected cumulative nMoneySupply (satoshi)
};

// The immutable anchor table (sorted by height, strictly increasing).
const MoneySupplyAnchor* MoneySupplyAnchorTable(size_t* count);

// Highest anchor at or below `height` (NULL when none).
const MoneySupplyAnchor* FindMoneySupplyAnchorAtOrBelow(int32_t height);

// Highest anchor in [lo, hi] (NULL when the range contains none).
const MoneySupplyAnchor* FindMoneySupplyAnchorInRange(int32_t lo, int32_t hi);

// Strictly-greater anchor helpers (used to detect a checkpoint crossing).
const MoneySupplyAnchor* FindMoneySupplyAnchorAbove(int32_t height);

// Highest anchor strictly BELOW `height` (used when an anchor is not applicable
// because the chain legitimately reorganized at that height).
const MoneySupplyAnchor* FindMoneySupplyAnchorBelow(int32_t height);

// ---- verification state (per process) -------------------------------------
// UNKNOWN  : verification not attempted (legacy/non-authoritative operation)
// VERIFIED : every applicable anchor matched the persisted active record
// VIOLATION: at least one applicable anchor disagreed (the accumulator has
//            diverged from the chain's own block data) -- values reported as
//            "moneysupply" must NOT be presented as a successful result.
enum MoneySupplyVerificationState
{
    MONEY_SUPPLY_UNKNOWN = 0,
    MONEY_SUPPLY_VERIFIED = 1,
    MONEY_SUPPLY_VIOLATION = 2,
    // The persisted accumulator could not be COMPARED with an anchor: the anchor
    // or the active record could not be read. Distinct from VIOLATION, which is a
    // proven numeric disagreement. Both fail closed (no number is reported), but
    // an operator must be able to tell "data disagrees" from "could not read".
    MONEY_SUPPLY_UNREADABLE = 3,
};

void SetMoneySupplyVerification(int state, const std::string& detail);
int  GetMoneySupplyVerification(std::string* detail = NULL);
const char* MoneySupplyVerificationName(int state);

#endif // INNOVA_SUPPLY_ANCHOR_H
