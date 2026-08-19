// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Serving-side getblocks -> inv ZERO-CONSUMPTION suppression.
//
// When -getblocksservedinvzero is enabled, a strict-inbound, non-whitelisted
// peer that repeatedly asks for getblocks inventory over an overlapping range
// at an UNCHANGED chain tip without EVER consuming any of the served inventory
// (no getdata matching a recently served window) stops being served: the
// getblocks inv reply is dropped without writing anything to the socket.
//
// Consumption (getdata matching a recently served window) is the load-bearing
// conservative signal.  A peer that consumes ANYTHING (even a sparse 0.75% of
// the served window) is never suppressed by this mechanism.  The mechanism
// never throttles or affects getdata -> block serving, never increments
// penalties/Misbehaving, and never disconnects.  With the flag off the
// behavior is byte-for-byte identical to legacy.
//
// State is kept per CNode in CNode::getBlocksServedInv (value members), so
// peers are independent and state dies with the connection.  All state access
// happens while cs_main is held (both the getblocks handler and the
// getdata-serving path hold cs_main), so no new lock is introduced.
#ifndef INNOVA_GETBLOCKSSERVEDINVZERO_H
#define INNOVA_GETBLOCKSSERVEDINVZERO_H

#include <stdint.h>

#include <vector>

#include "net.h"
#include "uint256.h"

// ---------------------------------------------------------------------------
// Policy constants.  Named, with defaults, for later A/B tuning; they are
// policy thresholds, not proven truth.  Each one can be overridden at startup
// via a -getblocksservedinv* argument or, in tests, via
// SetGetBlocksServedInvZeroConfig().
// ---------------------------------------------------------------------------

// GRACE: a peer must sit on the same (unchanged-tip) served window for this
// long before its zero-consumption repeats become suppressible.  Guards the
// slow-path where a legitimate peer has not yet had time to turn inv into
// getdata (getblocks -> inv -> getdata round trips).
static const int64_t GETBLOCKS_SERVED_INV_GRACE_S = 60;

// MIN_ITEMS: cumulative inv items served to the peer over the current window
// below which nothing is suppressed.  Empty and small (<MIN_ITEMS) replies
// are never suppressed.
static const uint64_t GETBLOCKS_SERVED_INV_MIN_ITEMS = 2000;

// INITIAL_STREAK: consecutive zero-consumption qualifying getblocks requests
// required before suppression fires for a peer with NO prior zero-consumption
// history.
static const unsigned int GETBLOCKS_SERVED_INV_INITIAL_STREAK = 3;

// REENTRY_STREAK: required streak for a peer WITH prior zero-consumption
// history (nGbPriorZeroConsume).  A safety release re-arms serving but does
// not erase history, so 2000INV -> 1getdata -> repeat cannot trivially reset
// protection.
static const unsigned int GETBLOCKS_SERVED_INV_REENTRY_STREAK = 1;

// RECENT_WINDOW_CAP: hard per-peer bound on retained recently-served windows.
// Prevents unbounded growth under a peer that cycles through many ranges.
static const size_t GETBLOCKS_SERVED_INV_RECENT_WINDOW_CAP = 8;

// W: expiry (seconds) of a recently-served window entry.  A served window
// older than W no longer participates in consumption matching or overlap.
static const int64_t GETBLOCKS_SERVED_INV_WINDOW_EXPIRY_S = 120;

// Runtime-tunable policy values.  Defaults are the named constants above.
struct GetBlocksServedInvZeroConfig
{
    int64_t nGraceS;              // GRACE
    uint64_t nMinItems;           // MIN_ITEMS
    unsigned int nInitialStreak;  // INITIAL_STREAK
    unsigned int nReentryStreak;  // REENTRY_STREAK
    size_t nRecentWindowCap;      // RECENT_WINDOW_CAP
    int64_t nWindowExpiryS;       // W

    GetBlocksServedInvZeroConfig();
};

// Set the module-scope enable flag (called once from AppInit2 before
// networking threads start; afterwards immutable).  Also (re)reads the
// -getblocksservedinv* tuning arguments into the active config.
void InitGetBlocksServedInvZero(bool fEnabled);

bool GetBlocksServedInvZeroEnabled();

const GetBlocksServedInvZeroConfig& GetBlocksServedInvConfig();

// Override the active policy config (used by tests and, at startup, by
// InitGetBlocksServedInvZero).
void SetGetBlocksServedInvZeroConfig(const GetBlocksServedInvZeroConfig& cfg);

// Per-request decision of the served-inv suppression evaluation.
struct GetBlocksServedInvDecision
{
    bool fSuppress;   // drop this inv reply, nothing written to the socket
    bool fQualify;    // zero-consumption qualifying request (streak +1)
    bool fDisarm;     // context legitimately changed; streak/window reset
    uint64_t nItemsAvoided; // predicted items not sent on suppression
    uint64_t nBytesAvoided; // predicted payload bytes not sent on suppression

    GetBlocksServedInvDecision();
};

// Evaluate one getblocks request against the peer's served-inv state.  Must
// be called while cs_main is held (the getblocks handler already holds it).
// Returns fSuppress=true only when ALL of the following hold:
//   1. -getblocksservedinvzero is enabled;
//   2. the peer is strict-inbound and not whitelisted;
//   3. request.hashChainTip == last served response chain tip
//      (a changed tip disarms instead);
//   4. the current window is at least GRACE old;
//   5. nGbServedInvItems >= MIN_ITEMS;
//   6. nGbGetDataMatches == 0 (zero consumption -- the binding signal);
//   7. the request's predicted served window overlaps a recently served
//      window (a genuinely-new range is still served);
//   8. nGbZeroConsumeStreak (this request included) >= the required streak
//      (INITIAL_STREAK normally, REENTRY_STREAK with prior zero-consume
//      history).
// On disarm (changed tip / no overlap) the window and streak are reset so a
// new-range or changed-tip request is served until re-evidenced.
GetBlocksServedInvDecision GetBlocksServedInvEvaluate(
    CNode::CGetBlocksServedInvState& state,
    const CGetBlocksRequestInfo& request,
    bool fStrictInbound,
    int64_t nNowUs,
    uint64_t nPredictedItems);

// Called once per inv item actually pushed to the peer in the getblocks reply
// path: accumulates the current-window item/byte counters and stamps the
// window start on the first item of a window.
void GetBlocksServedInvRecordItem(CNode::CGetBlocksServedInvState& state,
                                  int64_t nNowUs);

// Called once per served getblocks reply (after the response was built and
// its items recorded): appends the served window to the bounded recent set
// (with expiry and cap) and records the response chain tip.
void GetBlocksServedInvRecordResponse(
    CNode::CGetBlocksServedInvState& state,
    const CGetBlocksResponseInfo& response,
    const uint256& hashChainTip,
    int64_t nNowUs);

// Called from the getdata-serving path for each block getdata that matches a
// recently served window: increments the window's consumption counter and, if
// inv suppression is currently armed, performs a SAFETY RELEASE (re-arms
// serving).  Never affects block serving itself.  Returns true when this
// consumption triggered a safety release.
bool GetBlocksServedInvNoteGetData(CNode::CGetBlocksServedInvState& state,
                                   const uint256& hashBlock, int nHeight,
                                   int64_t nNowUs);

// True when the request's predicted served window height-ranges overlap any
// recently served (unexpired) window.  Exposed for tests.
bool GetBlocksServedInvWindowOverlaps(
    const CNode::CGetBlocksServedInvState& state,
    const CGetBlocksRequestInfo& request,
    int64_t nNowUs);


// ---------------------------------------------------------------------------
// Reconnect-persistent zero-consumption debt.
//
// A tiny, bounded, short-TTL, CNetAddr-keyed "debt" so that a same-peer
// reconnect (which destroys the per-CNode evidence window) does not trivially
// reset all zero-consumption evidence.  Only ZERO-consumption evidence is ever
// carried (the conservative boundary); a peer that consumes (any matches>0) is
// never carried, and its existing debt is cleared by real consumption.
//
// Bootstrap safety invariant (MUST hold): inherited debt LOWERS re-qualification
// cost (items primed toward MIN_ITEMS + re-entry streak) but must NEVER start a
// new connection already suppressed.  A reconnecting peer always gets a genuine
// serving opportunity first: the primed window is fresh, so GRACE must elapse
// (and, on this connection, the peer must re-demonstrate zero consumption)
// before any suppression can fire.  Inheritance therefore can never block the
// first necessary INV -> getdata -> block round trip.
// ---------------------------------------------------------------------------
struct CGetBlocksServedInvReconnectDebtEntry
{
    uint64_t nDebtItems;
    unsigned int nDebtStreak;
    int64_t nLastSeenUs;

    CGetBlocksServedInvReconnectDebtEntry()
        : nDebtItems(0), nDebtStreak(0), nLastSeenUs(0)
    {
    }
};
enum
{
    GETBLOCKS_SERVED_INV_RECONNECT_DEBT_CAP = 1024
};
static const int64_t GETBLOCKS_SERVED_INV_RECONNECT_DEBT_TTL_US =
    60LL * 1000000LL;

// Write on CNode teardown: only when the peer ends with real zero-consumption
// evidence (suppressed, OR >= MIN_ITEMS served with zero matches and prior
// zero-consumption history).  Never created for low-but-nonzero consumers.
void GetBlocksServedInvReconnectWrite(const CNode* pnode, int64_t nNowUs);

// Core of Write, exposed for tests: takes the peer's zero-consumption state and
// the IP key directly (no CNode needed).
void GetBlocksServedInvReconnectWriteState(
    const CNode::CGetBlocksServedInvState& state, const CNetAddr& addr,
    int64_t nNowUs);

// Prime a new connection's per-CNode state from the bounded IP-keyed debt (if
// any within TTL).  fPrimed=true only when warm debt existed and was applied.
// Must never set fSuppressInv=true.
void GetBlocksServedInvReconnectPrime(
    CNode::CGetBlocksServedInvState& state, const CNetAddr& addr,
    int64_t nNowUs, bool& fPrimed);

// Decrement/clear the IP-keyed debt when the peer demonstrates real
// consumption (a matching getdata -> block) so a consuming peer never leaks
// zero-consumption debt into future reconnects.
void GetBlocksServedInvReconnectClearedByConsumption(const CNetAddr& addr,
                                                     int64_t nNowUs);

// Introspection (tests).
size_t GetBlocksServedInvReconnectSize();
void GetBlocksServedInvReconnectClearAll();

#endif // INNOVA_GETBLOCKSSERVEDINVZERO_H
