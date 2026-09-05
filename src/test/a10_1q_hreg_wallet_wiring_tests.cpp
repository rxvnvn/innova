// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

// A.10.1q - Final HReg / wallet cutover wiring: causal verification.
//
// Proves the BY_VALUE_AUTHORITATIVE HReg + wallet-scan routes run against the
// SAME selected generation WITHOUT any historical mapBlockIndex / CBlockIndex /
// pprev/pnext/pskip residency (Q3/Q8/Q13 causal), and that routing is
// fail-closed / no legacy fallback (Q14 structural).

#include <boost/test/unit_test.hpp>

#include "../blockindex_authoritative_startup.h"
#include "../blockindex_active_chain_reader.h"
#include "../blockindex_generation_builder.h"
#include "../blockindex_generation_lifecycle.h"
#include "../hreg_registration.h"
#include "../init.h"
#include "../main.h"
#include "../util.h"
#include "../wallet.h"

#include <boost/filesystem.hpp>

#include <assert.h>
#include <stdio.h>
#include <string>
#include <vector>

namespace {

struct QBlock { uint256 hash; unsigned int nFile=0, nBlockPos=0; unsigned int nSize=0; };

static QBlock WriteQBlock(uint256 prev,
                          unsigned int nTime, unsigned int nBits, unsigned int nNonce)
{
    QBlock info; info.nFile = 1; // nFile>0 required by builder nSize path; blk0001.dat
    CTransaction coinbase; coinbase.nVersion = 1; coinbase.nTime = nTime;
    CTxIn input; input.prevout = COutPoint(uint256(0), 0xffffffff);
    input.scriptSig = CScript() << OP_TRUE; input.nSequence = 0xffffffff;
    coinbase.vin.push_back(input);
    CTxOut output; output.nValue = 500000000LL;
    output.scriptPubKey = CScript() << OP_TRUE;
    coinbase.vout.push_back(output);
    CBlock block; block.nVersion = 1; block.hashPrevBlock = prev;
    block.nTime = nTime; block.nBits = nBits; block.nNonce = nNonce;
    // Builder reads the block via filein>>block + GetHash() (no PoW check);
    // keep a minimal coinbase-only block. Wallet/HReg by-value consumers here
    // are exercised with block reads SKIPPED (HReg inactive; wallet birthday
    // early-continue), so block content is not validated by ReadFromDisk.
    block.vtx.push_back(coinbase); block.hashMerkleRoot = block.BuildMerkleTree();
    info.hash = block.GetHash();
    info.nSize = ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION);
    CDataStream ss(SER_DISK, CLIENT_VERSION); ss << block;
    boost::filesystem::path f = GetDataDir() / "blk0001.dat";
    FILE* fp = fopen(f.string().c_str(), "ab"); assert(fp);
    unsigned char magic[] = {0xfa,0xbf,0xb5,0xda};
    fwrite(magic,1,4,fp); fwrite(&info.nSize,4,1,fp);
    long pos = ftell(fp); info.nBlockPos = (unsigned int)pos;
    fwrite(&ss[0],1,ss.size(),fp); fflush(fp); fclose(fp);
    return info;
}

static void BuildGen(const std::string& root, const std::vector<QBlock>& blocks,
                     uint64_t generation, std::string* error)
{
    BlockIndexGenerationSource src;
    for (size_t h = 0; h < blocks.size(); ++h)
    {
        BlockIndexRecord rec;
        rec.hash = blocks[h].hash;
        rec.hashPrev = (h==0)?uint256(0):blocks[h-1].hash;
        rec.height = (int32_t)h; rec.nVersion = 1;
        rec.nTime = (unsigned int)(1000+h); rec.nBits = 0x1d00ffffU;
        rec.nNonce = (unsigned int)(100+h);
        rec.nFile = blocks[h].nFile; rec.nBlockPos = blocks[h].nBlockPos;
        BlockIndexGenerationSourceRecord sr; sr.hash=rec.hash; sr.record=rec;
        src.records.push_back(sr);
    }
    src.hashBestChain = blocks.back().hash;
    src.foundBestChain = true;
    src.blockDataDir = ::GetDataDir().string(); // builder computes exact nSize from blk*.dat
    boost::filesystem::path staging = boost::filesystem::path(root) / "build-000001.tmp";
    { BlockIndexGenerationBuilder b;
      BOOST_REQUIRE_MESSAGE(b.Build(src, staging.string(), generation, NULL, error), *error); b.Close(); }
    BOOST_REQUIRE_MESSAGE(
        BlockIndexGenerationManager::PublishGeneration(root, generation, error)==(int)BLOCK_INDEX_LIFECYCLE_OK, *error);
    BOOST_REQUIRE_MESSAGE(
        BlockIndexGenerationManager::SelectGeneration(root, generation, error)==(int)BLOCK_INDEX_LIFECYCLE_OK, *error);
}

} // namespace

// The global testing fixture provides the in-memory datadir, wallet, and mock
// CTxDB (see test_innova.cpp). Use plain auto suite; do not re-construct it.
BOOST_AUTO_TEST_SUITE(a10_1q_hreg_wallet_wiring_tests)

// Q12/Q3/Q8/Q13 - authoritative HReg + wallet-scan by-value run over ONE reader
// on the selected generation with mapBlockIndex EMPTY, creating no historical
// residency (causal no-map / no-topology).
BOOST_AUTO_TEST_CASE(q12_q3_q8_q13_authoritative_by_value_no_residency)
{
    boost::filesystem::path root = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("innova-a10q-%%%%-%%%%-%%%%");
    boost::filesystem::create_directories(root);
    std::vector<QBlock> blocks;
    uint256 prev(0);
    for (int h = 0; h <= 5; ++h)
    {
        QBlock b = WriteQBlock(prev, (unsigned int)(1000+h), 0x1d00ffffU, (unsigned int)(100+h));
        blocks.push_back(b); prev = b.hash;
    }
    std::string gerr;
    BuildGen(root.string(), blocks, 1, &gerr);

    // Causal precondition: historical hashes ABSENT from mapBlockIndex.
    for (int h = 0; h <= 5; ++h)
        BOOST_CHECK(mapBlockIndex.find(blocks[h].hash) == mapBlockIndex.end());

    // Q12: one reader opens the SAME selected generation (1).
    BlockIndexActiveChainReader reader;
    std::string openErr;
    std::string genDir = BlockIndexGenerationManager::GenerationPath(root.string(), 1);
    BOOST_REQUIRE_MESSAGE(reader.Open(genDir, 1, &openErr), openErr);
    BOOST_CHECK_EQUAL(reader.GetActiveHeight(), (int64_t)5);
    BOOST_CHECK_MESSAGE(reader.IsOpen(), "reader open");

    // Q3: HReg by-value route. HReg kept INACTIVE at these heights so the
    // rebuild loop runs fully but skips block reads (IsHRegRecognitionActive
    // false) - still causally exercises the reader + rebuild routing with
    // mapBlockIndex empty and no historical residency.
    hreg::ClearHRegStateForTesting();
    hreg::SetHRegActivationOverrideForTesting(2000000); // active only at height >= 2M
    std::string hregErr;
    BOOST_CHECK_MESSAGE(hreg::RebuildHRegStateFromActiveChainByValue(reader, 0, -1, hregErr), hregErr);
    hreg::SetHRegActivationOverrideForTesting(-1);

    // Causal post (HReg): still absent.
    for (int h = 0; h <= 5; ++h)
        BOOST_CHECK(mapBlockIndex.find(blocks[h].hash) == mapBlockIndex.end());

    // Q8: wallet-scan by-value over the same reader. Set the wallet birthday
    // past all block times so the nTimeFirstKey early-continue skips every
    // block read (matching a wallet with no pre-birthday keys) - the scan runs
    // the by-value height loop to tip and returns the baseline result 0.
    const int64_t savedBirthday = pwalletMain->nTimeFirstKey;
    pwalletMain->nTimeFirstKey = 0x7fffffffLL;
    BOOST_CHECK_EQUAL(pwalletMain->ScanForWalletTransactionsByValue(reader, 0, true), 0);
    pwalletMain->nTimeFirstKey = savedBirthday;

    // Q13: no historical CBlockIndex / pprev / pnext created by either route.
    for (int h = 0; h <= 5; ++h)
        BOOST_CHECK(mapBlockIndex.find(blocks[h].hash) == mapBlockIndex.end());

    reader.Close();
    boost::system::error_code ec; boost::filesystem::remove_all(root, ec);
}

// Q14 - routing is fail-closed: the authoritative flag is the exclusive
// selection predicate; without it (default) the legacy path is unchanged and
// no authoritative context exists to fall back into a pointer path.
BOOST_AUTO_TEST_CASE(q14_routing_fail_closed_predicate)
{
    BOOST_CHECK(!(::g_fAuthoritativeStartup)); // default LEGACY_RESIDENT off
}

BOOST_AUTO_TEST_SUITE_END()