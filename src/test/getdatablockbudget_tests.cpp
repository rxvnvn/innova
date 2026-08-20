// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Unit tests for the bounded getdata block-serve budget
// (-getdatablockbudget, default 1 == legacy one-block-per-call cap).
//
// ProcessGetData is file-static in main.cpp, so these tests drive it through
// the ProcessGetDataForTesting hook (mirrors the ibdforensic::*ForTesting
// pattern).  The budget break fires identically whether a requested block is
// found (served + PushMessage "block") or missing (queued into vNotFound):
// both paths advance the per-pass block counter and hit the same break.  The
// found->PushMessage("block") branch is byte-for-byte identical to legacy and
// is covered by the canary and p2p_sync; these tests therefore verify the
// budget/ordering/erase/fairness/tx/notfound semantics deterministically via
// queue consumption without needing on-disk block files.

#include <boost/test/unit_test.hpp>

#include <deque>
#include <set>
#include <sys/types.h>
#include <vector>

#include "../main.h"
#include "../net.h"
#include "../protocol.h"
#include "../uint256.h"
#include "../util.h"

using namespace std;

namespace {

// Distinctive hashes (high bits set) that never collide with the small
// TestHash values used by sibling suites or with real chain hashes.
uint256 BlockHash(unsigned int i)
{
    return uint256((uint64_t)0xABCD000000000000ULL + (uint64_t)i);
}
uint256 TxHash(unsigned int i)
{
    return uint256((uint64_t)0x100000000ULL + (uint64_t)i);
}
CInv BlockInv(unsigned int i) { return CInv(MSG_BLOCK, BlockHash(i)); }
CInv TxInv(unsigned int i) { return CInv(MSG_TX, TxHash(i)); }

struct CNodePtr
{
    CNode* n;
    CNodePtr(CNode* p) : n(p) {}
    ~CNodePtr() { delete n; }
};

CNode* MakeNode(const vector<CInv>& invs)
{
    CAddress addr;
    CNode* n = new CNode(INVALID_SOCKET, addr, "", true);
    n->vRecvGetData.assign(invs.begin(), invs.end());
    return n;
}

} // namespace

BOOST_AUTO_TEST_SUITE(getdatablockbudget_tests)

// 1 + 13. budget=1: exact legacy one-block-per-pass; default is 1.
BOOST_AUTO_TEST_CASE(budget1_legacy_one_block_per_call)
{
    ResetDataBlockBudgetCountersForTesting();
    InitGetDataBlockBudget(1);
    BOOST_CHECK_EQUAL(GetDataBlockBudget(), 1); // default == legacy

    vector<CInv> invs;
    for (unsigned i = 0; i < 10; ++i) invs.push_back(BlockInv(i));
    CNodePtr np(MakeNode(invs));
    CNode* n = np.n;

    ProcessGetDataForTesting(n);
    BOOST_CHECK_EQUAL(n->vRecvGetData.size(), (size_t)9);              // 1 consumed
    BOOST_CHECK(n->vRecvGetData.front().hash == BlockHash(1));         // order kept
    ProcessGetDataForTesting(n);
    BOOST_CHECK_EQUAL(n->vRecvGetData.size(), (size_t)8);
    BOOST_CHECK(n->vRecvGetData.front().hash == BlockHash(2));

    DataBlockBudgetCounters c = GetDataBlockBudgetCounters();
    BOOST_CHECK_GE(c.calls, 2);
    BOOST_CHECK_EQUAL(c.maxServedPerCall, 1);                          // never more than 1
    InitGetDataBlockBudget(1);
}

// 2. budget=4 with 10 queued: 4 / 4 / 2, ordering + erase boundaries.
BOOST_AUTO_TEST_CASE(budget4_drains_4_4_2_in_order)
{
    ResetDataBlockBudgetCountersForTesting();
    InitGetDataBlockBudget(4);

    vector<CInv> invs;
    for (unsigned i = 0; i < 10; ++i) invs.push_back(BlockInv(i));
    CNodePtr np(MakeNode(invs));
    CNode* n = np.n;

    ProcessGetDataForTesting(n);
    BOOST_CHECK_EQUAL(n->vRecvGetData.size(), (size_t)6);              // consumed 4
    BOOST_CHECK(n->vRecvGetData.front().hash == BlockHash(4));

    ProcessGetDataForTesting(n);
    BOOST_CHECK_EQUAL(n->vRecvGetData.size(), (size_t)2);              // consumed 4
    BOOST_CHECK(n->vRecvGetData.front().hash == BlockHash(8));

    ProcessGetDataForTesting(n);
    BOOST_CHECK_EQUAL(n->vRecvGetData.size(), (size_t)0);              // consumed 2

    DataBlockBudgetCounters c = GetDataBlockBudgetCounters();
    BOOST_CHECK_EQUAL(c.calls, 3);
    BOOST_CHECK_EQUAL(c.budgetHit, 2);                                 // only the two full passes
    BOOST_CHECK_EQUAL(c.maxServedPerCall, 4);
    BOOST_CHECK_EQUAL(c.queueRemaining, 0);
    InitGetDataBlockBudget(1);
}

// 3. budget > queue: all drained in one pass, no budget hit.
BOOST_AUTO_TEST_CASE(budget_larger_than_queue_drains_all)
{
    ResetDataBlockBudgetCountersForTesting();
    InitGetDataBlockBudget(8);

    vector<CInv> invs;
    for (unsigned i = 0; i < 3; ++i) invs.push_back(BlockInv(i));
    CNodePtr np(MakeNode(invs));
    CNode* n = np.n;

    ProcessGetDataForTesting(n);
    BOOST_CHECK_EQUAL(n->vRecvGetData.size(), (size_t)0);

    DataBlockBudgetCounters c = GetDataBlockBudgetCounters();
    BOOST_CHECK_EQUAL(c.budgetHit, 0);
    BOOST_CHECK_EQUAL(c.maxServedPerCall, 3);
    InitGetDataBlockBudget(1);
}

// 4. send buffer full: serving stops immediately despite remaining budget.
BOOST_AUTO_TEST_CASE(sendbuffer_full_stops_immediately)
{
    ResetDataBlockBudgetCountersForTesting();
    InitGetDataBlockBudget(8);

    vector<CInv> invs;
    for (unsigned i = 0; i < 5; ++i) invs.push_back(BlockInv(i));
    CNodePtr np(MakeNode(invs));
    CNode* n = np.n;
    n->nSendSize = SendBufferSize(); // guard fires on the first iteration

    ProcessGetDataForTesting(n);
    BOOST_CHECK_EQUAL(n->vRecvGetData.size(), (size_t)5);              // nothing consumed

    DataBlockBudgetCounters c = GetDataBlockBudgetCounters();
    BOOST_CHECK_EQUAL(c.sendBufferBreak, 1);
    BOOST_CHECK_EQUAL(c.maxServedPerCall, 0);
    InitGetDataBlockBudget(1);
}

// 5. two peers: the budget bounds each peer per pass; neither monopolizes.
BOOST_AUTO_TEST_CASE(two_peers_bounded_fairness)
{
    ResetDataBlockBudgetCountersForTesting();
    InitGetDataBlockBudget(8);

    vector<CInv> invsA;
    for (unsigned i = 0; i < 20; ++i) invsA.push_back(BlockInv(1000 + i));
    vector<CInv> invsB;
    for (unsigned i = 0; i < 20; ++i) invsB.push_back(BlockInv(2000 + i));
    CNodePtr na(MakeNode(invsA));
    CNodePtr nb(MakeNode(invsB));

    ProcessGetDataForTesting(na.n);
    ProcessGetDataForTesting(nb.n);

    BOOST_CHECK_EQUAL(na.n->vRecvGetData.size(), (size_t)12);
    BOOST_CHECK_EQUAL(nb.n->vRecvGetData.size(), (size_t)12);

    DataBlockBudgetCounters c = GetDataBlockBudgetCounters();
    BOOST_CHECK_EQUAL(c.calls, 2);
    BOOST_CHECK_EQUAL(c.maxServedPerCall, 8);                          // per-peer bound held
    InitGetDataBlockBudget(1);
}

// 6. MSG_TX unchanged: all tx drain in one call regardless of the block budget.
BOOST_AUTO_TEST_CASE(tx_path_unchanged_drains_all)
{
    ResetDataBlockBudgetCountersForTesting();
    InitGetDataBlockBudget(1);

    vector<CInv> invs;
    for (unsigned i = 0; i < 6; ++i) invs.push_back(TxInv(i));
    CNodePtr np(MakeNode(invs));
    CNode* n = np.n;

    ProcessGetDataForTesting(n);
    BOOST_CHECK_EQUAL(n->vRecvGetData.size(), (size_t)0);              // all 6 tx drained

    // Missing tx types queue exactly one "notfound" (unchanged).
    size_t notfound = 0;
    for (const SendMessageMeta& m : n->vSendMeta)
        if (m.command == "notfound") ++notfound;
    BOOST_CHECK_EQUAL(notfound, (size_t)1);

    DataBlockBudgetCounters c = GetDataBlockBudgetCounters();
    BOOST_CHECK_EQUAL(c.calls, 1);
    BOOST_CHECK_EQUAL(c.maxServedPerCall, 0);                          // no MSG_BLOCK seen
    InitGetDataBlockBudget(1);
}

// 7. mixed TX/BLOCK ordering: budget counts blocks only; tx in between drain.
BOOST_AUTO_TEST_CASE(mixed_tx_block_ordering)
{
    ResetDataBlockBudgetCountersForTesting();
    InitGetDataBlockBudget(2);

    // queue: B0, T0, B1, T1, B2
    vector<CInv> invs;
    invs.push_back(BlockInv(0));
    invs.push_back(TxInv(0));
    invs.push_back(BlockInv(1));
    invs.push_back(TxInv(1));
    invs.push_back(BlockInv(2));
    CNodePtr np(MakeNode(invs));
    CNode* n = np.n;

    // Pass 1 consumes B0, T0, B1 (B1 is the 2nd block -> budget hit).
    ProcessGetDataForTesting(n);
    BOOST_CHECK_EQUAL(n->vRecvGetData.size(), (size_t)2);
    BOOST_CHECK(n->vRecvGetData[0].hash == TxInv(1).hash);
    BOOST_CHECK(n->vRecvGetData[1].hash == BlockInv(2).hash);

    // Pass 2 drains T1, B2.
    ProcessGetDataForTesting(n);
    BOOST_CHECK_EQUAL(n->vRecvGetData.size(), (size_t)0);

    DataBlockBudgetCounters c = GetDataBlockBudgetCounters();
    BOOST_CHECK_EQUAL(c.calls, 2);
    BOOST_CHECK_EQUAL(c.budgetHit, 1);
    InitGetDataBlockBudget(1);
}

// 9 + 10 + 11. Full drain: ordering, erase boundaries, no duplicate serving,
// and one "notfound" queued per pass for the missing blocks.
BOOST_AUTO_TEST_CASE(full_drain_order_no_duplicate_notfound)
{
    ResetDataBlockBudgetCountersForTesting();
    InitGetDataBlockBudget(4);

    vector<CInv> invs;
    for (unsigned i = 0; i < 10; ++i) invs.push_back(BlockInv(i));
    CNodePtr np(MakeNode(invs));
    CNode* n = np.n;

    unsigned expectFront = 0;
    size_t totalConsumed = 0;
    size_t calls = 0;
    while (!n->vRecvGetData.empty())
    {
        BOOST_CHECK(n->vRecvGetData.front().hash == BlockHash(expectFront));
        size_t before = n->vRecvGetData.size();
        ProcessGetDataForTesting(n);
        size_t consumed = before - n->vRecvGetData.size();
        totalConsumed += consumed;
        expectFront += (unsigned)consumed;
        ++calls;
    }

    BOOST_CHECK_EQUAL(totalConsumed, (size_t)10);
    BOOST_CHECK_EQUAL(calls, 3); // 4 / 4 / 2

    // Missing MSG_BLOCK items produce NO "notfound" (send=false silently; only
    // missing tx/query types queue a "notfound").  Exact legacy behavior, so the
    // "notfound" path stays untriggered here.
    size_t notfound = 0;
    for (const SendMessageMeta& m : n->vSendMeta)
        if (m.command == "notfound") ++notfound;
    BOOST_CHECK_EQUAL(notfound, (size_t)0);
    InitGetDataBlockBudget(1);
}

// 12. Invalid budget config clamped safely.
BOOST_AUTO_TEST_CASE(budget_config_clamping)
{
    InitGetDataBlockBudget(0);     BOOST_CHECK_EQUAL(GetDataBlockBudget(), 1);
    InitGetDataBlockBudget(-5);    BOOST_CHECK_EQUAL(GetDataBlockBudget(), 1);
    InitGetDataBlockBudget(8);     BOOST_CHECK_EQUAL(GetDataBlockBudget(), 8);
    InitGetDataBlockBudget(1000);  BOOST_CHECK_EQUAL(GetDataBlockBudget(), 64);
    InitGetDataBlockBudget(64);    BOOST_CHECK_EQUAL(GetDataBlockBudget(), 64);
    InitGetDataBlockBudget(1);     // restore default
}

// Local structural benchmark: 56-block burst drained across budgets.  Passes
// are deterministic (= ceil(56/budget)); per-pass time here is harness CPU
// only (no msghand cadence, no disk reads), so it is a lower-bound cost proxy,
// not the production drain time (which is passes x msghand-pass-duration).
BOOST_AUTO_TEST_CASE(benchmark_56_block_drain_across_budgets)
{
    const unsigned kBlocks = 56;
    const int budgets[] = {1, 2, 4, 8, 16};
    BOOST_TEST_MESSAGE("getdatablockbudget local structural benchmark (56-block burst, missing blocks):");
    BOOST_TEST_MESSAGE("budget | passes | max_served/call | total_cpu_us | per-pass_us");
    for (int b : budgets)
    {
        ResetDataBlockBudgetCountersForTesting();
        InitGetDataBlockBudget(b);
        vector<CInv> invs;
        for (unsigned i = 0; i < kBlocks; ++i) invs.push_back(BlockInv(10000 + i));
        CNodePtr np(MakeNode(invs));
        CNode* n = np.n;
        int64_t t0 = GetTimeMicros();
        size_t passes = 0;
        while (!n->vRecvGetData.empty())
        {
            ProcessGetDataForTesting(n);
            ++passes;
        }
        int64_t dt = GetTimeMicros() - t0;
        DataBlockBudgetCounters c = GetDataBlockBudgetCounters();
        double per = passes ? (double)dt / (double)passes : 0.0;
        BOOST_TEST_MESSAGE("    " << b << " |   " << passes << "  |      " << c.maxServedPerCall
                                  << "      |      " << dt << "     |   " << per);
        // Structural invariant: passes == ceil(56 / budget).
        BOOST_CHECK_EQUAL(passes, (size_t)((kBlocks + (unsigned)b - 1) / (unsigned)b));
        InitGetDataBlockBudget(1);
    }
    InitGetDataBlockBudget(1);
}

BOOST_AUTO_TEST_SUITE_END()
