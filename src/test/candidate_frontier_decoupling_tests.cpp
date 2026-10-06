// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

// PM1-P0-07b (C3) - candidate frontier DECOUPLING proof suite.
//
// Proves that the by-value candidate frontier, when sourced from the persisted
// immutable static leaf frontier (candidate-leaves.dat via
// BlockIndexCandidateStartupBuilder + ReadCandidateLeafMetadata), reproduces the
// established legacy selection semantics on the same topology, with NO
// historical global resident-index scan in the by-value core, and that missing
// data is NOT_FOUND while corruption/mismatch is a fail-closed ERROR, and that
// the result is stable across a restart rebuild.

#include <boost/test/unit_test.hpp>

#include "main.h"
#include "candidate_frontier.h"
#include "candidate_frontier_metadata.h"
#include "blockindex_candidate_startup_builder.h"
#include "blockindex_v2_reader.h"
#include "blockindex_derived_state.h"
#include "blockindex_generation_builder.h"
#include "blockindex_generation_lifecycle.h"

#include <boost/filesystem.hpp>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = boost::filesystem;

extern std::map<uint256, CBlockIndex*> mapBlockIndex;
extern CBlockIndex* pindexBest;
extern uint256 nBestChainTrust;
extern std::set<uint256> setInvalidBlockHash;
extern std::map<uint256, CandidateTipRecord> mapCandidateTips;
extern uint256 hashBestChain;
extern int nBestHeight;

BOOST_AUTO_TEST_SUITE(candidate_frontier_decoupling_tests)

// ---------------------------------------------------------------------------
// Fork topology:  active g->a1->a2 (best=a2) ; side g->s1->s2->s3 (winner)
// tips = {a2, s3} ; trust(s3) > trust(a2) because the side branch is longer.
// ---------------------------------------------------------------------------
struct C3Topology
{
    std::vector<uint256> hashes;   // id order
    std::vector<BlockIndexId> ids;
};

static fs::path C3TempDir(const char* tag)
{
    fs::path p = fs::temp_directory_path() / fs::unique_path(std::string(tag) + "-%%%%-%%%%");
    fs::create_directories(p);
    return p;
}

// Write a synthetic block (matching header fields) to blk00001.dat.
static uint256 C3WriteBlock(const fs::path& dir, uint256 prev, unsigned int nTime,
                            unsigned int nBits, unsigned int nNonce,
                            unsigned int* outPos)
{
    CTransaction coinbase; coinbase.nVersion = 1; coinbase.nTime = nTime;
    CTxIn input; input.prevout = COutPoint(uint256(0), 0xffffffff);
    input.scriptSig = CScript() << OP_TRUE; input.nSequence = 0xffffffff;
    coinbase.vin.push_back(input);
    CTxOut output; output.nValue = 0; output.scriptPubKey = CScript() << OP_TRUE;
    coinbase.vout.push_back(output);
    CBlock block; block.nVersion = 1; block.hashPrevBlock = prev;
    block.nTime = nTime; block.nBits = nBits; block.nNonce = nNonce;
    block.vtx.push_back(coinbase); block.hashMerkleRoot = block.BuildMerkleTree();
    uint256 h = block.GetHash();
    CDataStream ss(SER_DISK, CLIENT_VERSION); ss << block;
    char name[32]; snprintf(name, sizeof(name), "blk%04u.dat", 1u);
    FILE* fp = fopen((dir / name).string().c_str(), "ab"); BOOST_REQUIRE(fp != NULL);
    unsigned char magic[] = {0xfa, 0xbf, 0xb5, 0xda};
    fwrite(magic, 1, 4, fp);
    unsigned int ns = ss.size(); fwrite(&ns, 4, 1, fp);
    long pos = ftell(fp); *outPos = (unsigned int)pos;
    fwrite(&ss[0], 1, ss.size(), fp); fflush(fp); fclose(fp);
    return h;
}

// Build an authoritative V2 generation holding the fork topology.
static bool C3BuildGeneration(const fs::path& datadir, const fs::path& v2root,
                              int nActive, C3Topology* topo, std::string* error)
{
    struct Pending { uint256 hash; uint256 prev; int height; unsigned int nTime, nBits, nNonce, nPos; };
    std::vector<Pending> pend;

    unsigned int pos = 0;
    // active chain: g(0) a1(1) .. a{nActive}(active height)
    uint256 prev(0);
    for (int h = 0; h <= nActive; ++h)
    {
        unsigned int p = 0;
        uint256 hsh = C3WriteBlock(datadir, prev, 1700000000u + (unsigned)h, 0x1d00ffffU, (unsigned)h, &p);
        Pending x; x.hash = hsh; x.prev = prev; x.height = h; x.nTime = 1700000000u + h;
        x.nBits = 0x1d00ffffU; x.nNonce = (unsigned)h; x.nPos = p;
        pend.push_back(x);
        prev = hsh;
    }
    uint256 activeTip = prev;
    // side branch g -> s1(1) -> s2(2) -> s3(3)  (longer than active when nActive=2)
    prev = pend[0].hash; // genesis
    for (int h = 1; h <= 3; ++h)
    {
        unsigned int p = 0;
        uint256 hsh = C3WriteBlock(datadir, prev, 1700000500u + (unsigned)h, 0x1d00ffffU,
                                   1000u + (unsigned)h, &p);
        Pending x; x.hash = hsh; x.prev = prev; x.height = h; x.nTime = 1700000500u + h;
        x.nBits = 0x1d00ffffU; x.nNonce = 1000u + h; x.nPos = p;
        pend.push_back(x);
        prev = hsh;
    }
    uint256 sideTip = prev;

    BlockIndexGenerationSource src;
    for (size_t i = 0; i < pend.size(); ++i)
    {
        BlockIndexRecord rec;
        rec.hash = pend[i].hash; rec.hashPrev = pend[i].prev; rec.height = pend[i].height;
        rec.nVersion = 1; rec.nTime = pend[i].nTime; rec.nBits = pend[i].nBits;
        rec.nNonce = pend[i].nNonce; rec.nFile = 1; rec.nBlockPos = pend[i].nPos;
        rec.nFlags = 0;
        BlockIndexGenerationSourceRecord sr; sr.hash = pend[i].hash; sr.record = rec;
        src.records.push_back(sr);
    }
    src.hashBestChain = activeTip; src.foundBestChain = true;
    src.blockDataDir = datadir.string();

    BlockIndexGenerationBuilder b;
    if (!b.Build(src, BlockIndexGenerationManager::StagingPath(v2root.string(), 1), 1, NULL, error))
    {
        b.Close();
        return false;
    }
    b.Close();
    if (BlockIndexGenerationManager::PublishGeneration(v2root.string(), 1, error) != BLOCK_INDEX_LIFECYCLE_OK)
        return false;
    if (BlockIndexGenerationManager::SelectGeneration(v2root.string(), 1, error) != BLOCK_INDEX_LIFECYCLE_OK)
        return false;

    if (topo)
    {
        for (size_t i = 0; i < pend.size(); ++i) topo->hashes.push_back(pend[i].hash);
    }
    (void)sideTip;
    return true;
}

static bool C3OpenStores(const fs::path& v2root, BlockIndexV2Reader* reader,
                         BlockIndexDerivedStateStore* derived, std::string* error)
{
    if (!reader->Open(v2root.string(), BlockIndexV2ReaderOptions(), error))
        return false;
    if (!BlockIndexDerivedStateStore::OpenReadOnly(
            BlockIndexGenerationManager::GenerationPath(v2root.string(), reader->Generation()),
            reader->Generation(), derived, error))
        return false;
    return true;
}

// Fixture: saves/restores every global the legacy oracle touches.
struct C3Fixture
{
    std::map<uint256, CBlockIndex*> savedMap;
    CBlockIndex* savedBest; uint256 savedTrust; uint256 savedBestChain; int savedHeight;
    std::vector<CBlockIndex*> mine;

    C3Fixture()
        : savedBest(NULL), savedTrust(0), savedBestChain(0), savedHeight(-1)
    {
        savedMap.swap(mapBlockIndex);
        savedBest = pindexBest; savedTrust = nBestChainTrust;
        savedBestChain = hashBestChain; savedHeight = nBestHeight;
        pindexBest = NULL; nBestChainTrust = 0; hashBestChain = 0; nBestHeight = -1;
        setInvalidBlockHash.clear(); mapCandidateTips.clear();
    }
    ~C3Fixture()
    {
        for (size_t i = 0; i < mine.size(); ++i) { delete mine[i]->phashBlock; delete mine[i]; }
        mine.clear();
        mapBlockIndex.clear(); mapCandidateTips.clear(); setInvalidBlockHash.clear();
        savedMap.swap(mapBlockIndex);
        pindexBest = savedBest; nBestChainTrust = savedTrust;
        hashBestChain = savedBestChain; nBestHeight = savedHeight;
    }
};

// Build the legacy resident mirror of the generation (real CBlockIndex graph)
// so the REAL legacy full-scan oracle (RebuildCandidateTips +
// EvaluateCandidateFrontier) runs over identical data.
static void C3BuildLegacyMirror(const BlockIndexV2Reader& reader,
                                const BlockIndexDerivedStateStore& derived,
                                C3Fixture* fx, uint256* bestHash, uint256* bestTrust)
{
    std::map<uint256, CBlockIndex*> byHash;
    const uint64_t count = reader.RecordCount();
    for (BlockIndexId id = 1; id <= count; ++id)
    {
        BlockIndexSnapshot snap;
        std::string err;
        BOOST_REQUIRE(reader.GetRecordById(id, &snap, &err) == BLOCK_INDEX_V2_READ_FOUND);
        BlockIndexDerivedEntry de;
        BOOST_REQUIRE(derived.Read(id, &de, &err) == BLOCK_INDEX_DERIVED_LOOKUP_FOUND);
        CBlockIndex* p = new CBlockIndex();
        p->phashBlock = new uint256(snap.hash);
        p->nHeight = snap.height;
        p->nChainTrust = de.chainTrust;
        p->nFlags = 0;
        p->nFile = snap.nFile;
        p->nBlockPos = snap.nBlockPos;
        byHash[snap.hash] = p;
        fx->mine.push_back(p);
        mapBlockIndex[snap.hash] = p;
    }
    // Link the resident pprev graph by PARENT HASH (exact legacy semantics).
    for (BlockIndexId id = 1; id <= count; ++id)
    {
        BlockIndexSnapshot snap;
        std::string err;
        BOOST_REQUIRE(reader.GetRecordById(id, &snap, &err) == BLOCK_INDEX_V2_READ_FOUND);
        if (snap.hashPrev != uint256(0))
            byHash[snap.hash]->pprev = byHash[snap.hashPrev];
    }
    const BlockIndexSnapshot tip = reader.GetTip();
    BOOST_REQUIRE(mapBlockIndex.count(tip.hash) == 1);
    pindexBest = mapBlockIndex[tip.hash];
    hashBestChain = tip.hash;
    nBestChainTrust = pindexBest->nChainTrust;
    nBestHeight = pindexBest->nHeight;
    if (bestHash) *bestHash = tip.hash;
    if (bestTrust) *bestTrust = pindexBest->nChainTrust;
}

// =====================================================================
// 1. candidate-leaves.dat == independently computed tip set
// =====================================================================
BOOST_AUTO_TEST_CASE(c3_leaves_equal_independent_tip_set)
{
    const fs::path datadir = C3TempDir("c3-leaves");
    const fs::path v2root = datadir / "blockindex-v2";
    std::string err;
    BOOST_REQUIRE_MESSAGE(C3BuildGeneration(datadir, v2root, 2, NULL, &err), err);

    BlockIndexV2Reader reader; BlockIndexDerivedStateStore derived;
    BOOST_REQUIRE_MESSAGE(C3OpenStores(v2root, &reader, &derived, &err), err);

    std::vector<uint256> leaves;
    BOOST_REQUIRE_MESSAGE(ReadCandidateLeafMetadata(reader.GenerationPath(), reader.Generation(),
                                                    &leaves, &err), err);

    // independent tip set: every record hash that is not any record's hashPrev
    std::set<uint256> referenced, all;
    for (BlockIndexId id = 1; id <= reader.RecordCount(); ++id)
    {
        BlockIndexSnapshot s; std::string e;
        BOOST_REQUIRE(reader.GetRecordById(id, &s, &e) == BLOCK_INDEX_V2_READ_FOUND);
        all.insert(s.hash);
        if (s.hashPrev != uint256(0)) referenced.insert(s.hashPrev);
    }
    std::set<uint256> expect;
    for (std::set<uint256>::iterator it = all.begin(); it != all.end(); ++it)
        if (!referenced.count(*it)) expect.insert(*it);

    BOOST_REQUIRE_EQUAL(leaves.size(), expect.size());
    for (size_t i = 0; i < leaves.size(); ++i)
        BOOST_REQUIRE(expect.count(leaves[i]) == 1);

    reader.Close();
    fs::remove_all(datadir);
}

// =====================================================================
// 2. by-value (candidate-leaves) selection == legacy full-scan oracle
// =====================================================================
BOOST_AUTO_TEST_CASE(c3_byvalue_parity_vs_legacy_oracle)
{
    const fs::path datadir = C3TempDir("c3-parity");
    const fs::path v2root = datadir / "blockindex-v2";
    std::string err;
    BOOST_REQUIRE_MESSAGE(C3BuildGeneration(datadir, v2root, 2, NULL, &err), err);

    BlockIndexV2Reader reader; BlockIndexDerivedStateStore derived;
    BOOST_REQUIRE_MESSAGE(C3OpenStores(v2root, &reader, &derived, &err), err);

    SnapshotCandidateFrontierStore store;
    BlockIndexCandidateStartupBuilder cb;
    BOOST_REQUIRE_MESSAGE(cb.Build(reader, derived, &store, &err), err);

    // by-value winner from the candidate-leaves-sourced store
    const CandidateFrontierAuthorityRecord selByValue = EvaluateCandidateFrontierByValue(store);
    BOOST_REQUIRE(selByValue.found);

    // legacy full-scan oracle over an identical resident mirror
    C3Fixture fx;
    C3BuildLegacyMirror(reader, derived, &fx, NULL, NULL);
    RebuildCandidateTips();
    for (std::map<uint256, CandidateTipRecord>::iterator it = mapCandidateTips.begin();
         it != mapCandidateTips.end(); ++it)
        it->second.fHasData = store.HasBlockData(it->first) ? 1 : 0;
    CBlockIndex* legacy = EvaluateCandidateFrontier();
    BOOST_REQUIRE(legacy != NULL);

    // SEMANTIC PARITY: identical winner hash and identical trust.
    BOOST_REQUIRE_MESSAGE(selByValue.hash == *legacy->phashBlock, "by-value winner != legacy oracle winner");
    BOOST_REQUIRE(selByValue.chainTrust == legacy->nChainTrust);
    BOOST_REQUIRE_EQUAL(selByValue.height, legacy->nHeight);

    // and the winner is the longer side tip, not the active best tip.
    BOOST_REQUIRE(selByValue.hash != reader.GetTip().hash);

    reader.Close();
    fs::remove_all(datadir);
}

// =====================================================================
// 3. NOT_FOUND vs ERROR (fail-closed)
// =====================================================================
BOOST_AUTO_TEST_CASE(c3_notfound_vs_error_fail_closed)
{
    const fs::path datadir = C3TempDir("c3-error");
    const fs::path v2root = datadir / "blockindex-v2";
    std::string err;
    BOOST_REQUIRE_MESSAGE(C3BuildGeneration(datadir, v2root, 2, NULL, &err), err);

    BlockIndexV2Reader reader; BlockIndexDerivedStateStore derived;
    BOOST_REQUIRE_MESSAGE(C3OpenStores(v2root, &reader, &derived, &err), err);
    SnapshotCandidateFrontierStore store;
    BlockIndexCandidateStartupBuilder cb;
    BOOST_REQUIRE_MESSAGE(cb.Build(reader, derived, &store, &err), err);

    // (a) NOT_FOUND: an unknown hash is a clean found=false, never an error.
    CandidateFrontierAuthorityRecord missing = store.Lookup(uint256(0xdeadbeefULL));
    BOOST_REQUIRE(!missing.found);
    BOOST_REQUIRE(!missing.isEligible);
    const CandidateFrontierAuthorityRecord parentMissing = store.GetParent(uint256(0xdeadbeefULL));
    BOOST_REQUIRE(!parentMissing.found);

    // (b) leaves-file generation mismatch is an ERROR (not silently consumed).
    std::vector<uint256> leaves;
    BOOST_REQUIRE(!ReadCandidateLeafMetadata(reader.GenerationPath(),
                                             reader.Generation() + 7, &leaves, &err));
    reader.Close();

    // (c) corrupt records.dat -> reader open or builder FAILS CLOSED (ERROR).
    const std::string recordsPath =
        boost::filesystem::path(BlockIndexGenerationManager::GenerationPath(v2root.string(), 1))
            .string() + "/records.dat";
    {
        FILE* f = fopen(recordsPath.c_str(), "r+b");
        BOOST_REQUIRE(f != NULL);
        fseek(f, 0, SEEK_END);
        long fsz = ftell(f);
        BOOST_REQUIRE(fsz > 64);
        fseek(f, fsz / 2, SEEK_SET);   // inside the committed record region
        unsigned char junk[8] = {0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE};
        fwrite(junk, 1, sizeof(junk), f);
        fflush(f); fclose(f);
    }
    {
        bool failClosed = false;
        std::string e2;
        BlockIndexV2Reader r2;
        if (!r2.Open(v2root.string(), BlockIndexV2ReaderOptions(), &e2))
        {
            failClosed = true; // reader refused the corrupt generation at open
        }
        else
        {
            BlockIndexDerivedStateStore d2;
            if (!BlockIndexDerivedStateStore::OpenReadOnly(
                    BlockIndexGenerationManager::GenerationPath(v2root.string(), r2.Generation()),
                    r2.Generation(), &d2, &e2))
            {
                failClosed = true;
            }
            else
            {
                SnapshotCandidateFrontierStore s2;
                BlockIndexCandidateStartupBuilder cb2;
                if (!cb2.Build(r2, d2, &s2, &e2))
                    failClosed = true; // builder refused -> no store produced
            }
            r2.Close();
        }
        BOOST_REQUIRE_MESSAGE(failClosed,
                              "corrupt records must fail closed (no candidate store produced)");
    }

    fs::remove_all(datadir);
}

// =====================================================================
// 4. restart: rebuild from the SAME generation yields the SAME winner
// =====================================================================
BOOST_AUTO_TEST_CASE(c3_restart_rebuild_same_winner)
{
    const fs::path datadir = C3TempDir("c3-restart");
    const fs::path v2root = datadir / "blockindex-v2";
    std::string err;
    BOOST_REQUIRE_MESSAGE(C3BuildGeneration(datadir, v2root, 2, NULL, &err), err);

    uint256 first;
    {
        BlockIndexV2Reader reader; BlockIndexDerivedStateStore derived;
        BOOST_REQUIRE_MESSAGE(C3OpenStores(v2root, &reader, &derived, &err), err);
        SnapshotCandidateFrontierStore store;
        BlockIndexCandidateStartupBuilder cb;
        BOOST_REQUIRE_MESSAGE(cb.Build(reader, derived, &store, &err), err);
        CandidateFrontierAuthorityRecord sel = EvaluateCandidateFrontierByValue(store);
        BOOST_REQUIRE(sel.found);
        first = sel.hash;
        reader.Close();
    }
    // fresh reader/derived/store == restart
    {
        BlockIndexV2Reader reader; BlockIndexDerivedStateStore derived;
        BOOST_REQUIRE_MESSAGE(C3OpenStores(v2root, &reader, &derived, &err), err);
        SnapshotCandidateFrontierStore store;
        BlockIndexCandidateStartupBuilder cb;
        BOOST_REQUIRE_MESSAGE(cb.Build(reader, derived, &store, &err), err);
        CandidateFrontierAuthorityRecord sel = EvaluateCandidateFrontierByValue(store);
        BOOST_REQUIRE(sel.found);
        BOOST_REQUIRE(sel.hash == first);
        reader.Close();
    }

    fs::remove_all(datadir);
}

BOOST_AUTO_TEST_SUITE_END()
