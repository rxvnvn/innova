// M8.3 real-execution verification: the deterministic ANON feed runs for REAL when
// cs::anon::g_anon is OPENED (not just wired-and-inert). Opens a scratch anonstore,
// connects synthetic anonymous blocks through s16_AnonConnectBlock (the exact production
// ConnectBlock entry point), and asserts the store is populated (anon outputs + spent key
// image) and reverts on disconnect. This is the missing evidence that the c7a623ed feed
// actually EXECUTES when the authoritative store is open.
#include "main.h"
#include "chainstate/anonstore/anonstore.h"
#include <boost/test/unit_test.hpp>
#include <boost/filesystem.hpp>
#include <cstdio>
#include <cstring>
#include <vector>
#include <array>
#include <limits>

namespace fs = boost::filesystem;

BOOST_AUTO_TEST_SUITE(anonfeed_cutover_tests)

namespace {
// deterministic 32-byte id
static std::array<unsigned char,32> id32(unsigned int a, unsigned int b){
    std::array<unsigned char,32> out{}; unsigned char* p = out.data();
    for(int k=0;k<4;k++){ p[k]=(unsigned char)((a>>(8*k))&0xff); p[4+k]=(unsigned char)((b>>(8*k))&0xff); }
    // fill remaining deterministically
    for(int k=8;k<32;k++) p[k]=(unsigned char)((a*31+b*7+k*3)&0xff);
    return out;
}
// deterministic 33-byte pubkey (one-time key the anon feed reads from the script)
static std::array<unsigned char,33> pkCoin(unsigned int tag){
    std::array<unsigned char,33> out{}; unsigned char* p = out.data();
    unsigned int a=0x5001, b=tag;
    for(int k=0;k<4;k++){ p[k]=(unsigned char)((a>>(8*k))&0xff); p[4+k]=(unsigned char)((b>>(8*k))&0xff); }
    for(int k=8;k<33;k++) p[k]=(unsigned char)((a*31+b*7+k*3)&0xff);
    return out;
}
// Build an ANON output CTxOut: scriptPubKey = OP_RETURN ANON_MARKER lenPk pkCoin [pad >=70].
CScript makeAnonOutScript(const std::array<unsigned char,33>& pk){
    CScript s;
    s << OP_RETURN;
    std::vector<unsigned char> body;
    body.push_back(0xb9);            // OP_ANON_MARKER
    body.push_back(33);              // lenPk
    body.insert(body.end(), pk.begin(), pk.end());
    body.push_back(1);               // lenR (dummy)
    body.push_back(0);               // R[0] placeholder
    while (body.size() < 69) body.push_back(0);   // pad so OP_RETURN+body >= MIN_ANON_OUT_SIZE(70)
    s.insert(s.end(), body.begin(), body.end());
    return s;
}
// Build an ANON input CTxIn: scriptSig = OP_RETURN ANON_MARKER [pkA 33B][pkB 33B][pad >=99]
// prevout.hash = key-image source; prevout.n = (ringSize<<16)|(low byte => keyimage[32]).
CTxIn makeAnonIn(const std::array<unsigned char,33>& pkA,
                 const std::array<unsigned char,33>& pkB, unsigned int ringSize){
    CScript s;
    s << OP_RETURN;
    std::vector<unsigned char> body;
    body.push_back(0xb9);
    body.insert(body.end(), pkA.begin(), pkA.end());
    body.insert(body.end(), pkB.begin(), pkB.end());
    while (body.size() < 98) body.push_back(0);   // pad so scriptSig.size()>=99 (IsAnonInput)
    s.insert(s.end(), body.begin(), body.end());
    uint256 kh; memcpy(kh.begin(), pkA.data(), 32);   // key-image source hash
    COutPoint prev(kh, (ringSize << 16) | 0x42);
    return CTxIn(prev, s, std::numeric_limits<unsigned int>::max());
}
static CTransaction makeTx(const std::vector<CTxIn>& ins, const std::vector<CTxOut>& outs){
    CTransaction tx;
    tx.vin = ins; tx.vout = outs; tx.nVersion = 1; tx.nLockTime = 0;
    return tx;
}
} // namespace

BOOST_AUTO_TEST_CASE(anon_feed_executes_with_opened_store)
{
    // scratch (space on /home/user; /tmp may be full)
    fs::path base("/home/user/data/anontest-cutover");
    fs::remove_all(base);
    fs::create_directories(base);
    cs::ChainError e;

    BOOST_REQUIRE(!cs::anon::g_anon.opened());
    BOOST_REQUIRE(cs::anon::g_anon.open(base.string(), &e));

    // ---- Block A: two anon outputs P (value 100) and Q (value 100) ----
    std::array<unsigned char,33> P = pkCoin(1), Q = pkCoin(2);
    std::vector<CTxOut> outsA;
    outsA.push_back(CTxOut(100, makeAnonOutScript(P)));
    outsA.push_back(CTxOut(100, makeAnonOutScript(Q)));
    CTransaction txA = makeTx(std::vector<CTxIn>(), outsA);
    CBlock blkA; blkA.vtx.push_back(txA);
    uint256 hashA = blkA.GetHash();
    BOOST_REQUIRE_MESSAGE(s16_AnonConnectBlock(blkA, 1, hashA), "feed connected block A");

    // THE KEY ASSERTION (real execution, not inert): g_anon is now populated.
    cs::anon::CAnonOutput aoP, aoQ; bool foundP=false, foundQ=false;
    BOOST_REQUIRE(cs::anon::g_anon.getAo(P.data(), &aoP, &foundP, &e));
    BOOST_REQUIRE(cs::anon::g_anon.getAo(Q.data(), &aoQ, &foundQ, &e));
    BOOST_REQUIRE(foundP && foundQ);
    BOOST_CHECK_EQUAL(aoP.nValue, 100);
    BOOST_CHECK_EQUAL(aoQ.nValue, 100);
    BOOST_CHECK_EQUAL(aoP.nBlockHeight, 1);
    BOOST_CHECK_EQUAL(cs::anon::g_anon.valueCount(), (size_t)2);

    // ---- Block B: anon input spending ring {P,Q}, key image KI ----
    CTxIn inB = makeAnonIn(P, Q, 2);
    // expected key image KI = prevout.hash[0..31] + (prevout.n & 0xFF)
    std::array<unsigned char,33> KI;
    memcpy(KI.data(), inB.prevout.hash.begin(), 32);
    KI[32] = (unsigned char)(inB.prevout.n & 0xFF);
    CTransaction txB = makeTx(std::vector<CTxIn>({inB}), std::vector<CTxOut>());
    CBlock blkB; blkB.vtx.push_back(txB);
    uint256 hashB = blkB.GetHash();
    BOOST_REQUIRE_MESSAGE(s16_AnonConnectBlock(blkB, 2, hashB), "feed connected block B");

    cs::anon::CKeyImageSpent ki; bool foundKi=false;
    BOOST_REQUIRE(cs::anon::g_anon.getKi(KI.data(), &ki, &foundKi, &e));
    BOOST_REQUIRE(foundKi);
    BOOST_CHECK_EQUAL(ki.nValue, 100);   // ring value resolved from committed P/Q
    BOOST_CHECK_EQUAL(ki.inputNo, 0u);
    BOOST_CHECK_EQUAL(cs::anon::g_anon.valueCount(), (size_t)3);

    // ---- revert block B (disconnect) ----
    cs::Hash bhB, parentA; memcpy(bhB.data(), hashB.begin(), 32);
    memcpy(parentA.data(), hashA.begin(), 32);
    BOOST_REQUIRE(cs::anon::g_anon.disconnect(bhB, parentA, &e));
    foundKi=false;
    BOOST_REQUIRE(cs::anon::g_anon.getKi(KI.data(), &ki, &foundKi, &e));
    BOOST_CHECK(!foundKi);                          // key image gone after disconnect
    cs::anon::CAnonOutput aoP2; bool f2=false;
    BOOST_REQUIRE(cs::anon::g_anon.getAo(P.data(), &aoP2, &f2, &e));
    BOOST_CHECK(f2 && aoP2.nValue==100);            // block-A outputs survive
    BOOST_CHECK_EQUAL(cs::anon::g_anon.valueCount(), (size_t)2);
    BOOST_REQUIRE(cs::anon::g_anon.tree().verifyAll(&e));

    cs::anon::g_anon.close();
    fs::remove_all(base);
}

BOOST_AUTO_TEST_SUITE_END()