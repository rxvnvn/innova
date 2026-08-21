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
#include <string>
#include <vector>

#include "../checkpoints.h"
#include "../collateralnode.h"
#include "../main.h"
#include "../net.h"
#include "../util.h"

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

BOOST_AUTO_TEST_SUITE_END()
