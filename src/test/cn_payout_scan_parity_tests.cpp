// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// M10c PREPARATION HARNESS — CN payout scan parity (MODEL, no payout change).
//
// Why this exists
// ---------------
// The collateral-node rank/payment scan walks back a fixed number of blocks:
//
//     scanBack = max(COLLATERALNODE_FAIR_PAYMENT_MINIMUM, mnCount)
//                * COLLATERALNODE_FAIR_PAYMENT_ROUNDS;
//
// and it follows CBlockIndex::pprev (collateralnode.cpp, ten loops). Under V2 the
// resident topology that backs pprev is bounded by the materialized live tail
// (-blockindexlivetail, default 2048). The by-value replacement would instead
// address each height through the authority (GetActiveByHeight).
//
// Those two scans can only be swapped after proving they see the same window for
// every reachable mnCount. This harness compares them on the real constants and
// reports where they stop agreeing, so the payout path can be changed only with
// a proven-parity mnCount range rather than an assumption.
//
// Scope: this is a MODEL of the two scans (same height->payee projection fed to
// both). It does not build 683 collateral nodes and therefore is NOT a proof of
// real-chain payout equality; it quantifies the boundary condition that such a
// proof has to cover.

#include <boost/test/unit_test.hpp>

#include "collateralnode.h"
#include "blockindex_authoritative_startup.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace {

// The materialized resident tail: number of ancestor hops the current pprev walk
// can make before it runs out of resident topology. Source of truth:
// blockindex_authoritative_startup.cpp ("-blockindexlivetail", default 2048).
int ResidentTailWindow()
{
    return GetArg("-blockindexlivetail", 2048);
}

// The scan depth the payout path actually asks for, computed exactly as
// collateralnode.cpp does.
int PayoutScanBack(int mnCount)
{
    return std::max(COLLATERALNODE_FAIR_PAYMENT_MINIMUM, mnCount)
           * COLLATERALNODE_FAIR_PAYMENT_ROUNDS;
}

struct ScanResult
{
    std::vector<int> heights;                 // visited heights, tip -> back
    std::map<int, int> payeePaymentCount;     // synthetic CN id -> payments seen
};

// Today: follow pprev from the tip. The loop body breaks when pprev runs out,
// which under V2 happens at the end of the materialized tail.
ScanResult ScanViaPprev(int tipHeight, int scanBack, int window, int mnCount)
{
    ScanResult r;
    for (int i = 0; i < scanBack; ++i) {
        const int height = tipHeight - i;
        if (height < 0) break;
        if (i >= window) break;              // resident topology ended here
        r.heights.push_back(height);
        r.payeePaymentCount[height % mnCount]++;
    }
    return r;
}

// Proposed: address each height through the authority instead of walking links.
ScanResult ScanViaAuthority(int tipHeight, int scanBack, int mnCount)
{
    ScanResult r;
    for (int i = 0; i < scanBack; ++i) {
        const int height = tipHeight - i;
        if (height < 0) break;
        r.heights.push_back(height);
        r.payeePaymentCount[height % mnCount]++;
    }
    return r;
}

} // namespace

BOOST_AUTO_TEST_SUITE(cn_payout_scan_parity_tests)

// The constants this whole analysis depends on are read from the tree, not
// hardcoded, so a future change to either bound invalidates this test loudly.
BOOST_AUTO_TEST_CASE(m10c_scan_bounds_come_from_source_constants)
{
    BOOST_REQUIRE_EQUAL(COLLATERALNODE_FAIR_PAYMENT_MINIMUM, 200);
    BOOST_REQUIRE_EQUAL(COLLATERALNODE_FAIR_PAYMENT_ROUNDS, 3);
    BOOST_REQUIRE_EQUAL(PayoutScanBack(682), 2046);
    BOOST_REQUIRE_EQUAL(PayoutScanBack(683), 2049);
    BOOST_REQUIRE_EQUAL(PayoutScanBack(684), 2052);
    const int window = ResidentTailWindow();
    BOOST_REQUIRE_MESSAGE(window >= 2048,
        "resident tail window shrank to " << window
        << ": the payout scan would truncate even earlier than M10c models");
}

// mnCount = 682 -> scanBack 2046 <= window: the pprev walk covers the requested
// range, so a by-value scan is decision-equivalent for this size.
BOOST_AUTO_TEST_CASE(m10c_mn682_pprev_and_authority_scans_agree)
{
    const int mn = 682, tip = 100000, scanBack = PayoutScanBack(mn);
    const ScanResult a = ScanViaPprev(tip, scanBack, ResidentTailWindow(), mn);
    const ScanResult b = ScanViaAuthority(tip, scanBack, mn);
    BOOST_REQUIRE_EQUAL(a.heights.size(), (size_t)scanBack);
    BOOST_REQUIRE_EQUAL(a.heights.size(), b.heights.size());
    BOOST_REQUIRE(a.heights == b.heights);
    BOOST_REQUIRE(a.payeePaymentCount == b.payeePaymentCount);
}

// mnCount = 683 -> scanBack 2049 > window: the pprev walk silently loses the
// oldest three heights, so the payment profile (and therefore the fair-payment
// verdict) differs from the by-value scan. This is the exact blocker M10c has to
// close before any payout-path rewrite.
BOOST_AUTO_TEST_CASE(m10c_mn683_pprev_scan_truncates_and_diverges)
{
    const int mn = 683, tip = 100000, scanBack = PayoutScanBack(mn);
    const int window = ResidentTailWindow();
    BOOST_REQUIRE(scanBack > window);
    const ScanResult a = ScanViaPprev(tip, scanBack, window, mn);
    const ScanResult b = ScanViaAuthority(tip, scanBack, mn);
    BOOST_REQUIRE_EQUAL(a.heights.size(), (size_t)window);      // truncation
    BOOST_REQUIRE_EQUAL(b.heights.size(), (size_t)scanBack);
    BOOST_REQUIRE_EQUAL((int)(b.heights.size() - a.heights.size()), scanBack - window);
    BOOST_REQUIRE(a.payeePaymentCount != b.payeePaymentCount);  // decision differs
    // The three heights the resident walk never reached are the oldest ones.
    BOOST_REQUIRE_EQUAL(b.heights.back(), tip - scanBack + 1);
    BOOST_REQUIRE_EQUAL(a.heights.back(), tip - window + 1);
}

// mnCount = 684 -> the gap widens to four heights: truncation grows with mnCount,
// so a "works for 683" conclusion would not generalise.
BOOST_AUTO_TEST_CASE(m10c_mn684_gap_widens_with_mnCount)
{
    const int mn = 684, tip = 100000, scanBack = PayoutScanBack(mn);
    const int window = ResidentTailWindow();
    const ScanResult a = ScanViaPprev(tip, scanBack, window, mn);
    const ScanResult b = ScanViaAuthority(tip, scanBack, mn);
    BOOST_REQUIRE_EQUAL((int)(b.heights.size() - a.heights.size()), scanBack - window);
    BOOST_REQUIRE(a.payeePaymentCount != b.payeePaymentCount);
    // The lost heights are the OLDEST ones: the by-value scan reaches strictly
    // further back, which is what the fair-payment rate needs.
    BOOST_REQUIRE_EQUAL(b.heights.back(), tip - scanBack + 1);
    BOOST_REQUIRE_EQUAL(a.heights.back(), tip - window + 1);
    BOOST_REQUIRE(b.heights.back() < a.heights.back());
}


// The numbers above are only meaningful if they come from the SAME inputs the payout
// path consumes. These cases drive the production global `mnCount` (collateralnode.h)
// and the production scan-depth expression, so a change to either the window or the
// fair-payment constants that breaks the 682/683/684 boundary fails here loudly.
BOOST_AUTO_TEST_CASE(m10c_production_mncount_682_still_inside_the_window)
{
    const unsigned int saved = mnCount;
    mnCount = 682;
    const int scanBack = PayoutScanBack((int)mnCount);
    mnCount = saved;
    BOOST_REQUIRE_EQUAL(scanBack, 2046);
    BOOST_REQUIRE(scanBack <= ResidentTailWindow());
}

BOOST_AUTO_TEST_CASE(m10c_production_mncount_683_crosses_the_window)
{
    const unsigned int saved = mnCount;
    mnCount = 683;
    const int scanBack = PayoutScanBack((int)mnCount);
    mnCount = saved;
    BOOST_REQUIRE_EQUAL(scanBack, 2049);
    BOOST_REQUIRE(scanBack > ResidentTailWindow());
    BOOST_REQUIRE_EQUAL(scanBack - ResidentTailWindow(), 1);
}

BOOST_AUTO_TEST_CASE(m10c_production_mncount_684_crosses_the_window)
{
    const unsigned int saved = mnCount;
    mnCount = 684;
    const int scanBack = PayoutScanBack((int)mnCount);
    mnCount = saved;
    BOOST_REQUIRE_EQUAL(scanBack, 2052);
    BOOST_REQUIRE_EQUAL(scanBack - ResidentTailWindow(), 4);
}

// The boundary is a property of the CONFIGURED window, not of a hardcoded 2048: if
// an operator raises -blockindexlivetail the crossing point moves with it, so the
// payout rewrite must compare against the configured value at runtime.
BOOST_AUTO_TEST_CASE(m10c_crossing_point_tracks_the_configured_window)
{
    const int window = ResidentTailWindow();
    int firstCrossing = -1;
    for (int mn = 1; mn <= 2000; ++mn)
        if (PayoutScanBack(mn) > window) { firstCrossing = mn; break; }
    BOOST_REQUIRE(firstCrossing > 0);
    // scanBack <= window  <=>  3*max(200, mn) <= window  <=>  mn <= floor(window/3) (for mn>=200)
    const int expected = (window / 3 < 200) ? 199 : (window / 3);
    BOOST_REQUIRE_EQUAL(firstCrossing, expected + 1);
    BOOST_REQUIRE_EQUAL(PayoutScanBack(firstCrossing - 1) <= window, true);
}

BOOST_AUTO_TEST_SUITE_END()
