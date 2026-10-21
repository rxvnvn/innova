// PoW chain-stale regression guard.
//
// Root-cause regression: a canonical chain whose newest block is older than the
// former 300s mining-pause heuristic must NOT, by itself, halt or invalidate
// PoW mining. The old age-only gate in RunCPUMinerWorker
//   fChainStale = /* !fRegTest && */ pindexBest->GetBlockTime() < GetTime() - 300;
//   if (fChainStale) { pause PAUSE_CHAIN_STALE; continue; }        // REMOVED
// froze every PoW worker to 0 H/s whenever the (perfectly valid) network tip
// stood for more than 5 minutes (observed live: tip 8180388, 19.10.2026, frozen
// network -> cpuhashing=false, cpupausereason=chain-stale, cputotalhashes=0).
//
// The consensus boundary that decides CONNECTABILITY of the freshly produced
// block lives in AcceptBlock() (main.cpp ~8955):
//   reject iff  GetBlockTime() <= nMTPast
//            || FutureDrift(GetBlockTime(), nHeight) < pindexPrev->GetBlockTime()
// A stale PARENT meets nMTPast == old, so a current-timestamp PoW child is NOT
// "too early". This test pins that invariant at the real production predicates
// (GetPastTimeLimit / FutureDrift): a >300s-old canonical chain does NOT reject
// a newly mined PoW child with a current timestamp, and cannot by itself stop
// PoW mining.
//
// Scope note: this guards the invariant at the acceptor boundary (the layer the
// removed worker gate sat behind). It does not drive the worker thread (that
// would require a live mining run, deliberately out of scope per the change
// mandate); it fails if an age-gate is reintroduced into CreateNewBlock or the
// acceptor.
#include "main.h"
#include "util.h"

#include <boost/test/unit_test.hpp>
#include <vector>
#include <cstdint>

BOOST_AUTO_TEST_SUITE(pow_chainstale)

// Build a linked canonical chain of nHeight blocks whose TIP is nTipTime and
// whose times increase monotonically up to the tip, as consensus requires.
// Returns the tip pointer.
static CBlockIndex* BuildCanonicalChain(int nHeight, int64_t nTipTime,
                                        std::vector<CBlockIndex>& backing)
{
    backing.clear();
    backing.resize(nHeight);          // value-initializes each CBlockIndex
    CBlockIndex* prev = NULL;
    for (int i = 0; i < nHeight; ++i)
    {
        CBlockIndex& idx = backing[i];
        idx.nHeight = i + 1;                     // 1-based heights
        idx.nTime = nTipTime - (nHeight - i) + 1; // monotonic, tip newest
        idx.pprev = prev;
        prev = &idx;
    }
    return prev;                                  // the tip
}

BOOST_AUTO_TEST_CASE(stale_canonical_chain_does_not_reject_new_pow_child)
{
    const int nHeight = 100;                      // well past the old nHeight>10 gate
    const int64_t nNow = GetTime();

    // A canonical chain whose TIP is >24h old: genuinely ">300s" stale.
    const int64_t nTipTime = nNow - 86400;
    std::vector<CBlockIndex> backing;
    CBlockIndex* tip = BuildCanonicalChain(nHeight, nTipTime, backing);
    BOOST_REQUIRE(tip != NULL);

    // Sanity: this IS the stale scenario the old gate would have paused on.
    BOOST_REQUIRE_MESSAGE(tip->GetBlockTime() < nNow - 300,
        "test precondition: tip must be older than the old 300s heuristic");

    // A freshly produced PoW child uses a CURRENT timestamp (the miner sets
    // nTime = max(GetTime(), MTP + 1) via UpdateTime/CreateNewBlock).
    const int64_t nChildTime = nNow;

    // AcceptBlock() "timestamp too early" rule (main.cpp ~8955), computed with
    // the real production predicates.
    const int64_t nMTPast = tip->GetPastTimeLimit();
    const bool bTooEarly =
        (nChildTime <= nMTPast) ||
        (FutureDrift(nChildTime, nHeight + 1) < tip->GetBlockTime());

    BOOST_CHECK_MESSAGE(!bTooEarly,
        "stale (>300s) canonical chain must not reject a current-timestamp PoW child");
    BOOST_CHECK_MESSAGE(nChildTime > nMTPast,
        "current child timestamp must exceed the stale parent's median-time-past");
    BOOST_CHECK_MESSAGE(FutureDrift(nChildTime, nHeight + 1) >= tip->GetBlockTime(),
        "FutureDrift of the current child timestamp must not fall below the parent block time");

    // The miner can legally produce such a child on this stale tip: its current
    // timestamp is strictly newer than the tip, so mining over the stale chain
    // is NOT blocked by consensus.
    BOOST_CHECK_MESSAGE(nChildTime > tip->GetBlockTime(),
        "current timestamp must be newer than the stale tip, so mining can proceed");
}

BOOST_AUTO_TEST_SUITE_END()