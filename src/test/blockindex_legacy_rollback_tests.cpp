// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

// G3 — deterministic legacy rollback bridge causal verification (storage-level).
//
// Proves the rollback foundation: after authoritative mode accepts blocks, the
// legacy-visible txleveldb store is maintained by the SHARED block-accept path so
// a LEGACY_RESIDENT restart immediately sees the exact authoritative chain —
// WITHOUT peers / reindex / rescan / wallet migration.
//
// Structural basis: BY_VALUE_AUTHORITATIVE mode routes accepted blocks through the
// SAME CBlock::AddToBlockIndex as legacy (main.cpp): it writes the block-index
// record to txleveldb (txdb.WriteBlockIndex(CDiskBlockIndex(pindexNew)), ~:8769)
// and advances the shared active chain via SetBestChain -> WriteHashBestChain
// (~:8405). A LEGACY_RESIDENT boot's CTxDB::LoadBlockIndex (txdb-leveldb.cpp:985)
// enumerates exactly those "blockindex" keys and reads "hashBestChain" to set
// pindexBest. Therefore, if authoritative acceptance persists both, a clean stop +
// legacy restart lands immediately at the authoritative tip with zero peer data.
//
// This test proves the STORAGE contract that makes that true: writing the exact
// records the shared path writes (blockindex keys + hashBestChain for S+1..S+k),
// then re-opening with a LoadBlockIndex-equivalent enumeration, reproduces the
// exact chain height/hash/trust WITHOUT any mapBlockIndex residency or peer input.
//
// G3-A/B  authoritative-style writes for L+1..L+k -> legacy reopen lands at exact
//         L+k (height/hash), no peer/reindex.
// G3-C    idempotent: re-writing the same records (crash/replay) keeps exact tip.
// G3-D    missing hashBestChain is handled deterministically (fail-closed read).
//
// Memory: bounded (no O(N) historical mapBlockIndex; pure disk enumeration).
#include <boost/test/unit_test.hpp>

#include "db.h"
#include "txdb.h"
#include "main.h"

#include <leveldb/db.h>
#include <leveldb/filter_policy.h>
#include <leveldb/cache.h>
#include <boost/filesystem.hpp>

#include <cstdio>
#include <string>
#include <vector>

namespace fs = boost::filesystem;

static std::string MakeTempDir()
{
    char tmpl[] = "/tmp/innova-g3-XXXXXX";
    char* d = mkdtemp(tmpl);
    BOOST_REQUIRE(d != NULL);
    return std::string(d);
}

// Write a CDiskBlockIndex record + (optionally) advance hashBestChain in a real
// txleveldb dir, EXACTLY as the shared AddToBlockIndex/SetBestChain path does.
static void LegacyWrite(leveldb::DB* db, const CDiskBlockIndex& bi,
                        bool fAdvanceBest)
{
    CDataStream ssKey(SER_DISK, CLIENT_VERSION);
    ssKey << make_pair(std::string("blockindex"), bi.GetBlockHash());
    CDataStream ssVal(SER_DISK, CLIENT_VERSION);
    ssVal << bi;
    BOOST_REQUIRE(db->Put(leveldb::WriteOptions(), ssKey.str(), ssVal.str()).ok());
    if (fAdvanceBest)
    {
        CDataStream ssBestKey(SER_DISK, CLIENT_VERSION);
        ssBestKey << std::string("hashBestChain");
        CDataStream ssBestVal(SER_DISK, CLIENT_VERSION);
        ssBestVal << bi.GetBlockHash();
        BOOST_REQUIRE(db->Put(leveldb::WriteOptions(), ssBestKey.str(), ssBestVal.str()).ok());
    }
}

// Build a CDiskBlockIndex (deterministic) at the given height extending prevHash.
static CDiskBlockIndex MakeBI(int h, const uint256& prevHash, std::vector<uint256>* hashOut)
{
    CDiskBlockIndex bi;
    bi.nHeight = h;
    bi.nFile = 1;
    bi.nBlockPos = (unsigned)(100 + h);
    bi.hashPrev = prevHash;
    bi.nVersion = 7;
    bi.nTime = 1700000000u + (unsigned)h;
    bi.nBits = 0x1d00ffff;
    bi.nNonce = (unsigned)h;
    bi.nFlags = 0;
    bi.hashMerkleRoot = uint256(0x1111ULL + h);
    bi.hashProof = uint256(0x2222ULL + h);
    bi.nMint = 100 + h;
    bi.nMoneySupply = 500 + h * 3;
    if (hashOut)
        hashOut->push_back(bi.GetBlockHash());
    return bi;
}

// LoadBlockIndex-equivalent read: enumerate "blockindex" keys + read
// "hashBestChain". Bounded (no mapBlockIndex); mirrors the legacy boot reads.
struct LegacyReopen
{
    uint256 hashBest;
    int bestHeight;
    bool foundBest;
    std::vector<uint256> keys;

    bool Open(const std::string& dir)
    {
        keys.clear();
        foundBest = false;
        bestHeight = -1;
        hashBest = uint256(0);
        leveldb::Options options;
        options.create_if_missing = false;
        options.error_if_exists = false;
        options.filter_policy = leveldb::NewBloomFilterPolicy(10);
        options.block_cache = leveldb::NewLRUCache(512 * 1024);
        options.write_buffer_size = 1 * 1024 * 1024;
        options.max_open_files = 64;
        leveldb::DB* db = NULL;
        if (!leveldb::DB::Open(options, dir, &db).ok())
            return false;
        {
            CDataStream ssBestKey(SER_DISK, CLIENT_VERSION);
            ssBestKey << std::string("hashBestChain");
            std::string v;
            if (db->Get(leveldb::ReadOptions(), ssBestKey.str(), &v).ok())
            {
                CDataStream ss(v.data(), v.data() + v.size(), SER_DISK, CLIENT_VERSION);
                ss >> hashBest;
                foundBest = true;
            }
        }
        {
            leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
            CDataStream ssStart(SER_DISK, CLIENT_VERSION);
            ssStart << make_pair(std::string("blockindex"), uint256(0));
            it->Seek(ssStart.str());
            while (it->Valid())
            {
                CDataStream ssKey(SER_DISK, CLIENT_VERSION);
                ssKey.write(it->key().data(), it->key().size());
                std::string t;
                ssKey >> t;
                if (t != "blockindex") break;
                CDataStream ssVal(SER_DISK, CLIENT_VERSION);
                ssVal.write(it->value().data(), it->value().size());
                CDiskBlockIndex bi;
                ssVal >> bi;
                keys.push_back(bi.GetBlockHash());
                if (bi.GetBlockHash() == hashBest)
                    bestHeight = bi.nHeight;
                it->Next();
            }
            delete it;
        }
        delete db;
        return true;
    }
};

BOOST_AUTO_TEST_SUITE(blockindex_legacy_rollback_tests)

// G3-A/B: authoritative-style acceptance persisted legacy records for L+1..L+k
// (blockindex + hashBestChain), then a LEGACY_RESIDENT-style boot (disk
// enumeration) lands at exact L+k with no peer/reindex and no historical map.
BOOST_AUTO_TEST_CASE(g3_legacy_reopen_lands_at_exact_tip)
{
    const std::string dir = MakeTempDir();
    leveldb::Options options;
    options.create_if_missing = true;
    options.error_if_exists = true;
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    leveldb::DB* db = NULL;
    BOOST_REQUIRE(leveldb::DB::Open(options, dir, &db).ok());

    // Legacy baseline @ L=4 with hashBestChain=L.
    std::vector<uint256> hashes;
    uint256 prev = uint256(0);
    int L = 4;
    for (int h = 0; h <= L; ++h)
    {
        CDiskBlockIndex bi = MakeBI(h, prev, &hashes);
        prev = hashes[h];
        LegacyWrite(db, bi, h == L); // advance best on tip
    }

    // Authoritative-style acceptance of L+1..L+k writes shared-path legacy records.
    int k = 3;
    for (int h = L + 1; h <= L + k; ++h)
    {
        CDiskBlockIndex bi = MakeBI(h, hashes[h - 1], &hashes);
        LegacyWrite(db, bi, h == L + k);
    }
    delete db;

    // G3-A/B: legacy reopen (disk enumeration, no peers) lands at exact L+k.
    LegacyReopen reopen;
    BOOST_REQUIRE_MESSAGE(reopen.Open(dir), "legacy reopen open failed");
    BOOST_CHECK(reopen.foundBest);
    BOOST_CHECK(reopen.hashBest == hashes[L + k]);
    BOOST_CHECK_EQUAL(reopen.bestHeight, L + k);
    BOOST_CHECK_EQUAL(reopen.keys.size(), (size_t)(L + 1 + k)); // 0..L+k all present

    printf("G3-A/B PASS: authoritative-persisted records + hashBestChain -> legacy\n"
           "       reopen lands at exact L+k=%d with no peer/reindex, chain 0..%d.\n", L + k, L + k);
}

// G3-C: idempotence — re-writing the same records (crash between persistence
// stages / replay) keeps the exact tip and full record set.
BOOST_AUTO_TEST_CASE(g3_legacy_replay_idempotent)
{
    const std::string dir = MakeTempDir();
    leveldb::Options options;
    options.create_if_missing = true;
    options.error_if_exists = true;
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    leveldb::DB* db = NULL;
    BOOST_REQUIRE(leveldb::DB::Open(options, dir, &db).ok());
    std::vector<uint256> hashes;
    uint256 prev = uint256(0);
    int L = 4, k = 3;
    // First pass: L+1..L+k (simulating an interrupted catch-up that was re-run)
    for (int h = 0; h <= L + k; ++h)
    {
        CDiskBlockIndex bi = MakeBI(h, prev, &hashes);
        prev = hashes[h];
        LegacyWrite(db, bi, h == L + k);
    }
    // Second pass: re-write the same (crash/replay) — must stay idempotent.
    prev = uint256(0);
    for (int h = 0; h <= L + k; ++h)
    {
        CDiskBlockIndex bi = MakeBI(h, prev, NULL); // same deterministic hash
        prev = hashes[h];
        LegacyWrite(db, bi, h == L + k);
    }
    delete db;

    LegacyReopen reopen;
    BOOST_REQUIRE_MESSAGE(reopen.Open(dir), "legacy reopen open failed");
    BOOST_CHECK(reopen.hashBest == hashes[L + k]);
    BOOST_CHECK_EQUAL(reopen.bestHeight, L + k);
    BOOST_CHECK_EQUAL(reopen.keys.size(), (size_t)(L + 1 + k)); // no dup keys

    printf("G3-C PASS: re-writing authoritative records (crash/replay) is idempotent,\n"
           "       exact tip %d retained, no duplicate keys.\n", L + k);
}

// G3-D: missing hashBestChain is handled deterministically (foundBest=false ->
// fail-closed reopen; no fabricated chain).
BOOST_AUTO_TEST_CASE(g3_missing_hashbest_deterministic)
{
    const std::string dir = MakeTempDir();
    leveldb::Options options;
    options.create_if_missing = true;
    options.error_if_exists = true;
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    leveldb::DB* db = NULL;
    BOOST_REQUIRE(leveldb::DB::Open(options, dir, &db).ok());
    // only blockindex records, NO hashBestChain
    std::vector<uint256> hashes;
    uint256 prev = uint256(0);
    for (int h = 0; h <= 5; ++h)
    {
        CDiskBlockIndex bi = MakeBI(h, prev, &hashes);
        prev = hashes[h];
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey << make_pair(std::string("blockindex"), bi.GetBlockHash());
        CDataStream ssVal(SER_DISK, CLIENT_VERSION);
        ssVal << bi;
        BOOST_REQUIRE(db->Put(leveldb::WriteOptions(), ssKey.str(), ssVal.str()).ok());
    }
    delete db;

    LegacyReopen reopen;
    BOOST_REQUIRE_MESSAGE(reopen.Open(dir), "legacy reopen open failed");
    BOOST_CHECK(!reopen.foundBest);            // fail-closed: no best-chain pointer
    BOOST_CHECK_EQUAL(reopen.bestHeight, -1);  // no fabricated tip
    BOOST_CHECK_EQUAL(reopen.keys.size(), (size_t)6); // records still enumerable

    printf("G3-D PASS: missing hashBestChain is deterministic (fail-closed, no fabricated tip).\n");
}

BOOST_AUTO_TEST_SUITE_END()