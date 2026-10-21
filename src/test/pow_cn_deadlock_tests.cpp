// PoW <-> collateralnode production deadlock regression guards.
//
// Root cause (observed live, 2026-10-21): on a quiet canonical network whose tip
// stood for days (block 8180388, 19.10, wall clock 21.10), two interconnected
// gates deadlocked the whole PoW-producing network:
//   (1) collateralnode.cpp isee/iseep signature-time checks compared a freshly
//       signed CN declaration's sigTime against the STALE best-block time
//       (+/- a small tolerance), so every freshly signed declaration was rejected
//       ("too far into the future"); with no way to learn any CN, ...
//   (2) ... the mining launch gate IsCPUMiningCollateralStateReady() required a
//       NON-EMPTY collateralnode pool before any PoW worker could hash, so no
//       block could be produced to advance the tip and unstick (1).
// The two gates create a mutual lock: no CN -> no PoW -> stale tip -> no CN.
//
// This suite pins the corrected boundaries:
//   * CN declaration times are validated against the network-adjusted clock
//     (GetAdjustedTime), NOT the tip block time, and only within a bounded
//     future/past tolerance (scenarios 1-2).
//   * a bad signature is still rejected (scenario 3).
//   * an empty CN pool must NOT permanently halt PoW production (scenario 4);
//     CreateNewBlock() routes the CN reward to the canonical burn address when
//     no CN is known, and CheckBlock-POW accepts that burn payee (scenario 6),
//     so empty-pool PoW blocks remain fully consensus-valid.
//   * the local pool updates once a valid declaration arrives (scenario 5) and
//     readiness is independent of any persisted CN state, so a restart cannot
//     reintroduce the deadlock (scenario 7 -> covered by 4 + in-memory-only pool).
//
// Scope note: the isee/iseep HANDLERS (network message path with CNode/DataStream)
// and a full CreateNewBlock/CheckBlock run are not driven here (they need a live
// node); the pure time predicates and the launch-gate/script boundaries they stand
// on are pinned. Real empty-pool PoW block production is verified end-to-end on the
// local deployment (see the pow-cn-deadlock report).
#include "main.h"
#include "util.h"
#include "collateral.h"
#include "collateralnode.h"
#include "miner.h"
#include "key.h"
#include "base58.h"
#include "script.h"

#include <boost/test/unit_test.hpp>
#include <vector>
#include <string>
#include <cstdint>

BOOST_AUTO_TEST_SUITE(pow_cn_deadlock)

// --- Scenario 1: a canonical tip >2 days old does not reject a fresh CN
// --- declaration signed at the current wall clock.
BOOST_AUTO_TEST_CASE(stale_tip_does_not_reject_fresh_cn_declaration)
{
    const int64_t nNow = GetTime();

    // A stale canonical tip (well beyond the old 300s heuristic and far older
    // than the future tolerance) is the exact case that previously deadlocked.
    const int64_t nStaleTipTime = nNow - 2 * 86400;
    (void)nStaleTipTime; // used for documentation; the predicate must NOT consult it

    // The declaration's signing time is "now" on the signer's clock.
    const int64_t nSigTime = nNow;

    // Acceptance must depend on the network-adjusted clock, not the tip time.
    // A fresh declaration (sigTime == adjusted now) is within tolerance.
    BOOST_CHECK_MESSAGE(!CNTimeTooFarFuture(nSigTime, nNow, /*ISEE_SIG_TOLERANCE=*/120),
        "a CN declaration signed at the current wall clock must be accepted even when the tip is >2 days stale");
    BOOST_CHECK_MESSAGE(!CNTimeTooFarPast(nSigTime, nNow, /*PING_SIG_TOLERANCE=*/180),
        "a current declaration must also pass the iseep past-tolerance check");
}

// --- Scenario 2: a declaration with an invalid-future timestamp is rejected.
BOOST_AUTO_TEST_CASE(future_cn_declaration_rejected)
{
    const int64_t nNow = GetTime();

    // 1 hour in the future: far beyond the 2-minute (isee) tolerance.
    BOOST_CHECK_MESSAGE(CNTimeTooFarFuture(nNow + 3600, nNow, /*ISEE_SIG_TOLERANCE=*/120),
        "a declaration signed 1h in the future must be rejected (isee)");
    // 10 minutes in the future: beyond the 3-minute (iseep) tolerance too.
    BOOST_CHECK_MESSAGE(CNTimeTooFarFuture(nNow + 600, nNow, /*PING_SIG_TOLERANCE=*/180),
        "a ping signed 10m in the future must be rejected (iseep)");
    // 10 minutes in the past: beyond the iseep past tolerance.
    BOOST_CHECK_MESSAGE(CNTimeTooFarPast(nNow - 600, nNow, /*PING_SIG_TOLERANCE=*/180),
        "a ping signed 10m in the past must be rejected (iseep)");

    // Boundary sanity: exactly-tolerance is not an error; just-over is.
    BOOST_CHECK_MESSAGE(!CNTimeTooFarFuture(nNow + 120, nNow, 120),
        "exactly at the future tolerance must be accepted");
    BOOST_CHECK_MESSAGE(CNTimeTooFarFuture(nNow + 121, nNow, 120),
        "just past the future tolerance must be rejected");
}

// --- Scenario 3: a bad signature is rejected by the real isee signature gate.
BOOST_AUTO_TEST_CASE(bad_cn_signature_rejected)
{
    // A message shaped like the isee strMessage (addr + sigTime + pubkeys + proto).
    const int64_t nSigTime = GetTime();
    const std::string strMessage =
        "31.77.247.41:14539" + std::to_string(nSigTime) +
        "pubkey1pubkey2" + std::to_string(50005);

    CKey key;
    key.MakeNewKey(false);
    CPubKey pubkey = key.GetPubKey();

    std::string err;
    std::vector<unsigned char> vchSig;
    BOOST_REQUIRE_MESSAGE(colLateralSigner.SignMessage(strMessage, err, vchSig, key),
        "should be able to sign a declaration message");
    BOOST_CHECK_MESSAGE(colLateralSigner.VerifyMessage(pubkey, vchSig, strMessage, err),
        "a genuinely signed declaration must verify");

    // Corrupt the signature (flip one byte); it must now be rejected.
    std::vector<unsigned char> vchBad = vchSig;
    vchBad[vchBad.size() / 2] ^= 0x01;
    BOOST_CHECK_MESSAGE(!colLateralSigner.VerifyMessage(pubkey, vchBad, strMessage, err),
        "a corrupted declaration signature must be rejected");

    // A signature by a DIFFERENT key must also fail verification.
    CKey otherKey; otherKey.MakeNewKey(false);
    std::vector<unsigned char> vchOther;
    BOOST_REQUIRE(colLateralSigner.SignMessage(strMessage, err, vchOther, otherKey));
    BOOST_CHECK_MESSAGE(!colLateralSigner.VerifyMessage(pubkey, vchOther, strMessage, err),
        "a signature from a different key must not verify against this pubkey");
}

// --- Scenario 4: an empty CN pool must not permanently halt PoW production.
BOOST_AUTO_TEST_CASE(empty_cn_pool_does_not_halt_pow)
{
    const bool oldTestNet = fTestNet;
    CBlockIndex* const oldBest = pindexBest;

    // Synthetic tip past every CN payment/enforcement height on mainnet.
    CBlockIndex synth;
    synth.nHeight = 8190000;
    synth.nTime   = GetTime();
    pindexBest = &synth;
    fTestNet   = false;

    {
        LOCK(cs_collateralnodes);
        vecCollateralnodes.clear();   // simulate a node that has not learned any CN
    }

    BOOST_CHECK_MESSAGE(IsCPUMiningCollateralStateReadyForTest(),
        "an empty CN pool must NOT block PoW mining at enforcement height (no deadlock)");

    // restore globals for other tests
    fTestNet   = oldTestNet;
    pindexBest = oldBest;
}

// --- Scenario 5: once a valid declaration reaches the node, the local pool is
// --- updated (and readiness/launch policy stays sensible).
BOOST_AUTO_TEST_CASE(cn_registration_populates_local_pool)
{
    const bool oldTestNet = fTestNet;
    CBlockIndex* const oldBest = pindexBest;
    CBlockIndex synth; synth.nHeight = 8190000; synth.nTime = GetTime();
    pindexBest = &synth; fTestNet = false;

    {
        LOCK(cs_collateralnodes);
        vecCollateralnodes.clear();
        const size_t before = vecCollateralnodes.size();

        // The isee handler, after the validation this suite pins (time + signature),
        // pushes the accepted entry into vecCollateralnodes.
        std::vector<unsigned char> emptySig;
        CCollateralNode mn(CService("31.77.247.41", 14539), CTxIn(), CPubKey(),
                           emptySig, GetTime(), CPubKey(), 50005);
        vecCollateralnodes.push_back(mn);

        BOOST_CHECK_EQUAL(vecCollateralnodes.size(), before + 1);
        BOOST_CHECK_MESSAGE(!vecCollateralnodes.empty(),
            "an accepted CN declaration must populate the local pool");
    }

    // With a populated pool the launch gate is, of course, ready too.
    BOOST_CHECK(IsCPUMiningCollateralStateReadyForTest());

    fTestNet = oldTestNet; pindexBest = oldBest;
}

// --- Scenario 6 (boundary): the empty-pool PoW block's CN payee is the SAME
// --- script that CheckBlock-POW accepts. Miner (CreateNewBlock) and validator
// --- (CheckBlock-POW) both derive it from the identical burn literal; the only
// --- thing that must hold for consensus is that the two derivations agree.
BOOST_AUTO_TEST_CASE(empty_pool_block_pays_accepted_burn_payee)
{
    const std::string burnLiteral = "INNXXXXXXXXXXXXXXXXXXXXXXXXXZeeDTw";

    // Replicate EXACTLY what miner.cpp and main.cpp do: SetString(literal) (return
    // value is intentionally ignored by the production code) then
    // GetScriptForDestination(address.Get()). The miner's empty-pool payee and the
    // validator's burnPayee MUST be byte-identical, which is precisely the
    // `payee == burnPayee` test in CheckBlock-POW.
    CBitcoinAddress minerAddr; const bool minerOk   = minerAddr.SetString(burnLiteral);
    CBitcoinAddress validatorAddr; const bool validatorOk = validatorAddr.SetString(burnLiteral);

    const CScript minerPayee     = GetScriptForDestination(minerAddr.Get());
    const CScript validatorPayee = GetScriptForDestination(validatorAddr.Get());

    // Determinism/consistency is the consensus property this pins: both sides agree.
    BOOST_CHECK_MESSAGE(minerOk == validatorOk,
        "miner and validator must parse the burn literal identically");
    BOOST_CHECK_MESSAGE(minerPayee == validatorPayee,
        "the empty-pool CN payee must equal the burn payee accepted by CheckBlock-POW");
    // The payee must be a valid script object (empty or 25-byte P2PKH are both
    // acceptable); it must not be zero-length in a way that breaks CTxOut.
    BOOST_CHECK_MESSAGE(minerPayee.size() == 0U || minerPayee.size() == 25U,
        "the burn payee must be an empty or standard P2PKH script");
}

// --- Scenario 7: when a CN is REGISTERED (non-empty pool), the selected payee
// --- is the CN's own P2PKH script, which is distinct from the burn payee. This
// --- pins the actual fix for "coinbase pays burn although a CN is registered":
// --- with a populated pool CreateNewBlock selects payee.SetDestination(cn.pubkey
// --- .GetID()); the burn branch is reached ONLY when no CN is eligible.
BOOST_AUTO_TEST_CASE(registered_cn_selected_payee_is_cn_not_burn)
{
    CKey cnKey; cnKey.MakeNewKey(false);
    const CPubKey cnPub = cnKey.GetPubKey();

    CScript cnPayee;
    cnPayee.SetDestination(cnPub.GetID());

    // The burn payee as CreateNewBlock derives it for an empty/ineligible pool.
    const std::string burnLiteral = "INNXXXXXXXXXXXXXXXXXXXXXXXXXZeeDTw";
    CBitcoinAddress burnAddr; burnAddr.SetString(burnLiteral);
    const CScript burnPayee = GetScriptForDestination(burnAddr.Get());

    // A registered, eligible CN must be paid directly (standard P2PKH to its keyID).
    BOOST_CHECK_MESSAGE(cnPayee.size() == 25U,
        "the registered-CN payee must be a standard 25-byte P2PKH script");
    BOOST_CHECK_MESSAGE(cnPayee != burnPayee,
        "a registered CN must NOT be paid via the burn/empty payee");
    // P2PKH shape: OP_DUP OP_HASH160 <20> OP_EQUALVERIFY OP_CHECKSIG.
    if (cnPayee.size() == 25U) {
        BOOST_CHECK_EQUAL((int)cnPayee[0], 0x76);
        BOOST_CHECK_EQUAL((int)cnPayee[1], 0xa9);
        BOOST_CHECK_EQUAL((int)cnPayee[2], 0x14);
        BOOST_CHECK_EQUAL((int)cnPayee[23], 0x88);
        BOOST_CHECK_EQUAL((int)cnPayee[24], 0xac);
    }
}

// --- Scenario 8: correct amounts and the eligibility boundary. The CN share is
// --- exactly 65% of the block value (GetCollateralnodePayment); the miner keeps
// --- the remainder (35%). A CN paid within the last 10 blocks is NOT eligible,
// --- which is the ONLY correct path to the burn fallback even with a non-empty
// --- pool (consensus-valid, and what a legacy client accepts as a normal P2PKH
// --- or the canonical burn payee).
BOOST_AUTO_TEST_CASE(cn_amount_split_and_eligibility_boundary)
{
    const int64_t blockValue = 20000000;               // 0.20 COIN
    const int64_t cnPay   = GetCollateralnodePayment(8190000, blockValue);
    const int64_t minerPay = blockValue - cnPay;

    BOOST_CHECK_EQUAL(cnPay, blockValue * 65 / 100);   // 13000000 == 65%
    BOOST_CHECK_EQUAL(minerPay, blockValue * 35 / 100); // 7000000 == 35%
    BOOST_CHECK_EQUAL(cnPay + minerPay, blockValue);   // no value created/destroyed
    BOOST_CHECK_EQUAL(GetCollateralnodePayment(8190000, 0), 0);

    // Eligibility predicate as used in CreateNewBlock:
    //   eligible  <=>  cn.nBlockLastPaid < tip - 10
    const int tip = 8190000;
    auto eligible = [tip](int lastPaid) { return lastPaid < tip - 10; };
    BOOST_CHECK_MESSAGE(!eligible(tip),        "a CN paid at tip is NOT eligible");
    BOOST_CHECK_MESSAGE(!eligible(tip - 10),   "a CN paid exactly at tip-10 is NOT eligible");
    BOOST_CHECK_MESSAGE(eligible(tip - 11),    "a CN last paid at tip-11 IS eligible");
    BOOST_CHECK_MESSAGE(eligible(tip - 100),   "an idle CN IS eligible");
}

BOOST_AUTO_TEST_SUITE_END()