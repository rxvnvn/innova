#include <boost/test/unit_test.hpp>

#include "blockindex_hot_owner.h"
#include "main.h"
#include "sync.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

// =====================================================================
// A.10.1d — BlockIndexHotOwner ownership / materialization. Legacy
// mapBlockIndex remains authoritative and untouched (not used here).
// =====================================================================

/** Deterministic by-value materializer over a small in-memory authority map.
 *  Mirrors layer-1 metadata materialization (no block bytes). */
class SnapshotHotMaterializer : public BlockIndexHotMaterializer
{
public:
    struct Rec
    {
        uint256 parent;
        uint256 chainTrust;
        uint256 hashMerkle;
        int height;
        bool operatorInvalid;
        uint64_t generation;
    };

    std::map<uint256, Rec> authority;   // by-value authority (no CBlockIndex*)
    uint64_t currentGen;
    // thread-safe counter (the owner's materializer runs OUTSIDE the owner lock,
    // so concurrent peers may call it simultaneously).
    mutable std::atomic<int> materializeCalls;

    SnapshotHotMaterializer() : currentGen(1), materializeCalls(0) {}

    void Add(uint256 hash, uint256 parent, uint256 trust, int height,
             uint64_t gen = 0)
    {
        Rec r; r.parent = parent; r.chainTrust = trust;
        r.hashMerkle = uint256(0x55UL); r.height = height;
        r.operatorInvalid = false; r.generation = (gen ? gen : currentGen);
        authority[hash] = r;
    }

    BlockIndexHotStatus Materialize(const BlockIndexLogicalId& id,
                                    BlockIndexHotMaterialized* out) const override
    {
        ++materializeCalls;
        out->found = false;
        out->generation = currentGen;
        std::map<uint256, Rec>::const_iterator it = authority.find(id.GetHash());
        if (it == authority.end())
            return BlockIndexHotStatus::AUTHORITY_MISSING;
        const Rec& r = it->second;
        if (r.operatorInvalid)
            return BlockIndexHotStatus::MATERIALIZATION_UNAVAILABLE;
        BlockIndexSnapshot& s = out->snapshot;
        s.found = true;
        s.hash = id.GetHash();
        s.hashPrev = r.parent;
        s.hashMerkleRoot = r.hashMerkle;
        s.nChainTrust = r.chainTrust;
        s.height = r.height;
        s.nVersion = 4;
        s.nTime = 0x60000000U;
        s.nBits = 0x207fffffU;
        s.nNonce = 0;
        s.nFlags = 0;
        s.nFile = 0;
        s.nBlockPos = 0;
        s.fProofOfStake = false;
        s.fInMainChain = false;
        s.hasStakeModifierTime = false;
        s.hasStakeModifierChecksum = false;
        out->hasBlockSize = true;
        out->blockSize = 640;
        out->found = true;
        return BlockIndexHotStatus::OK;
    }
};

static uint256 hs(int v)
{
    uint256 h;
    h.SetHex(strprintf("%064x", (unsigned long long)v));
    return h;
}

BOOST_AUTO_TEST_SUITE(blockindex_hot_owner_tests)

// --- H0: materialize one logical block -> resident -----------------------
BOOST_AUTO_TEST_CASE(h0_materialize_resident)
{
    SnapshotHotMaterializer mat;
    mat.Add(hs(1), hs(0), uint256(4), 1);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    BlockIndexHotHandle h;
    BOOST_CHECK(owner.Pin(BlockIndexLogicalId(hs(1)), &h) == BlockIndexHotStatus::OK);
    BOOST_CHECK(owner.IsResident(BlockIndexLogicalId(hs(1))));
    BOOST_REQUIRE(h.IsValid());
    CBlockIndex* p = h.Get();
    BOOST_REQUIRE(p != NULL);
    BOOST_CHECK(p->GetBlockHash() == hs(1));
    BOOST_CHECK(p->nHeight == 1);
    BOOST_CHECK(p->nChainTrust == uint256(4));
    BOOST_CHECK(p->pprev == NULL); // sparse-hot: ancestry via by-value seam
    BOOST_CHECK(owner.ResidentCount() == (size_t)1);
}

// --- H1 (RED): pinned object survives an eviction attempt -----------------
BOOST_AUTO_TEST_CASE(h1_pinned_survives_eviction)
{
    SnapshotHotMaterializer mat;
    mat.Add(hs(1), hs(0), uint256(4), 1);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    BlockIndexHotHandle h;
    BOOST_CHECK(owner.Pin(BlockIndexLogicalId(hs(1)), &h) == BlockIndexHotStatus::OK);
    CBlockIndex* before = h.Get();
    BOOST_REQUIRE(before != NULL);

    // Eviction attempt on the pinned object must be blocked and leave it valid.
    BlockIndexHotStatus st = owner.EvictResident(BlockIndexLogicalId(hs(1)));
    BOOST_CHECK(st == BlockIndexHotStatus::EVICTION_BLOCKED);
    BOOST_CHECK(owner.IsResident(BlockIndexLogicalId(hs(1))));
    BOOST_REQUIRE(h.IsValid());
    CBlockIndex* after = h.Get();
    BOOST_REQUIRE(after != NULL);
    BOOST_CHECK(after == before); // same object survived; still resident + valid
}

// --- H2 (RED): after final pin release, object becomes evictable ----------
BOOST_AUTO_TEST_CASE(h2_release_makes_eligible)
{
    SnapshotHotMaterializer mat;
    mat.Add(hs(1), hs(0), uint256(4), 1);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    {
        BlockIndexHotHandle h;
        BOOST_CHECK(owner.Pin(BlockIndexLogicalId(hs(1)), &h) == BlockIndexHotStatus::OK);
        std::vector<BlockIndexLogicalId> elig = owner.EvictEligible();
        BOOST_CHECK(elig.empty()); // pinned -> not eligible
    } // h released here (RAII)

    std::vector<BlockIndexLogicalId> elig = owner.EvictEligible();
    BOOST_REQUIRE_EQUAL(elig.size(), (size_t)1);
    BOOST_CHECK(elig[0].GetHash() == hs(1));
    BOOST_CHECK(owner.IsResident(BlockIndexLogicalId(hs(1)))); // still resident
}

// --- H3: two pins same hash -> one object, PinCount==2 --------------------
BOOST_AUTO_TEST_CASE(h3_two_pins_one_object)
{
    SnapshotHotMaterializer mat;
    mat.Add(hs(7), hs(0), uint256(9), 1);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    BlockIndexHotHandle h1, h2;
    BOOST_CHECK(owner.Pin(BlockIndexLogicalId(hs(7)), &h1) == BlockIndexHotStatus::OK);
    int callsAfterFirst = mat.materializeCalls;
    BOOST_CHECK(owner.Pin(BlockIndexLogicalId(hs(7)), &h2) == BlockIndexHotStatus::OK);
    BOOST_CHECK(mat.materializeCalls == callsAfterFirst); // no duplicate materialize
    BOOST_CHECK(h1.Get() == h2.Get()); // same object
    BOOST_CHECK(owner.PinCount() == (size_t)2);
    BOOST_CHECK(owner.ResidentCount() == (size_t)1);
    h2.Reset();
    h1.Reset();
    BOOST_CHECK(owner.PinCount() == (size_t)0);
}

// --- H4: materialize same hash twice -> no duplicate object ---------------
BOOST_AUTO_TEST_CASE(h4_no_duplicate_object)
{
    SnapshotHotMaterializer mat;
    mat.Add(hs(3), hs(0), uint256(5), 1);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    BlockIndexHotHandle h1;
    BlockIndexHotHandle h2;
    owner.Pin(BlockIndexLogicalId(hs(3)), &h1);
    owner.Pin(BlockIndexLogicalId(hs(3)), &h2);
    BOOST_CHECK(h1.Get() == h2.Get());
    BOOST_CHECK(owner.ResidentCount() == (size_t)1);
}

// --- H5: authority lookup works with object evicted -----------------------
BOOST_AUTO_TEST_CASE(h5_authority_with_evicted)
{
    SnapshotHotMaterializer mat;
    mat.Add(hs(5), hs(0), uint256(6), 1);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    BlockIndexHotHandle h;
    owner.Pin(BlockIndexLogicalId(hs(5)), &h);
    h.Reset(); // unpin
    // evict
    BOOST_CHECK(owner.EvictResident(BlockIndexLogicalId(hs(5))) == BlockIndexHotStatus::OK);
    BOOST_CHECK(!owner.IsResident(BlockIndexLogicalId(hs(5))));
    // authority still available by value (materializer can re-serve it)
    BlockIndexHotMaterialized m;
    BOOST_CHECK(mat.Materialize(BlockIndexLogicalId(hs(5)), &m) == BlockIndexHotStatus::OK);
    BOOST_CHECK(m.found);
    BOOST_CHECK(m.snapshot.hash == hs(5));
}

// --- H6: re-materialize evicted object -> same logical/derived state ------
BOOST_AUTO_TEST_CASE(h6_rematerialize_same_state)
{
    SnapshotHotMaterializer mat;
    mat.Add(hs(9), hs(0), uint256(11), 2);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    {
        BlockIndexHotHandle h;
        owner.Pin(BlockIndexLogicalId(hs(9)), &h);
        BOOST_CHECK(h.Get()->nChainTrust == uint256(11));
    }
    owner.EvictResident(BlockIndexLogicalId(hs(9)));

    BlockIndexHotHandle h2;
    BOOST_CHECK(owner.Pin(BlockIndexLogicalId(hs(9)), &h2) == BlockIndexHotStatus::OK);
    CBlockIndex* p = h2.Get();
    BOOST_REQUIRE(p != NULL);
    BOOST_CHECK(p->GetBlockHash() == hs(9));
    BOOST_CHECK(p->nChainTrust == uint256(11));
    BOOST_CHECK(p->nHeight == 2);
    BOOST_CHECK(p->hashMerkleRoot == uint256(0x55UL));
}

// --- H7: parent cold / child hot navigation (sparse semantics) ------------
BOOST_AUTO_TEST_CASE(h7_cold_parent_hot_child)
{
    SnapshotHotMaterializer mat;
    mat.Add(hs(1), hs(0), uint256(4), 1);   // active child
    mat.Add(hs(0), uint256(0), uint256(2), 0); // genesis parent
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    BlockIndexHotHandle hChild;
    owner.Pin(BlockIndexLogicalId(hs(1)), &hChild);
    CBlockIndex* child = hChild.Get();
    BOOST_REQUIRE(child != NULL);
    BOOST_CHECK(child->pprev == NULL); // sparse: no resident parent pointer
    BOOST_CHECK(child->hashMerkleRoot == uint256(0x55UL));
    // logical parent resolvable by value via the materializer authority
    BlockIndexHotMaterialized parent;
    BOOST_CHECK(mat.Materialize(BlockIndexLogicalId(hs(0)), &parent) == BlockIndexHotStatus::OK);
    BOOST_CHECK(parent.found && parent.snapshot.hash == hs(0));
    BOOST_CHECK(child->GetBlockHash() == hs(1));
}

// --- H8: reorg path pins ancestors then releases --------------------------
BOOST_AUTO_TEST_CASE(h8_reorg_pins_and_releases)
{
    SnapshotHotMaterializer mat;
    mat.Add(hs(0), uint256(0), uint256(2), 0);
    mat.Add(hs(1), hs(0), uint256(4), 1);
    mat.Add(hs(2), hs(1), uint256(6), 2);
    mat.Add(hs(3), hs(0), uint256(8), 1); // side branch (higher trust)
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    // Simulate a connect: pin the ancestor path (vDisconnect/vConnect) then release.
    {
        BlockIndexHotHandle h0, h1, h2, h3;
        owner.Pin(BlockIndexLogicalId(hs(0)), &h0);
        owner.Pin(BlockIndexLogicalId(hs(1)), &h1);
        owner.Pin(BlockIndexLogicalId(hs(2)), &h2);
        owner.Pin(BlockIndexLogicalId(hs(3)), &h3);
        // all pinned during connect window; not evictable
        BOOST_CHECK(owner.EvictEligible().empty());
    } // all released
    // after release each object is evictable
    std::vector<BlockIndexLogicalId> elig = owner.EvictEligible();
    BOOST_CHECK(elig.size() >= (size_t)3);
    BOOST_CHECK(owner.ResidentCount() == (size_t)4);
}

// --- H9: candidate winner hash -> materialization request boundary --------
BOOST_AUTO_TEST_CASE(h9_candidate_winner_to_materialize)
{
    SnapshotHotMaterializer mat;
    mat.Add(hs(50), hs(0), uint256(20), 1); // winner (higher trust)
    mat.Add(hs(0), uint256(0), uint256(2), 0);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    // A.10.1c frontier-style selection by trust (by-value):
    // best trust = 2; winner = hs(50) with trust 20.
    BlockIndexHotHandle hWin;
    BOOST_CHECK(owner.Pin(BlockIndexLogicalId(hs(50)), &hWin) == BlockIndexHotStatus::OK);
    BlockIndexHotHandle hBest;
    owner.Pin(BlockIndexLogicalId(hs(0)), &hBest);
    BOOST_REQUIRE(hWin.Get() != NULL);
    BOOST_CHECK(hWin.Get()->nChainTrust == uint256(20));
    BOOST_CHECK(owner.ResidentCount() == (size_t)2);
}

// --- H10: materialization missing does not mutate authority ---------------
BOOST_AUTO_TEST_CASE(h10_missing_does_not_mutate_authority)
{
    SnapshotHotMaterializer mat;
    mat.Add(hs(1), hs(0), uint256(4), 1);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    // absent hash -> materialization unavailable, but authority map untouched
    BlockIndexHotHandle h;
    BlockIndexHotStatus st = owner.Pin(BlockIndexLogicalId(hs(999)), &h);
    BOOST_CHECK(st == BlockIndexHotStatus::MATERIALIZATION_UNAVAILABLE ||
                st == BlockIndexHotStatus::AUTHORITY_MISSING);
    BOOST_CHECK(!h.IsValid());
    // authority still intact for the valid hash
    BlockIndexHotMaterialized m;
    BOOST_CHECK(mat.Materialize(BlockIndexLogicalId(hs(1)), &m) == BlockIndexHotStatus::OK);
    BOOST_CHECK(m.found && m.snapshot.nChainTrust == uint256(4));
}

// --- H11: generation mismatch prevents stale publish ----------------------
BOOST_AUTO_TEST_CASE(h11_generation_mismatch_no_publish)
{
    SnapshotHotMaterializer mat;
    mat.Add(hs(1), hs(0), uint256(4), 1, /*gen=*/1);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(2); // owner CURRENT moved past materializer gen=1
    owner.SetMaterializer(&mat);

    BlockIndexHotHandle h;
    BlockIndexHotStatus st = owner.Pin(BlockIndexLogicalId(hs(1)), &h);
    // materializer serves generation 1, owner expects 2 -> mismatch, no publish
    BOOST_CHECK(st == BlockIndexHotStatus::GENERATION_MISMATCH);
    BOOST_CHECK(!h.IsValid());
    BOOST_CHECK(!owner.IsResident(BlockIndexLogicalId(hs(1))));
    BOOST_CHECK(owner.ResidentCount() == (size_t)0);
}

// --- H12: fixed hot set with historical N growth -> resident indep of N ---
BOOST_AUTO_TEST_CASE(h12_fixed_hot_independent_of_n)
{
    SnapshotHotMaterializer mat;
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    // grow authority (historical N) massively without materializing all of it
    for (int i = 1; i <= 100000; ++i)
        mat.Add(hs(i), hs(i - 1), uint256(2 * (i + 1)), i, mat.currentGen);
    // materialize only 3 hot objects (fixed H)
    BlockIndexHotHandle h1, h2, h3;
    owner.Pin(BlockIndexLogicalId(hs(10)), &h1);
    owner.Pin(BlockIndexLogicalId(hs(50000)), &h2);
    owner.Pin(BlockIndexLogicalId(hs(99999)), &h3);
    // resident count is O(H), independent of N=100000
    BOOST_CHECK(owner.ResidentCount() == (size_t)3);
    BOOST_CHECK(owner.PinCount() == (size_t)3);
    // the 100k authority records are NOT resident objects
    BlockIndexHotMetrics m = owner.Metrics();
    BOOST_CHECK(m.residentCount == 3);
}

// --- H13: concurrent/in-flight same-hash requests coalesce ----------------
BOOST_AUTO_TEST_CASE(h13_inflight_same_hash)
{
    SnapshotHotMaterializer mat;
    mat.Add(hs(4), hs(0), uint256(7), 1);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    // two pins on the same hash while first still pinned -> same object, one mat
    BlockIndexHotHandle a, b;
    owner.Pin(BlockIndexLogicalId(hs(4)), &a);
    int calls = mat.materializeCalls;
    owner.Pin(BlockIndexLogicalId(hs(4)), &b);
    BOOST_CHECK(mat.materializeCalls == calls); // coalesced (no duplicate materialize)
    BOOST_CHECK(a.Get() == b.Get());
}

// --- H14: eviction race cannot invalidate a live pin (deterministic) ------
BOOST_AUTO_TEST_CASE(h14_eviction_cannot_invalidate_pin)
{
    SnapshotHotMaterializer mat;
    mat.Add(hs(2), hs(0), uint256(3), 1);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    BlockIndexHotHandle h;
    owner.Pin(BlockIndexLogicalId(hs(2)), &h);
    CBlockIndex* live = h.Get();
    BOOST_REQUIRE(live != NULL);
    // attempt to evict the pinned object (would be a race in a live system)
    BlockIndexHotStatus st = owner.EvictResident(BlockIndexLogicalId(hs(2)));
    BOOST_CHECK(st == BlockIndexHotStatus::EVICTION_BLOCKED);
    // the live pointer/handle remains valid
    BOOST_CHECK(h.IsValid());
    BOOST_CHECK(h.Get() == live);
    BOOST_CHECK(owner.IsResident(BlockIndexLogicalId(hs(2))));
}

// --- Anchor: PinPermanent object never evictable --------------------------
BOOST_AUTO_TEST_CASE(h_anchor_not_evictable)
{
    SnapshotHotMaterializer mat;
    mat.Add(hs(0), uint256(0), uint256(2), 0); // genesis anchor
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);
    owner.PinPermanent(BlockIndexLogicalId(hs(0)));

    BlockIndexHotHandle h;
    owner.Pin(BlockIndexLogicalId(hs(0)), &h);
    h.Reset();
    // even unpinned, anchor is not evictable
    BlockIndexHotStatus st = owner.EvictResident(BlockIndexLogicalId(hs(0)));
    BOOST_CHECK(st == BlockIndexHotStatus::EVICTION_BLOCKED);
    BOOST_CHECK(owner.IsResident(BlockIndexLogicalId(hs(0))));
    BOOST_CHECK(owner.EvictEligible().empty());
}

// --- Two-phase split-lock (the required async pattern) ---------------------
// RequestMaterialization (under lock) must ONLY transition IDLE->PENDING and
// mint a token — it must NOT call the materializer inside the lock. The
// materializer runs outside; PublishMaterialized (under lock) validates the
// token/generation and atomically publishes.
BOOST_AUTO_TEST_CASE(twophase_request_does_no_io)
{
    SnapshotHotMaterializer mat;
    mat.Add(hs(20), hs(0), uint256(30), 1);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    // Phase 1 (under lock): request only — NO I/O.
    BlockIndexHotToken tok;
    BlockIndexHotStatus st = owner.RequestMaterialization(BlockIndexLogicalId(hs(20)), &tok);
    BOOST_CHECK(st == BlockIndexHotStatus::MATERIALIZATION_PENDING);
    BOOST_CHECK(tok.IsValid());
    BOOST_CHECK(mat.materializeCalls == 0); // materializer NOT called inside the lock
    BOOST_CHECK(!owner.IsResident(BlockIndexLogicalId(hs(20)))); // not yet published

    // Phase 2 (OUTSIDE lock): materialize.
    BlockIndexHotMaterialized m;
    BOOST_CHECK(mat.Materialize(BlockIndexLogicalId(hs(20)), &m) == BlockIndexHotStatus::OK);
    BOOST_CHECK(mat.materializeCalls == 1);

    // Phase 3 (under lock): validate token + atomic publish.
    st = owner.PublishMaterialized(tok, m);
    BOOST_CHECK(st == BlockIndexHotStatus::OK);
    BOOST_CHECK(owner.IsResident(BlockIndexLogicalId(hs(20))));
    BOOST_CHECK(owner.ResidentCount() == (size_t)1);
}

// A stale/foreign token (mismatched seq) must be rejected on publish and never
// publish a partial/foreign object: GENERATION_MISMATCH, no resident.
BOOST_AUTO_TEST_CASE(twophase_stale_token_rejected)
{
    SnapshotHotMaterializer mat;
    mat.Add(hs(21), hs(0), uint256(9), 1);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    BlockIndexHotToken tok;
    owner.RequestMaterialization(BlockIndexLogicalId(hs(21)), &tok);

    // forge a stale/invalid token (seq mismatch / invalid)
    BlockIndexHotToken stale = tok;
    stale.seq = tok.seq + 1000; // foreign/stale

    BlockIndexHotMaterialized m;
    mat.Materialize(BlockIndexLogicalId(hs(21)), &m);
    BlockIndexHotStatus st = owner.PublishMaterialized(stale, m);
    BOOST_CHECK(st == BlockIndexHotStatus::GENERATION_MISMATCH);
    BOOST_CHECK(!owner.IsResident(BlockIndexLogicalId(hs(21))));
    BOOST_CHECK(owner.ResidentCount() == (size_t)0);
}

// Generation moved while in flight: publish rejects (GENERATION_MISMATCH).
BOOST_AUTO_TEST_CASE(twophase_generation_moved_rejected)
{
    SnapshotHotMaterializer mat;
    mat.Add(hs(22), hs(0), uint256(9), 1, /*gen=*/1);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(1);
    owner.SetMaterializer(&mat);

    BlockIndexHotToken tok;
    owner.RequestMaterialization(BlockIndexLogicalId(hs(22)), &tok);

    BlockIndexHotMaterialized m;
    mat.Materialize(BlockIndexLogicalId(hs(22)), &m);

    // owner CURRENT moves to 2 before completion (e.g. a reorg switched gen)
    owner.SetCurrentGeneration(2);
    BlockIndexHotStatus st = owner.PublishMaterialized(tok, m);
    BOOST_CHECK(st == BlockIndexHotStatus::GENERATION_MISMATCH);
    BOOST_CHECK(!owner.IsResident(BlockIndexLogicalId(hs(22))));
}

// =====================================================================
// A.10.1e — CONCURRENCY HARDENING (deterministic C0-C7 under a leaf lock)
// =====================================================================
// All tests run several threads against one owner + a shared materializer.
// The owner's internal leaf lock (cs) serializes pin/evict/materialize state
// transitions; the materializer is atomic-counted and runs OUTSIDE `cs`.

// --- C0: concurrent Pin same hash from N peers -> one resident, N pins -----
BOOST_AUTO_TEST_CASE(c0_concurrent_pin_same_hash)
{
    SnapshotHotMaterializer mat;
    mat.Add(hs(31), hs(0), uint256(9), 1);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    const int N = 8;
    std::vector<BlockIndexHotHandle> handles(N);
    std::vector<std::thread> threads;
    std::atomic<int> okCount(0);
    boost::barrier bar(N);
    for (int i = 0; i < N; ++i)
    {
        threads.emplace_back([&, i]{
            bar.wait();
            BlockIndexHotStatus st = owner.Pin(BlockIndexLogicalId(hs(31)), &handles[i]);
            if (st == BlockIndexHotStatus::OK) ++okCount;
        });
    }
    for (auto& t : threads) t.join();
    BOOST_CHECK_EQUAL(okCount.load(), N);
    BOOST_CHECK(owner.ResidentCount() == (size_t)1); // ONE resident object
    BOOST_CHECK(owner.PinCount() == (size_t)N);
    // All handles point to the SAME object.
    CBlockIndex* p0 = handles[0].Get();
    for (int i = 1; i < N; ++i) BOOST_CHECK(handles[i].Get() == p0);
    // Release all -> eligible.
    for (auto& h : handles) h.Reset();
    BOOST_CHECK(owner.PinCount() == (size_t)0);
    BOOST_CHECK(!owner.EvictEligible().empty());
}

// --- C1: concurrent Pin/Evict race cannot free a live pin ----------------
BOOST_AUTO_TEST_CASE(c1_pin_vs_evict_race)
{
    SnapshotHotMaterializer mat;
    mat.Add(hs(2), hs(0), uint256(3), 1);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    BlockIndexHotHandle h;
    owner.Pin(BlockIndexLogicalId(hs(2)), &h); // pin held

    // Concurrent eviction attempts while pinned.
    std::thread t1([&]{ for (int i=0;i<2000;++i) owner.EvictResident(BlockIndexLogicalId(hs(2))); });
    std::thread t2([&]{ for (int i=0;i<2000;++i) owner.EvictResident(BlockIndexLogicalId(hs(2))); });
    std::thread t3([&]{ for (int i=0;i<2000;++i) owner.EvictResident(BlockIndexLogicalId(hs(2))); });
    t1.join(); t2.join(); t3.join();

    // The pin must have survived every eviction attempt.
    BlockIndexHotStatus st = owner.EvictResident(BlockIndexLogicalId(hs(2)));
    BOOST_CHECK(st == BlockIndexHotStatus::EVICTION_BLOCKED);
    BOOST_CHECK(h.IsValid());
    CBlockIndex* p = h.Get();
    BOOST_REQUIRE(p != NULL);
    BOOST_CHECK(owner.IsResident(BlockIndexLogicalId(hs(2))));
    h.Reset();
    BOOST_CHECK(owner.IsResident(BlockIndexLogicalId(hs(2)))); // still resident until evicted
}

// --- C2: concurrent Pin/Release -> PinCount returns to 0, no double-free ----
BOOST_AUTO_TEST_CASE(c2_concurrent_pin_release)
{
    SnapshotHotMaterializer mat;
    mat.Add(hs(3), hs(0), uint256(5), 1);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    const int N = 8;
    std::vector<std::thread> ts;
    boost::barrier bar(N);
    for (int i = 0; i < N; ++i)
    {
        ts.emplace_back([&, i]{
            bar.wait();
            for (int r = 0; r < 100; ++r)
            {
                BlockIndexHotHandle h;
                owner.Pin(BlockIndexLogicalId(hs(3)), &h);
                h.Reset();
            }
        });
    }
    for (auto& t : ts) t.join();
    BOOST_CHECK(owner.PinCount() == (size_t)0);
    BOOST_CHECK(owner.ResidentCount() == (size_t)1);
    BlockIndexHotMetrics m = owner.Metrics();
    BOOST_CHECK(m.pinnedCount == 0);
}

// --- C3: concurrent generation rollover vs delayed completion ------------
BOOST_AUTO_TEST_CASE(c3_gen_rollover_stale_completion)
{
    SnapshotHotMaterializer mat;
    mat.Add(hs(9), hs(0), uint256(9), 1, /*gen=*/1);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(1);
    owner.SetMaterializer(&mat);

    BlockIndexHotToken tok;
    owner.RequestMaterialization(BlockIndexLogicalId(hs(9)), &tok); // PENDING under gen1
    // Another thread rolls CURRENT to 2 before completion arrives.
    std::thread t([&]{ owner.SetCurrentGeneration(2); });
    t.join();
    BlockIndexHotMaterialized m;
    mat.Materialize(BlockIndexLogicalId(hs(9)), &m);
    BlockIndexHotStatus st = owner.PublishMaterialized(tok, m);
    BOOST_CHECK(st == BlockIndexHotStatus::GENERATION_MISMATCH);
    BOOST_CHECK(!owner.IsResident(BlockIndexLogicalId(hs(9))));
    BOOST_CHECK(owner.ResidentCount() == (size_t)0);
}

// --- C4: EvictEligible snapshot vs later Pin race -------------------------
BOOST_AUTO_TEST_CASE(c4_evict_snapshot_vs_pin)
{
    SnapshotHotMaterializer mat;
    mat.Add(hs(1), hs(0), uint256(9), 1);
    mat.Add(hs(2), hs(0), uint256(4), 1);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    // materialize two unpinned (evictable) objects
    BlockIndexHotHandle h1; owner.Pin(BlockIndexLogicalId(hs(1)), &h1); h1.Reset();
    BlockIndexHotHandle h2; owner.Pin(BlockIndexLogicalId(hs(2)), &h2); h2.Reset();

    std::vector<BlockIndexLogicalId> snapshot = owner.EvictEligible();
    BOOST_CHECK(snapshot.size() >= (size_t)2); // both are evictable in the snapshot

    // Whereas the snapshot said "evictable", a consumer concurrently Pins hs(1).
    BlockIndexHotHandle hKeep;
    std::thread pinT([&]{ owner.Pin(BlockIndexLogicalId(hs(1)), &hKeep); });
    pinT.join();
    // The snapshot is advisory; the owner must re-check pins under the lock at
    // eviction time. Because hKeep now holds a pin, eviction of hs(1) must block.
    BlockIndexHotStatus st = owner.EvictResident(BlockIndexLogicalId(hs(1)));
    BOOST_CHECK(st == BlockIndexHotStatus::EVICTION_BLOCKED);
    // hs(2) remains evictable.
    BlockIndexHotStatus st2 = owner.EvictResident(BlockIndexLogicalId(hs(2)));
    BOOST_CHECK(st2 == BlockIndexHotStatus::OK);
    hKeep.Reset();
}

// --- C5: anchor vs eviction race -----------------------------------------
BOOST_AUTO_TEST_CASE(c5_anchor_vs_eviction)
{
    SnapshotHotMaterializer mat;
    mat.Add(hs(0), uint256(0), uint256(2), 0); // genesis anchor
    mat.Add(hs(5), hs(0), uint256(7), 1);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);
    owner.PinPermanent(BlockIndexLogicalId(hs(0)));

    BlockIndexHotHandle h5; owner.Pin(BlockIndexLogicalId(hs(5)), &h5); h5.Reset();

    // Concurrent eviction attempts against the anchor while another thread pins it.
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; ++i)
        ts.emplace_back([&]{ for (int r=0;r<2000;++r) owner.EvictResident(BlockIndexLogicalId(hs(0))); });
    ts.emplace_back([&]{ BlockIndexHotHandle h; owner.Pin(BlockIndexLogicalId(hs(0)), &h); h.Reset(); });
    for (auto& t : ts) t.join();

    BlockIndexHotStatus st = owner.EvictResident(BlockIndexLogicalId(hs(0)));
    BOOST_CHECK(st == BlockIndexHotStatus::EVICTION_BLOCKED);
    BOOST_CHECK(owner.IsResident(BlockIndexLogicalId(hs(0))));
    // non-anchor still evictable
    BlockIndexHotStatus st5 = owner.EvictResident(BlockIndexLogicalId(hs(5)));
    BOOST_CHECK(st5 == BlockIndexHotStatus::OK);
}

// --- C6: concurrent metrics snapshot is consistent (no torn read) ---------
BOOST_AUTO_TEST_CASE(c6_concurrent_metrics_consistent)
{
    SnapshotHotMaterializer mat;
    for (int i = 1; i <= 16; ++i) mat.Add(hs(i), hs(0), uint256(i + 1), 1);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    std::atomic<bool> stop(false);
    std::atomic<int> torn(0);
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; ++i)
        ts.emplace_back([&, i]{
            while (!stop)
            {
                BlockIndexHotHandle h;
                owner.Pin(BlockIndexLogicalId(hs(1 + (i % 16))), &h);
                h.Reset();
            }
        });
    // Reader thread samples Metrics() repeatedly; they must never be inconsistent.
    std::thread reader([&]{
        for (int k = 0; k < 5000; ++k)
        {
            BlockIndexHotMetrics m = owner.Metrics();
            if (m.residentCount < 0 || m.pinnedCount < 0 || m.evictableCount < 0 ||
                m.residentCount > 16)
                ++torn;
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    stop = true;
    for (auto& t : ts) t.join();
    reader.join();
    BOOST_CHECK_EQUAL(torn.load(), 0); // metrics never inconsistent/torn
}

// --- C7: raw-pointer escape guarantee under a concurrent pin --------------
BOOST_AUTO_TEST_CASE(c7_raw_pointer_valid_under_pin)
{
    SnapshotHotMaterializer mat;
    mat.Add(hs(7), hs(0), uint256(9), 1);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    BlockIndexHotHandle h;
    owner.Pin(BlockIndexLogicalId(hs(7)), &h);
    CBlockIndex* p = h.Get();
    BOOST_REQUIRE(p != NULL);
    // While the pin is held, concurrent eviction must be blocked, so the raw
    // pointer handed out via Get() remains valid.
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; ++i)
        ts.emplace_back([&]{ for (int r=0;r<2000;++r) owner.EvictResident(BlockIndexLogicalId(hs(7))); });
    for (auto& t : ts) t.join();
    BOOST_CHECK(h.IsValid());
    BOOST_CHECK(h.Get() == p); // same live object, not freed
    h.Reset();
}

BOOST_AUTO_TEST_SUITE_END()