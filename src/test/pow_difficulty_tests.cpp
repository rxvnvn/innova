// pow_difficulty_tests.cpp
//
// Regression guards for the PoW/PoS difficulty retarget after removal of the
// never-firing FORK_HEIGHT_TIGHTER_DRIFT gate (mainnet height 999999999 made the
// POST-FORK branch dead). The retained implementation is the pre-fork Dark-Gravity
// smoothing:
//
//   bnNew = prevPowBits * ((nInterval-1)*eff + 2*S) / ((nInterval+1)*eff)
//   nInterval = nTargetTimespan(30) / nEffectiveSpacing
//   S = nTime(prev same-type) - nTime(prevPrev same-type); negative S -> eff
//
// No consensus formula/parameter change is permitted here — the check is that the
// collapsed code computes EXACTLY this preserved pre-fork arithmetic, including the
// edge behaviours, byte-identically, and bounds itself to bnProofOfWorkLimit.

#include <boost/test/unit_test.hpp>
#include <vector>

#include "main.h"
#include "util.h"
#include "bignum.h"
#include "uint256.h"

extern CBigNum bnProofOfWorkLimit;

static const int64_t TARGET_TIMESPAN = 30; // static const in main.cpp:4577 (nTargetTimespan)

BOOST_AUTO_TEST_SUITE(pow_difficulty_tests)

// Builds a short canonical all-PoW chain whose last spacing (the gap between the
// two most recent PoW blocks) equals `gapS`. Returns the required bits for the
// block that would extend it, computed by the retained implementation.
static unsigned int RequiredForLastGap(int64_t gapS, unsigned int baseBits,
                                       int64_t baseTime)
{
    const int N = 5; // >=3 so prevPrev->pprev != NULL (general formula, not early return)
    std::vector<CBlockIndex> backing(static_cast<size_t>(N));
    CBlockIndex* prev = NULL;
    for (int i = 0; i < N; ++i) {
        CBlockIndex& idx = backing[static_cast<size_t>(i)];
        idx.nHeight = i + 1;
        idx.pprev = prev;
        idx.nBits = baseBits;
        prev = &idx;
    }
    // earlier blocks: sparse but strictly below the last-prev time
    for (int i = 0; i < N - 2; ++i)
        backing[static_cast<size_t>(i)].nTime = baseTime - 1000;
    // last two PoW blocks define spacing = gapS
    backing[static_cast<size_t>(N - 2)].nTime = baseTime;
    backing[static_cast<size_t>(N - 1)].nTime = baseTime + gapS;
    return GetNextTargetRequired(&backing[static_cast<size_t>(N - 1)], false);
}

// Independent, byte-identical reimplementation of the retained pre-fork formula.
static unsigned int IndependentExpected(int64_t gapS, unsigned int baseBits,
                                        bool* clamped)
{
    CBigNum bn;
    bn.SetCompact(baseBits);
    unsigned int eff = GetTargetSpacingForHeight(10); // any height past the early returns
    int64_t nInterval = TARGET_TIMESPAN / (int64_t)eff;
    int64_t S = gapS;
    if (S < 0) S = eff; // pre-fork: negative spacing -> effective spacing
    bn *= ((nInterval - 1) * eff + S + S);
    bn /= ((nInterval + 1) * eff);
    if (bn <= 0 || bn > bnProofOfWorkLimit) {
        bn = bnProofOfWorkLimit;
        if (clamped) *clamped = true;
    }
    return bn.GetCompact();
}

BOOST_AUTO_TEST_CASE(spacing_equals_effective_is_unchanged)
{
    const unsigned int base = 0x1d00ffff;
    unsigned int eff = GetTargetSpacingForHeight(10);
    unsigned int got  = RequiredForLastGap(eff, base, 123456789);
    BOOST_CHECK_EQUAL(got, IndependentExpected(eff, base, NULL));
    // spacing == effective -> smoothing multiplier is exactly 1.0 (unchanged bits)
    BOOST_CHECK_EQUAL(got, base);
}

BOOST_AUTO_TEST_CASE(decreasing_spacing_increases_difficulty)
{
    const unsigned int base = 0x1d00ffff;
    unsigned int eff = GetTargetSpacingForHeight(10);
    unsigned int fast = RequiredForLastGap(0, base, 123456789);       // S=0
    bool cl = false; unsigned int exp = IndependentExpected(0, base, &cl);
    BOOST_CHECK_EQUAL(fast, exp);
    BOOST_CHECK(!cl);
    // target strictly smaller than base => difficulty strictly higher
    BOOST_CHECK(fast != base);
    // monotonic: a larger spacing must not produce a HARDER (smaller) target.
    // decoded to target integer (compact is not a monotonic int ordering).
    unsigned int s2 = RequiredForLastGap(2, base, 123456789);
    unsigned int s1 = RequiredForLastGap(1, base, 123456789);
    CBigNum t1, t2;
    t1.SetCompact(s1); t2.SetCompact(s2);
    BOOST_CHECK(t2 >= t1); // larger gap -> target >= (difficulty <=)
}

BOOST_AUTO_TEST_CASE(increasing_spacing_decreases_difficulty)
{
    const unsigned int base = 0x1d00ffff;
    unsigned int eff = GetTargetSpacingForHeight(10);
    unsigned int slow = RequiredForLastGap(eff + 30, base, 123456789);
    bool cl = false; unsigned int exp = IndependentExpected(eff + 30, base, &cl);
    BOOST_CHECK_EQUAL(slow, exp);
    // target larger than base (unless clamped); difficulty lower
    if (!cl)
        BOOST_CHECK(slow != base);
}

BOOST_AUTO_TEST_CASE(negative_spacing_falls_back_to_effective)
{
    // pre-fork: negative spacing is replaced by the effective spacing, which makes
    // the multiplier exactly 1.0 -> the returned bits must equal the base bits.
    const unsigned int base = 0x1e0137ba;
    unsigned int got = RequiredForLastGap(-120, base, 123456789);
    BOOST_CHECK_EQUAL(got, base);
}

BOOST_AUTO_TEST_CASE(limit_clamp_bounds_the_result)
{
    // base at the proof-of-work limit; any multiplier > 1 must clamp back to the
    // limit (target can never exceed bnProofOfWorkLimit).
    unsigned int baseLimit = bnProofOfWorkLimit.GetCompact();
    unsigned int eff = GetTargetSpacingForHeight(10);
    unsigned int got = RequiredForLastGap(eff + 60, baseLimit, 123456789);
    BOOST_CHECK_EQUAL(got, baseLimit);
    // A mid-range base must never exceed the limit even for huge positive spacing.
    unsigned int gotMid = RequiredForLastGap(eff * 200, 0x1d00ffff, 123456789);
    CBigNum bn; bn.SetCompact(gotMid);
    BOOST_CHECK(bn <= bnProofOfWorkLimit || gotMid == bnProofOfWorkLimit.GetCompact());
}

BOOST_AUTO_TEST_SUITE_END()