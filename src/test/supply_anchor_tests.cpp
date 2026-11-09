// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.
//
// ===========================================================================
// MS-ANCHOR — independent money-supply verification.
// ===========================================================================
// The per-block identity (supply(h) == supply(h-1) + mint - fees - burned) is
// satisfied by ANY consistently re-based chain, so the publication-time gate
// cannot detect an accumulator that was poisoned once and then grew normally.
// These anchors are satoshi-exact cumulative supplies derived from the chain's
// OWN block bytes; a disagreement is proof of divergence.
//
// Covered here (fast, deterministic, no daemon):
//   * table invariants (sorted, monotonic, plausible increments, well-formed hashes);
//   * height lookup helpers around a >8,000,000-height chain;
//   * verification state publication;
//   * matching anchor -> VERIFIED, mismatching value -> VIOLATION and fail-closed;
//   * a legitimate reorganisation at an anchored height -> anchor SKIPPED, the
//     next lower anchor decides (never a false violation);
//   * unresolvable anchor record -> fail closed;
//   * no applicable anchor -> UNKNOWN (never silently "verified");
//   * repeated verification (restart-equivalence shape) is stable.
// ===========================================================================

#include <boost/test/unit_test.hpp>

#include "supply_anchor.h"
#include "main.h"
#include "blockindex_accessor.h"
#include "util.h"

#include <string>
#include <vector>
#include <map>

BOOST_AUTO_TEST_SUITE(supply_anchor_tests)

// ---- fake active-by-height resolver (test seam) ----------------------------
namespace {

struct SAFakeChain
{
    // height -> (hash, supply)
    std::map<int32_t, std::pair<std::string, int64_t> > byHeight;
    bool fail = false;
};

SAFakeChain* g_saFake = NULL;

bool SAResolve(int32_t height, BlockIndexSnapshot* out, std::string* error)
{
    if (g_saFake == NULL || g_saFake->fail)
    {
        if (error) *error = "injected resolver failure";
        return false;
    }
    std::map<int32_t, std::pair<std::string, int64_t> >::const_iterator it =
        g_saFake->byHeight.find(height);
    if (it == g_saFake->byHeight.end())
    {
        if (error) *error = strprintf("height %d not in fake chain", height);
        return false;
    }
    out->found = true;
    out->height = height;
    out->hash.SetHex(it->second.first);
    out->nMoneySupply = it->second.second;
    return true;
}

struct SAScope
{
    SAFakeChain chain;
    SAScope() { g_saFake = &chain; SetMoneySupplyVerification(MONEY_SUPPLY_UNKNOWN, ""); }
    ~SAScope() { g_saFake = NULL; SetMoneySupplyVerification(MONEY_SUPPLY_UNKNOWN, ""); }
};

bool SAIsHex64(const std::string& s)
{
    if (s.size() != 64) return false;
    for (size_t i = 0; i < s.size(); ++i)
    {
        const char c = s[i];
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) return false;
    }
    return true;
}

} // namespace

BOOST_AUTO_TEST_CASE(sa_table_invariants)
{
    size_t n = 0;
    const MoneySupplyAnchor* t = MoneySupplyAnchorTable(&n);
    BOOST_REQUIRE(t != NULL);
    BOOST_CHECK_MESSAGE(n >= 20, "the anchor table must actually cover the chain; n=" << n);
    for (size_t i = 0; i < n; ++i)
    {
        BOOST_CHECK_MESSAGE(SAIsHex64(std::string(t[i].hashHex)),
                            "anchor " << i << " hash is not a 64-char lowercase hex string");
        BOOST_CHECK_MESSAGE(t[i].supply > 0, "anchor " << i << " supply must be positive");
        BOOST_CHECK_MESSAGE(t[i].height > 8000000,
                            "mainnet anchors must sit above 8,000,000 (got " << t[i].height << ")");
        if (i > 0)
        {
            BOOST_CHECK_MESSAGE(t[i].height > t[i - 1].height, "anchor table must be strictly sorted");
            const int64_t inc = t[i].supply - t[i - 1].supply;
            BOOST_CHECK_MESSAGE(inc > 0, "cumulative supply must increase between anchors");
            // A plausible increment: far below 1000 INN per interval, far above 0.
            BOOST_CHECK_MESSAGE(inc < 1000LL * COIN, "implausible supply jump between anchors");
        }
    }
}

BOOST_AUTO_TEST_CASE(sa_lookup_helpers_above_8m)
{
    size_t n = 0;
    const MoneySupplyAnchor* t = MoneySupplyAnchorTable(&n);
    BOOST_REQUIRE(n >= 4);
    // Below the first anchor: no anchor at or below.
    BOOST_CHECK(FindMoneySupplyAnchorAtOrBelow(t[0].height - 1) == NULL);
    // Exact hit.
    BOOST_CHECK(FindMoneySupplyAnchorAtOrBelow(t[2].height) == &t[2]);
    // Between anchors -> the lower one.
    BOOST_CHECK(FindMoneySupplyAnchorAtOrBelow(t[2].height + 1) == &t[2]);
    // Strictly below.
    BOOST_CHECK(FindMoneySupplyAnchorBelow(t[2].height) == &t[1]);
    // Strictly above.
    BOOST_CHECK(FindMoneySupplyAnchorAbove(t[2].height) == &t[3]);
    // Range covering the first two.
    BOOST_CHECK(FindMoneySupplyAnchorInRange(t[0].height, t[1].height) == &t[1]);
    // Range with none.
    BOOST_CHECK(FindMoneySupplyAnchorInRange(t[0].height + 1, t[1].height - 1) == NULL);
    // Tip beyond the table -> the newest anchor.
    BOOST_CHECK(FindMoneySupplyAnchorAtOrBelow(t[n - 1].height + 5000) == &t[n - 1]);
}

BOOST_AUTO_TEST_CASE(sa_verification_state_roundtrip)
{
    SetMoneySupplyVerification(MONEY_SUPPLY_VIOLATION, "boom");
    std::string detail;
    BOOST_CHECK_EQUAL(GetMoneySupplyVerification(&detail), (int)MONEY_SUPPLY_VIOLATION);
    BOOST_CHECK_EQUAL(detail, std::string("boom"));
    BOOST_CHECK_EQUAL(std::string(MoneySupplyVerificationName(MONEY_SUPPLY_VIOLATION)), std::string("VIOLATION"));

    SetMoneySupplyVerification(MONEY_SUPPLY_VERIFIED, "ok");
    BOOST_CHECK_EQUAL(GetMoneySupplyVerification(&detail), (int)MONEY_SUPPLY_VERIFIED);
    BOOST_CHECK_EQUAL(std::string(MoneySupplyVerificationName(MONEY_SUPPLY_VERIFIED)), std::string("VERIFIED"));

    SetMoneySupplyVerification(MONEY_SUPPLY_UNKNOWN, "");
    BOOST_CHECK_EQUAL(std::string(MoneySupplyVerificationName(MONEY_SUPPLY_UNKNOWN)), std::string("UNVERIFIED"));

    // Four states, four distinct diagnostics: UNREADABLE must never alias VIOLATION.
    SetMoneySupplyVerification(MONEY_SUPPLY_UNREADABLE, "cannot read");
    BOOST_CHECK_EQUAL(GetMoneySupplyVerification(&detail), (int)MONEY_SUPPLY_UNREADABLE);
    BOOST_CHECK_EQUAL(std::string(MoneySupplyVerificationName(MONEY_SUPPLY_UNREADABLE)), std::string("UNREADABLE"));
    BOOST_CHECK(std::string(MoneySupplyVerificationName(MONEY_SUPPLY_UNREADABLE)) !=
                std::string(MoneySupplyVerificationName(MONEY_SUPPLY_VIOLATION)));
}

BOOST_AUTO_TEST_CASE(sa_matching_anchor_is_verified)
{
    SAScope scope;
    size_t n = 0;
    const MoneySupplyAnchor* t = MoneySupplyAnchorTable(&n);
    BOOST_REQUIRE(n >= 2);
    const MoneySupplyAnchor& a = t[1]; // deliberately not the newest
    scope.chain.byHeight[a.height] = std::make_pair(std::string(a.hashHex), a.supply);

    std::string detail;
    BOOST_CHECK(VerifyMoneySupplyAnchorsWithResolver(a.height, SAResolve, &detail));
    BOOST_CHECK_EQUAL(GetMoneySupplyVerification(&detail), (int)MONEY_SUPPLY_VERIFIED);
    BOOST_CHECK_MESSAGE(detail.find(a.hashHex) != std::string::npos,
                        "the verdict must name the anchor it used: " << detail);
}

BOOST_AUTO_TEST_CASE(sa_mismatching_anchor_fails_closed)
{
    SAScope scope;
    size_t n = 0;
    const MoneySupplyAnchor* t = MoneySupplyAnchorTable(&n);
    BOOST_REQUIRE(n >= 2);
    const MoneySupplyAnchor& a = t[1];
    // THE OBSERVED DEFECT SHAPE: the canonical block, a plausible but re-based value.
    scope.chain.byHeight[a.height] = std::make_pair(std::string(a.hashHex), (int64_t)(a.supply - INT64_C(1107370942728933)));

    std::string detail;
    BOOST_CHECK_MESSAGE(!VerifyMoneySupplyAnchorsWithResolver(a.height, SAResolve, &detail),
                        "a supply mismatch against an anchored canonical block must fail closed");
    BOOST_CHECK_EQUAL(GetMoneySupplyVerification(NULL), (int)MONEY_SUPPLY_VIOLATION);
    BOOST_CHECK_MESSAGE(detail.find("MONEY SUPPLY UNVERIFIED") != std::string::npos,
                        "the violation must state that the value is unverified: " << detail);
}

BOOST_AUTO_TEST_CASE(sa_reorged_anchor_is_skipped_not_flagged)
{
    SAScope scope;
    size_t n = 0;
    const MoneySupplyAnchor* t = MoneySupplyAnchorTable(&n);
    BOOST_REQUIRE(n >= 3);
    const MoneySupplyAnchor& newer = t[2];
    const MoneySupplyAnchor& older = t[1];
    // The chain reorganised at the newer anchored height: different block there.
    scope.chain.byHeight[newer.height] = std::make_pair(std::string("00000000000000000000000000000000000000000000000000000000000000aa"),
                                                        newer.supply + COIN);
    scope.chain.byHeight[older.height] = std::make_pair(std::string(older.hashHex), older.supply);

    std::string detail;
    BOOST_CHECK_MESSAGE(VerifyMoneySupplyAnchorsWithResolver(newer.height, SAResolve, &detail),
                        "a legitimate reorganisation must not raise a violation");
    BOOST_CHECK_EQUAL(GetMoneySupplyVerification(NULL), (int)MONEY_SUPPLY_VERIFIED);
    BOOST_CHECK_MESSAGE(detail.find("reorganized") != std::string::npos,
                        "the skipped anchor must be reported: " << detail);
}

BOOST_AUTO_TEST_CASE(sa_unresolvable_anchor_fails_closed)
{
    SAScope scope;
    size_t n = 0;
    const MoneySupplyAnchor* t = MoneySupplyAnchorTable(&n);
    BOOST_REQUIRE(n >= 1);
    scope.chain.fail = true;
    std::string detail;
    BOOST_CHECK(!VerifyMoneySupplyAnchorsWithResolver(t[0].height, SAResolve, &detail));
    // A READ failure is diagnostic-distinct from a proven numeric divergence.
    BOOST_CHECK_EQUAL(GetMoneySupplyVerification(NULL), (int)MONEY_SUPPLY_UNREADABLE);
    BOOST_CHECK_MESSAGE(std::string(MoneySupplyVerificationName(MONEY_SUPPLY_UNREADABLE)) == "UNREADABLE",
                        "a read failure must report its own status, not VIOLATION");
    BOOST_CHECK_MESSAGE(detail.find("cannot be read") != std::string::npos,
                        "the read failure must say so: " << detail);
}

BOOST_AUTO_TEST_CASE(sa_no_applicable_anchor_is_unknown_never_verified)
{
    SAScope scope;
    size_t n = 0;
    const MoneySupplyAnchor* t = MoneySupplyAnchorTable(&n);
    BOOST_REQUIRE(n >= 1);
    std::string detail;
    BOOST_CHECK(VerifyMoneySupplyAnchorsWithResolver(t[0].height - 10, SAResolve, &detail));
    BOOST_CHECK_EQUAL(GetMoneySupplyVerification(NULL), (int)MONEY_SUPPLY_UNKNOWN);
    BOOST_CHECK_MESSAGE(detail.find("UNVERIFIED") != std::string::npos,
                        "absence of an applicable anchor must be reported as UNVERIFIED: " << detail);
}

BOOST_AUTO_TEST_CASE(sa_repeated_verification_is_stable_restart_shape)
{
    SAScope scope;
    size_t n = 0;
    const MoneySupplyAnchor* t = MoneySupplyAnchorTable(&n);
    BOOST_REQUIRE(n >= 2);
    const MoneySupplyAnchor& a = t[n - 1];
    scope.chain.byHeight[a.height] = std::make_pair(std::string(a.hashHex), a.supply);
    for (int i = 0; i < 3; ++i) // startup, post-restart, post-restart
    {
        SetMoneySupplyVerification(MONEY_SUPPLY_UNKNOWN, "");
        std::string detail;
        BOOST_CHECK(VerifyMoneySupplyAnchorsWithResolver(a.height, SAResolve, &detail));
        BOOST_CHECK_EQUAL(GetMoneySupplyVerification(NULL), (int)MONEY_SUPPLY_VERIFIED);
    }
    // The poisoned shape must be stable too (a restart never "heals" it).
    scope.chain.byHeight[a.height] = std::make_pair(std::string(a.hashHex), INT64_C(20000000));
    for (int i = 0; i < 3; ++i)
    {
        SetMoneySupplyVerification(MONEY_SUPPLY_UNKNOWN, "");
        BOOST_CHECK(!VerifyMoneySupplyAnchorsWithResolver(a.height, SAResolve, NULL));
        BOOST_CHECK_EQUAL(GetMoneySupplyVerification(NULL), (int)MONEY_SUPPLY_VIOLATION);
    }
}

BOOST_AUTO_TEST_SUITE_END()
