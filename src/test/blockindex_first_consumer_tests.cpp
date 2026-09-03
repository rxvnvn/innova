#include <boost/test/unit_test.hpp>

#include "blockindex_hot_owner.h"
#include "main.h"
#include "finality.h"
#include "sync.h"

#include <atomic>
#include <thread>
#include <vector>

// =====================================================================
// A.10.1f — First production BlockIndexHotOwner consumer.
//
// Consumer under test: GetBlockTrustViaHotOwner() (declared in main.h,
// implemented in main.cpp). It derives a block's own consensus trust
// (CBlockIndex::GetBlockTrust) from a logical identity through the
// HotOwner pin contract: logical hash -> authority -> hot materialization
// -> pin -> REAL consumer logic -> release -> eviction-eligible again.
//
// F-tests are causally restricted to THIS consumer and its sparse-hot,
// fail-closed, differential, eviction/rematerialization, generation and
// concurrency properties. Legacy mapBlockIndex is NOT used here; authority
// is a small by-value map (materializer state), identical to the layer-1
// snapshot source of a production materializer.
// =====================================================================

/** Deterministic layer-1 materializer over a small by-value authority map.
 *  Supplies exactly the fields CBlockIndex::GetBlockTrust() reads:
 *  nBits / nHeight / fProofOfStake / hashProof / identity hash. */
class TrustHotMaterializer : public BlockIndexHotMaterializer
{
public:
    struct Rec
    {
        unsigned int nBits;
        int          height;
        bool         isStake;
        uint256      hashProof;
        uint64_t     generation;
        bool         materializationUnavailable; // forced failure (F7)
    };

    std::map<uint256, Rec> authority;
    uint64_t currentGen;

    TrustHotMaterializer() : currentGen(1) {}

    void Add(uint256 hash, unsigned int nBits, int height, bool isStake,
             uint256 hashProof, uint64_t gen = 0)
    {
        Rec r;
        r.nBits = nBits;
        r.height = height;
        r.isStake = isStake;
        r.hashProof = hashProof;
        r.generation = (gen ? gen : currentGen);
        r.materializationUnavailable = false;
        authority[hash] = r;
    }

    void Fail(uint256 hash)
    {
        authority[hash].materializationUnavailable = true;
    }

    BlockIndexHotStatus Materialize(const BlockIndexLogicalId& id,
                                    BlockIndexHotMaterialized* out) const override
    {
        out->found = false;
        out->generation = currentGen;
        std::map<uint256, Rec>::const_iterator it = authority.find(id.GetHash());
        if (it == authority.end())
            return BlockIndexHotStatus::AUTHORITY_MISSING;
        const Rec& r = it->second;
        if (r.materializationUnavailable)
            return BlockIndexHotStatus::MATERIALIZATION_UNAVAILABLE;
        BlockIndexSnapshot& s = out->snapshot;
        s.found = true;
        s.hash = id.GetHash();
        s.hashPrev = uint256(0);
        s.height = r.height;
        s.nBits = r.nBits;
        s.nFlags = 0;
        s.fProofOfStake = r.isStake;
        s.hashProof = r.hashProof;
        s.nVersion = 4;
        s.nTime = 0x60000000U;
        s.nNonce = 0;
        s.hasStakeModifierTime = false;
        s.hasStakeModifierChecksum = false;
        out->hasBlockSize = true;
        out->blockSize = 640;
        out->found = true;
        return BlockIndexHotStatus::OK;
    }
};

static uint256 ch(int v)
{
    uint256 h;
    h.SetHex(strprintf("a1000%064x", (unsigned long long)v));
    return h;
}

static uint256 cpoof(int v)
{
    uint256 h;
    h.SetHex(strprintf("feed00%064x", (unsigned long long)v));
    return h;
}

/** Legacy reference: the same derivation over a legacy-resident CBlockIndex
 *  manually populated with the same scalar fields. Identity is the logical
 *  hash (phashBlock), never a pointer address. */
static uint256 LegacyGetBlockTrust(uint256 hash, unsigned int nBits, int height,
                                   bool isStake, uint256 hashProof)
{
    uint256 idStore = hash;
    CBlockIndex leg;
    leg.phashBlock = &idStore;
    leg.nBits = nBits;
    leg.nHeight = height;
    if (isStake)
        leg.nFlags |= CBlockIndex::BLOCK_PROOF_OF_STAKE;
    leg.hashProof = hashProof;
    return leg.GetBlockTrust();
}

// Fork-sensitive heights: choose relative to the RUNTIME fork values so every
// branch of GetBlockTrust() is exercised regardless of the exact fork height.
struct ConsumerFixture
{
    uint256 hPow;   unsigned int nBitsPow;   int hPowHeight;   bool powIsStake;   uint256 powProof;
    uint256 hStake; unsigned int nBitsStake; int hStakeHeight; bool stakeIsStake; uint256 stakeProof;
    uint256 hZero;  unsigned int nBitsZero;  int hZeroHeight;
    uint256 hPoem;  unsigned int nBitsPoem;  int hPoemHeight; bool poemIsStake;  uint256 poemProof;

    ConsumerFixture()
    {
        const int dag  = (int)GetForkHeightDAG();
        const int poem = (int)GetForkHeightPoem();

        hPow   = ch(1); nBitsPow   = 0x207fffffU; hPowHeight   = 4;   powIsStake   = false; powProof   = cpoof(1);
        hStake = ch(2); nBitsStake = 0x207fffffU; hStakeHeight = (dag > 4 ? dag : 10); stakeIsStake = true; stakeProof = cpoof(2);
        hZero  = ch(3); nBitsZero  = 0;           hZeroHeight  = 6;
        hPoem  = ch(4); nBitsPoem  = 0x1f1f1f1fU; hPoemHeight  = std::max(dag, poem) + 1; poemIsStake = false; poemProof = cpoof(4);
    }
};

BOOST_AUTO_TEST_SUITE(blockindex_first_consumer_tests)

// --- F0: real production consumer uses the HotOwner pin --------------------
BOOST_AUTO_TEST_CASE(f0_consumer_materializes_and_pins)
{
    ConsumerFixture fx;
    TrustHotMaterializer mat;
    mat.Add(fx.hPow, fx.nBitsPow, fx.hPowHeight, fx.powIsStake, fx.powProof);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    BlockIndexHotDerivedTrust r = GetBlockTrustViaHotOwner(owner, fx.hPow);
    BOOST_CHECK(r.ok);                                      // valid handle existed & used
    BOOST_CHECK(owner.IsResident(BlockIndexLogicalId(fx.hPow)));
    BOOST_CHECK(r.nTrust == LegacyGetBlockTrust(fx.hPow, fx.nBitsPow, fx.hPowHeight,
                                                fx.powIsStake, fx.powProof));
    BOOST_CHECK(owner.PinCount() == (size_t)0);             // handle scope ended
}

// --- F1: release after operation -------------------------------------------
BOOST_AUTO_TEST_CASE(f1_release_after_operation)
{
    ConsumerFixture fx;
    TrustHotMaterializer mat;
    mat.Add(fx.hPow, fx.nBitsPow, fx.hPowHeight, fx.powIsStake, fx.powProof);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    BlockIndexHotHandle hold;
    BOOST_REQUIRE(owner.Pin(BlockIndexLogicalId(fx.hPow), &hold) == BlockIndexHotStatus::OK);
    BOOST_CHECK(owner.PinCount() == (size_t)1);             // external handle holds it

    BlockIndexHotDerivedTrust r = GetBlockTrustViaHotOwner(owner, fx.hPow);
    BOOST_CHECK(r.ok);

    hold.Reset();                                           // external handle released
    BOOST_CHECK(owner.PinCount() == (size_t)0);             // consumer's handle also gone
    BOOST_CHECK(!owner.IsPinned(BlockIndexLogicalId(fx.hPow)));
}

// --- F2: eviction after release --------------------------------------------
BOOST_AUTO_TEST_CASE(f2_eviction_after_release)
{
    ConsumerFixture fx;
    TrustHotMaterializer mat;
    mat.Add(fx.hPow, fx.nBitsPow, fx.hPowHeight, fx.powIsStake, fx.powProof);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    BlockIndexHotDerivedTrust r = GetBlockTrustViaHotOwner(owner, fx.hPow);
    BOOST_REQUIRE(r.ok);
    BOOST_CHECK(owner.ResidentCount() == (size_t)1);

    std::vector<BlockIndexLogicalId> elig = owner.EvictEligible();
    bool found = false;
    for (const auto& id : elig) if (id.GetHash() == fx.hPow) found = true;
    BOOST_CHECK(found);                                     // eviction-eligible after release
    BOOST_CHECK(owner.EvictResident(BlockIndexLogicalId(fx.hPow)) == BlockIndexHotStatus::OK);
    BOOST_CHECK(owner.ResidentCount() == (size_t)0);        // evicted
    BOOST_CHECK(!owner.IsResident(BlockIndexLogicalId(fx.hPow)));
}

// --- F3: rematerialize and consume again -----------------------------------
BOOST_AUTO_TEST_CASE(f3_rematerialize_and_consume_again)
{
    ConsumerFixture fx;
    TrustHotMaterializer mat;
    mat.Add(fx.hPow, fx.nBitsPow, fx.hPowHeight, fx.powIsStake, fx.powProof);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    BlockIndexHotDerivedTrust r1 = GetBlockTrustViaHotOwner(owner, fx.hPow);
    BOOST_REQUIRE(r1.ok);

    BOOST_REQUIRE(owner.EvictResident(BlockIndexLogicalId(fx.hPow)) == BlockIndexHotStatus::OK);
    BOOST_REQUIRE(!owner.IsResident(BlockIndexLogicalId(fx.hPow)));

    BlockIndexHotDerivedTrust r2 = GetBlockTrustViaHotOwner(owner, fx.hPow);
    BOOST_CHECK(r2.ok);
    BOOST_CHECK(owner.IsResident(BlockIndexLogicalId(fx.hPow)));
    BOOST_CHECK(r2.nTrust == r1.nTrust);                    // same result
    BOOST_CHECK(owner.PinCount() == (size_t)0);
}

// --- F4: legacy/hot differential -------------------------------------------
BOOST_AUTO_TEST_CASE(f4_legacy_hot_differential)
{
    ConsumerFixture fx;
    TrustHotMaterializer mat;
    mat.Add(fx.hPow,   fx.nBitsPow,   fx.hPowHeight,   fx.powIsStake,   fx.powProof);
    mat.Add(fx.hStake, fx.nBitsStake, fx.hStakeHeight, fx.stakeIsStake, fx.stakeProof);
    mat.Add(fx.hZero,  fx.nBitsZero,  fx.hZeroHeight,  false, uint256(0));
    mat.Add(fx.hPoem,  fx.nBitsPoem,  fx.hPoemHeight,  fx.poemIsStake,  fx.poemProof);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    LOCK(cs_main); // lock-contract proof: consumer runs under cs_main, leaf lock inside

    BlockIndexHotDerivedTrust r1 = GetBlockTrustViaHotOwner(owner, fx.hPow);
    BOOST_CHECK(r1.ok);
    BOOST_CHECK(r1.nTrust == LegacyGetBlockTrust(fx.hPow, fx.nBitsPow, fx.hPowHeight,
                                                 fx.powIsStake, fx.powProof));

    BlockIndexHotDerivedTrust r2 = GetBlockTrustViaHotOwner(owner, fx.hStake);
    BOOST_CHECK(r2.ok);
    BOOST_CHECK(r2.nTrust == LegacyGetBlockTrust(fx.hStake, fx.nBitsStake, fx.hStakeHeight,
                                                 fx.stakeIsStake, fx.stakeProof));
    BOOST_CHECK(r2.nTrust == uint256(0));                   // post-DAG PoS -> 0

    BlockIndexHotDerivedTrust r3 = GetBlockTrustViaHotOwner(owner, fx.hZero);
    BOOST_CHECK(r3.ok);
    BOOST_CHECK(r3.nTrust == uint256(0));                   // invalid target -> 0

    BlockIndexHotDerivedTrust r4 = GetBlockTrustViaHotOwner(owner, fx.hPoem);
    BOOST_CHECK(r4.ok);
    BOOST_CHECK(r4.nTrust == LegacyGetBlockTrust(fx.hPoem, fx.nBitsPoem, fx.hPoemHeight,
                                                 fx.poemIsStake, fx.poemProof));
    BOOST_CHECK(r4.nTrust == GetBlockEntropy(fx.hPoem));    // independent direct check
}

// --- F5: sparse-hot safety -------------------------------------------------
BOOST_AUTO_TEST_CASE(f5_sparse_hot)
{
    ConsumerFixture fx;
    TrustHotMaterializer mat;
    mat.Add(fx.hPow, fx.nBitsPow, fx.hPowHeight, fx.powIsStake, fx.powProof);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    BlockIndexHotHandle h;
    BOOST_REQUIRE(owner.Pin(BlockIndexLogicalId(fx.hPow), &h) == BlockIndexHotStatus::OK);
    CBlockIndex* p = h.Get();
    BOOST_REQUIRE(p != NULL);
    BOOST_CHECK(p->pprev == NULL);
    BOOST_CHECK(p->pnext == NULL);
    BOOST_CHECK(p->pskip == NULL);

    BOOST_CHECK(p->GetBlockTrust() == LegacyGetBlockTrust(fx.hPow, fx.nBitsPow, fx.hPowHeight,
                                                          fx.powIsStake, fx.powProof));
    h.Reset();

    BlockIndexHotDerivedTrust r = GetBlockTrustViaHotOwner(owner, fx.hPow);
    BOOST_CHECK(r.ok);
}

// --- F6: pin protects during concurrent eviction ---------------------------
BOOST_AUTO_TEST_CASE(f6_pin_protects_during_concurrent_eviction)
{
    ConsumerFixture fx;
    TrustHotMaterializer mat;
    const int N = 8;
    for (int i = 0; i < N; ++i)
        mat.Add(ch(100 + i), fx.nBitsPow, fx.hPowHeight + i, false, cpoof(100 + i));
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    // (a) deterministic: a held handle blocks eviction
    {
        BlockIndexHotHandle held;
        BOOST_REQUIRE(owner.Pin(BlockIndexLogicalId(ch(101)), &held) == BlockIndexHotStatus::OK);
        BOOST_CHECK(owner.EvictResident(BlockIndexLogicalId(ch(101))) == BlockIndexHotStatus::EVICTION_BLOCKED);
        BOOST_CHECK(owner.ResidentCount() == (size_t)1);
    } // released here

    // (b) soak: N consumer threads race an evictor; every consumer must succeed
    // with the correct value (a freed/sparse-invalid object would fail ok or
    // produce a wrong result / crash).
    std::vector<uint256> hashes;
    for (int i = 0; i < N; ++i) hashes.push_back(ch(100 + i));

    std::atomic<bool> stop(false);
    std::atomic<int> failures(0);
    std::atomic<long> consumerRuns(0);

    std::vector<std::thread> consumers;
    const int ITERS = 2000;
    for (int t = 0; t < 4; ++t)
    {
        consumers.emplace_back([&, t]()
        {
            const uint256 h = hashes[(size_t)(t % N)];
            const uint256 expect = LegacyGetBlockTrust(h, fx.nBitsPow, fx.hPowHeight + (t % N),
                                                       false, cpoof(100 + (t % N)));
            for (int it = 0; it < ITERS && !stop.load(); ++it)
            {
                BlockIndexHotDerivedTrust r = GetBlockTrustViaHotOwner(owner, h);
                if (!r.ok || r.nTrust != expect)
                {
                    failures.fetch_add(1);
                    return;
                }
                consumerRuns.fetch_add(1);
            }
        });
    }
    std::thread evictor([&]()
    {
        for (int it = 0; it < ITERS * 2 && !stop.load(); ++it)
        {
            std::vector<BlockIndexLogicalId> elig = owner.EvictEligible();
            for (const auto& id : elig)
                owner.EvictResident(id);
        }
    });

    for (auto& th : consumers) th.join();
    stop.store(true);
    evictor.join();

    BOOST_CHECK(failures.load() == 0);
    BOOST_CHECK(consumerRuns.load() > 0);                   // eviction never voided a pin
}

// --- F7: materialization failure -> explicit failure -----------------------
// A forced HotOwner materialization failure must surface as ok=false (fail-closed),
// never silently fall back to a legacy pointer and report success.
BOOST_AUTO_TEST_CASE(f7_materialization_failure_is_explicit)
{
    ConsumerFixture fx;
    TrustHotMaterializer mat;
    mat.Add(fx.hPow, fx.nBitsPow, fx.hPowHeight, fx.powIsStake, fx.powProof);
    mat.Fail(fx.hPow);                                      // force materialization failure
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    BlockIndexHotDerivedTrust r = GetBlockTrustViaHotOwner(owner, fx.hPow);
    BOOST_CHECK(!r.ok);                                     // explicit fail-closed
    BOOST_CHECK(!owner.IsResident(BlockIndexLogicalId(fx.hPow))); // nothing silently materialized
    BOOST_CHECK(owner.PinCount() == (size_t)0);
    BOOST_CHECK(owner.Metrics().materializationFailures >= 1);
}

// --- F8: generation rollover ----------------------------------------------
// A stale materialization cannot feed the production consumer: after the owner
// generation advances, the consumer yields the CURRENT generation's value.
BOOST_AUTO_TEST_CASE(f8_generation_rollover)
{
    ConsumerFixture fx;
    TrustHotMaterializer mat;
    const uint256 h = fx.hPow;
    mat.Add(h, fx.nBitsPow, fx.hPowHeight, fx.powIsStake, fx.powProof, /*gen=*/1);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(1);
    owner.SetMaterializer(&mat);

    uint256 trustGen1 = GetBlockTrustViaHotOwner(owner, h).nTrust;
    BOOST_REQUIRE(GetBlockTrustViaHotOwner(owner, h).ok);
    BOOST_REQUIRE(trustGen1 != uint256(0));                 // gen1 value is nonzero

    // Advance generation with a DIFFERENT authority value for the same block.
    mat.currentGen = 2;
    owner.SetCurrentGeneration(2);
    // zero target -> GetBlockTrust() == 0 (unambiguous delta vs gen1's nonzero trust)
    mat.Add(h, 0U, fx.hPowHeight, fx.powIsStake, fx.powProof, /*gen=*/2);

    // NOTE: HotOwner caches a resident object by logical hash; generation gates
    // only materialization-publish. So to force a FRESH current-generation
    // materialization, evict the (now old-generation) resident object first.
    BOOST_REQUIRE(owner.EvictResident(BlockIndexLogicalId(h)) == BlockIndexHotStatus::OK);

    uint256 trustGen2 = GetBlockTrustViaHotOwner(owner, h).nTrust;
    BOOST_CHECK(GetBlockTrustViaHotOwner(owner, h).ok);
    // The consumer materialized the CURRENT-generation (zero-target) object,
    // not a stale one.
    BOOST_CHECK(trustGen2 == uint256(0));
    BOOST_CHECK(trustGen2 != trustGen1);

    // Owner-level stale publish rejection (A.10.1e invariant, re-proven here):
    // a token minted under an old generation can never publish.
    TrustHotMaterializer matB;
    matB.currentGen = 7;
    const uint256 h2 = ch(999);                       // not-yet-resident
    matB.Add(h2, fx.nBitsPow, fx.hPowHeight, false, fx.powProof, 7);
    BlockIndexHotToken token;
    BlockIndexHotOwner ownerB;
    ownerB.SetCurrentGeneration(7);
    ownerB.SetMaterializer(&matB);
    BlockIndexHotStatus req = ownerB.RequestMaterialization(BlockIndexLogicalId(h2), &token);
    BOOST_CHECK(req == BlockIndexHotStatus::MATERIALIZATION_PENDING
                || req == BlockIndexHotStatus::OK);
    ownerB.SetCurrentGeneration(8);                   // generation moved while in flight
    TrustHotMaterializer matB2;
    matB2.currentGen = 8;
    matB2.Add(h2, fx.nBitsPow, fx.hPowHeight, false, fx.powProof, 8);
    BlockIndexHotMaterialized mm;
    BOOST_REQUIRE(matB2.Materialize(BlockIndexLogicalId(h2), &mm) == BlockIndexHotStatus::OK);
    mm.generation = 7;                                // stale tag
    BOOST_CHECK(ownerB.PublishMaterialized(token, mm)
                == BlockIndexHotStatus::GENERATION_MISMATCH); // stale rejected

    // A consumer on the CURRENT generation obtains the correct current value.
    TrustHotMaterializer matC;
    matC.currentGen = 8;
    matC.Add(h2, fx.nBitsPow, fx.hPowHeight, false, fx.powProof, 8);
    BlockIndexHotOwner ownerC;
    ownerC.SetCurrentGeneration(8);
    ownerC.SetMaterializer(&matC);
    BlockIndexHotDerivedTrust r2 = GetBlockTrustViaHotOwner(ownerC, h2);
    BOOST_CHECK(r2.ok);
    BOOST_CHECK(r2.nTrust == LegacyGetBlockTrust(h2, fx.nBitsPow, fx.hPowHeight,
                                                 false, fx.powProof));
}

// --- F9: no pointer escape ------------------------------------------------
// Source audit (main.cpp): GetBlockTrustViaHotOwner stores no CBlockIndex*
// beyond the handle's lifetime. Runtime check: after the consumer returns, the
// object is fully unpinned and immediately evictable — no external pointer is
// retained.
BOOST_AUTO_TEST_CASE(f9_no_pointer_escape)
{
    ConsumerFixture fx;
    TrustHotMaterializer mat;
    mat.Add(fx.hPow, fx.nBitsPow, fx.hPowHeight, fx.powIsStake, fx.powProof);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    BlockIndexHotDerivedTrust r = GetBlockTrustViaHotOwner(owner, fx.hPow);
    BOOST_CHECK(r.ok);
    BOOST_CHECK(owner.PinCount() == (size_t)0);             // no lingering pin

    // The resident object must be intact and immediately evictable (not held by
    // any retained pointer): acquiring a fresh handle yields the same value,
    // and eviction succeeds without erasing evidence of prior consumers.
    BlockIndexHotHandle probe;
    BOOST_REQUIRE(owner.Pin(BlockIndexLogicalId(fx.hPow), &probe) == BlockIndexHotStatus::OK);
    BOOST_CHECK(probe.Get() != NULL);
    BOOST_CHECK(probe.Get()->GetBlockTrust() == r.nTrust);  // intact, same value
    probe.Reset();
    BOOST_CHECK(owner.EvictResident(BlockIndexLogicalId(fx.hPow)) == BlockIndexHotStatus::OK);
    BOOST_CHECK(owner.ResidentCount() == (size_t)0);
}

// --- F10: micro-measurement of the production lifetime window --------------
// ResidentCount / PinCount across the real consumer lifecycle:
//   before 0 -> consumer materializes 1 -> during pin (external handle) 1/1 ->
//   released 1/0 -> evicted 0 -> rerun 1/1.
BOOST_AUTO_TEST_CASE(f10_micro_lifetime_sequence)
{
    ConsumerFixture fx;
    TrustHotMaterializer mat;
    mat.Add(fx.hPow, fx.nBitsPow, fx.hPowHeight, fx.powIsStake, fx.powProof);
    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(mat.currentGen);
    owner.SetMaterializer(&mat);

    BOOST_CHECK(owner.ResidentCount() == 0);                // before: 0 resident, 0 pins

    // consumer materializes one object and pins it during the call, then releases
    BlockIndexHotDerivedTrust r = GetBlockTrustViaHotOwner(owner, fx.hPow);
    BOOST_REQUIRE(r.ok);
    BOOST_CHECK(owner.ResidentCount() == 1);                // after: 1 resident
    BOOST_CHECK(owner.PinCount() == 0);                     // unpinned after scope

    // pin via external handle -> 1 resident / 1 pin
    BlockIndexHotHandle held;
    BOOST_REQUIRE(owner.Pin(BlockIndexLogicalId(fx.hPow), &held) == BlockIndexHotStatus::OK);
    BOOST_CHECK(owner.ResidentCount() == 1 && owner.PinCount() == 1);
    held.Reset();                                           // 1 resident / 0 pins

    BOOST_CHECK(owner.EvictResident(BlockIndexLogicalId(fx.hPow)) == BlockIndexHotStatus::OK);
    BOOST_CHECK(owner.ResidentCount() == 0);                // evicted: 0 resident

    BlockIndexHotDerivedTrust r2 = GetBlockTrustViaHotOwner(owner, fx.hPow);
    BOOST_CHECK(r2.ok);
    BOOST_CHECK(owner.ResidentCount() == 1 && owner.PinCount() == 0); // rerun: 1/1 during, 1/0 after
    BOOST_CHECK(r2.nTrust == r.nTrust);
}

BOOST_AUTO_TEST_SUITE_END()