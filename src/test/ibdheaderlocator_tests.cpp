#include <boost/test/unit_test.hpp>
#include "ibdheaderscheduler.h"
#include <algorithm>
#include <set>

namespace {
uint256 LH(uint64_t n) { return uint256(n); }
uint256 Resolve(const std::vector<uint256>& locator, const std::set<uint256>& known)
{
    for (std::vector<uint256>::const_iterator i = locator.begin(); i != locator.end(); ++i)
        if (known.count(*i)) return *i;
    return uint256(0);
}
CIbdHeaderGraph Chain(uint64_t last)
{
    CIbdHeaderGraph g;
    BOOST_REQUIRE(g.SetAuthoritativeAnchor(LH(1), 0));
    for (uint64_t i = 2; i <= last; ++i)
        BOOST_REQUIRE(g.Insert(LH(i), LH(i - 1)) == CIbdHeaderGraph::INSERTED_ACTIVE);
    return g;
}
}

BOOST_AUTO_TEST_SUITE(ibdheaderlocator_tests)

BOOST_AUTO_TEST_CASE(anchor_only)
{
    CIbdHeaderGraph g;
    BOOST_REQUIRE(g.SetAuthoritativeAnchor(LH(1), 0));
    std::vector<uint256> v = g.BuildContinuationLocator();
    BOOST_REQUIRE(v.size() == 1U);
    BOOST_CHECK(v[0] == LH(1));
}

BOOST_AUTO_TEST_CASE(order_and_remote_resolution)
{
    CIbdHeaderGraph g = Chain(4);
    std::vector<uint256> v = g.BuildContinuationLocator();
    BOOST_REQUIRE(v.size() == 4U);
    BOOST_CHECK(v[0] == LH(4)); BOOST_CHECK(v[1] == LH(3));
    BOOST_CHECK(v[2] == LH(2)); BOOST_CHECK(v[3] == LH(1));
    BOOST_CHECK(Resolve(v, std::set<uint256>{LH(1),LH(2),LH(3),LH(4)}) == LH(4));
    BOOST_CHECK(Resolve(v, std::set<uint256>{LH(1),LH(2),LH(3)}) == LH(3));
    BOOST_CHECK(Resolve(v, std::set<uint256>{LH(1)}) == LH(1));
}

BOOST_AUTO_TEST_CASE(full_batch_continuation_advances)
{
    CIbdHeaderGraph g = Chain(2001);
    std::vector<uint256> first = g.BuildContinuationLocator();
    BOOST_CHECK(Resolve(first, std::set<uint256>{LH(1),LH(2001)}) == LH(2001));
    for (uint64_t i = 2002; i <= 4001; ++i)
        BOOST_REQUIRE(g.Insert(LH(i), LH(i - 1)) == CIbdHeaderGraph::INSERTED_ACTIVE);
    std::vector<uint256> second = g.BuildContinuationLocator();
    BOOST_CHECK(second.front() == LH(4001));
    BOOST_CHECK(Resolve(second, std::set<uint256>{LH(1),LH(2001),LH(4001)}) == LH(4001));
    BOOST_CHECK(second.front() != first.front());
}

BOOST_AUTO_TEST_CASE(reanchor_and_quarantine_exclusion)
{
    CIbdHeaderGraph g = Chain(4);
    BOOST_REQUIRE(g.Insert(LH(20), LH(2)) == CIbdHeaderGraph::INSERTED_ELIGIBLE);
    BOOST_REQUIRE(g.ActivateBranch(LH(20)));
    BOOST_REQUIRE(g.QuarantineBranch(LH(20)));
    std::vector<uint256> v = g.BuildContinuationLocator();
    BOOST_CHECK(std::find(v.begin(), v.end(), LH(20)) == v.end());
    BOOST_REQUIRE(g.SetAuthoritativeAnchor(LH(90), 10));
    v = g.BuildContinuationLocator();
    BOOST_REQUIRE(v.size() == 1U);
    BOOST_CHECK(v[0] == LH(90));
    BOOST_CHECK(std::find(v.begin(), v.end(), LH(4)) == v.end());
}

BOOST_AUTO_TEST_CASE(unique_bounded_exponential_locator)
{
    CIbdHeaderGraph g = Chain(300);
    std::vector<uint256> v = g.BuildContinuationLocator(16);
    BOOST_CHECK(v.size() <= 16U);
    BOOST_CHECK(v.front() == LH(300)); BOOST_CHECK(v.back() == LH(1));
    std::set<uint256> unique(v.begin(), v.end());
    BOOST_CHECK(unique.size() == v.size());
    BOOST_CHECK(g.BuildContinuationLocator(0).empty());
    v = g.BuildContinuationLocator(1);
    BOOST_REQUIRE(v.size() == 1U); BOOST_CHECK(v[0] == LH(1));
}

BOOST_AUTO_TEST_SUITE_END()
