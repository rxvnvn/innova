#include <boost/test/unit_test.hpp>

#include "../blockindex_startup_authority.h"
#include "../blockindex_accessor.h"
#include "../main.h"

#include <map>
#include <type_traits>
#include <vector>

namespace {

static_assert(std::is_copy_constructible<BlockIndexStartupRecord>::value,
              "startup records must be durable by-value results");
static_assert(std::is_same<decltype(BlockIndexStartupRecord().logicalId),
                           BlockIndexLogicalId>::value,
              "startup identity must be the stable logical hash type");
static_assert(sizeof(LegacyBlockIndexStartupAuthority) <= sizeof(void*),
              "legacy startup authority must not own an all-history mirror");

struct StartupAuthorityFixture
{
    std::map<uint256, CBlockIndex*> savedMap;
    CBlockIndex* savedBest;
    CBlockIndex* savedGenesis;
    uint256 savedHashBest;
    uint256 savedBestTrust;
    int savedBestHeight;
    std::vector<CBlockIndex*> owned;

    uint256 hGenesis;
    uint256 hOne;
    uint256 hTwo;
    uint256 hTip;
    uint256 hSide;

    StartupAuthorityFixture()
        : savedBest(pindexBest), savedGenesis(pindexGenesisBlock),
          savedHashBest(hashBestChain), savedBestTrust(nBestChainTrust),
          savedBestHeight(nBestHeight),
          hGenesis(uint256(1001)), hOne(uint256(1002)), hTwo(uint256(1003)),
          hTip(uint256(1004)), hSide(uint256(2003))
    {
        LOCK(cs_main);
        savedMap.swap(mapBlockIndex);
        pindexBest = NULL;
        pindexGenesisBlock = NULL;
        hashBestChain = 0;
        nBestChainTrust = 0;
        nBestHeight = -1;
        ClearFindBlockByHeightCache();
        ClearBlockIndexAccessorState();

        CBlockIndex* genesis = Add(hGenesis, 0, NULL, 10, 0, 0, 0);
        CBlockIndex* one = Add(hOne, 1, genesis, 20, 111, 1000, 501);
        CBlockIndex* two = Add(hTwo, 2, one, 30, 222, 1000, 502);
        CBlockIndex* tip = Add(hTip, 3, two, 40, 333, 1200, 503);
        CBlockIndex* side = Add(hSide, 2, one, 25, 444, 1000, 504);
        (void)side;

        genesis->pnext = one;
        one->pnext = two;
        two->pnext = tip;
        tip->pnext = NULL;

        pindexGenesisBlock = genesis;
        pindexBest = tip;
        hashBestChain = hTip;
        nBestHeight = 3;
        nBestChainTrust = tip->nChainTrust;
    }

    ~StartupAuthorityFixture()
    {
        LOCK(cs_main);
        for (size_t i = 0; i < owned.size(); ++i)
            delete owned[i];
        mapBlockIndex.clear();
        savedMap.swap(mapBlockIndex);
        pindexBest = savedBest;
        pindexGenesisBlock = savedGenesis;
        hashBestChain = savedHashBest;
        nBestChainTrust = savedBestTrust;
        nBestHeight = savedBestHeight;
        ClearFindBlockByHeightCache();
        ClearBlockIndexAccessorState();
    }

    CBlockIndex* Add(const uint256& hash, int height, CBlockIndex* parent,
                     uint64_t trust, unsigned int checksum,
                     int64_t modifierTime, unsigned int blockSize)
    {
        CBlockIndex* p = new CBlockIndex();
        p->pprev = parent;
        p->nHeight = height;
        p->nChainTrust = uint256(trust);
        p->nStakeModifierChecksum = checksum;
        p->nStakeModifierTime = modifierTime;
        p->nSize = blockSize;
        p->nTime = 100000 + height;
        p->nBits = 0x1e0fffff;
        std::map<uint256, CBlockIndex*>::iterator inserted =
            mapBlockIndex.insert(std::make_pair(hash, p)).first;
        p->phashBlock = &inserted->first;
        owned.push_back(p);
        return p;
    }
};

static void CheckRecordMatches(const BlockIndexStartupRecord& record,
                               const CBlockIndex* pindex)
{
    BOOST_REQUIRE(pindex != NULL);
    BOOST_CHECK(record.logicalId == BlockIndexLogicalId(pindex->GetBlockHash()));
    BOOST_CHECK_EQUAL(record.height, pindex->nHeight);
    BOOST_CHECK_EQUAL(record.active, pindex->IsInMainChain());
    BOOST_CHECK_EQUAL(record.hasParent, pindex->pprev != NULL);
    BOOST_CHECK(record.parentLogicalId == BlockIndexLogicalId(
        pindex->pprev ? pindex->pprev->GetBlockHash() : uint256(0)));
    BOOST_CHECK_EQUAL(record.derived.hasChainTrust, true);
    BOOST_CHECK(record.derived.chainTrust == pindex->nChainTrust);
    BOOST_CHECK_EQUAL(record.derived.hasStakeModifierChecksum, true);
    BOOST_CHECK_EQUAL(record.derived.stakeModifierChecksum,
                      pindex->nStakeModifierChecksum);
    BOOST_CHECK_EQUAL(record.derived.hasStakeModifierTime,
                      pindex->nStakeModifierTime != 0);
    BOOST_CHECK_EQUAL(record.derived.stakeModifierTime,
                      pindex->nStakeModifierTime);
    BOOST_CHECK_EQUAL(record.derived.hasBlockSize, pindex->nSize != 0);
    BOOST_CHECK_EQUAL(record.derived.blockSize, pindex->nSize);
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(blockindex_startup_authority_tests, StartupAuthorityFixture)

BOOST_AUTO_TEST_CASE(tip_and_active_projection_match_legacy_values)
{
    LOCK(cs_main);
    LegacyBlockIndexStartupAuthority authority;

    const BlockIndexStartupAuthorityIdentity identity = authority.Identity();
    BOOST_CHECK_EQUAL(identity.kind, BLOCK_INDEX_STARTUP_AUTHORITY_LEGACY);
    BOOST_CHECK_EQUAL(identity.generationQualified, false);
    BOOST_CHECK_EQUAL(identity.generation, 0ULL);

    BlockIndexStartupResult tip = authority.GetTip();
    BOOST_REQUIRE_EQUAL(tip.status, BLOCK_INDEX_STARTUP_OK);
    CheckRecordMatches(tip.record, pindexBest);
    BOOST_CHECK(tip.record.logicalId == BlockIndexLogicalId(hashBestChain));
    BOOST_CHECK(tip.record.derived.chainTrust == nBestChainTrust);

    const int heights[] = {0, 2, 3};
    for (size_t i = 0; i < sizeof(heights) / sizeof(heights[0]); ++i)
    {
        BlockIndexStartupResult active = authority.GetActiveByHeight(heights[i]);
        BOOST_REQUIRE_EQUAL(active.status, BLOCK_INDEX_STARTUP_OK);
        CheckRecordMatches(active.record, FindBlockByHeight(heights[i]));
    }
}

BOOST_AUTO_TEST_CASE(logical_identity_survives_resident_pointer_replacement)
{
    LOCK(cs_main);
    LegacyBlockIndexStartupAuthority authority;
    BlockIndexStartupResult before = authority.LookupByHash(BlockIndexLogicalId(hSide));
    BOOST_REQUIRE_EQUAL(before.status, BLOCK_INDEX_STARTUP_OK);

    CBlockIndex* oldSide = mapBlockIndex[hSide];
    CBlockIndex* replacement = new CBlockIndex(*oldSide);
    replacement->phashBlock = &mapBlockIndex.find(hSide)->first;
    mapBlockIndex[hSide] = replacement;
    for (size_t i = 0; i < owned.size(); ++i)
        if (owned[i] == oldSide)
            owned[i] = replacement;
    delete oldSide;

    BlockIndexStartupResult after = authority.LookupByHash(BlockIndexLogicalId(hSide));
    BOOST_REQUIRE_EQUAL(after.status, BLOCK_INDEX_STARTUP_OK);
    BOOST_CHECK(before.record.logicalId == after.record.logicalId);
    BOOST_CHECK_EQUAL(before.record.height, after.record.height);
    BOOST_CHECK(before.record.derived.chainTrust == after.record.derived.chainTrust);
}

BOOST_AUTO_TEST_CASE(parent_successor_and_nonactive_results_are_typed)
{
    LOCK(cs_main);
    LegacyBlockIndexStartupAuthority authority;

    BlockIndexStartupResult parent = authority.GetParent(BlockIndexLogicalId(hTwo));
    BOOST_REQUIRE_EQUAL(parent.status, BLOCK_INDEX_STARTUP_OK);
    BOOST_CHECK(parent.record.logicalId == BlockIndexLogicalId(hOne));

    BlockIndexStartupResult successor = authority.GetNextActive(BlockIndexLogicalId(hTwo));
    BOOST_REQUIRE_EQUAL(successor.status, BLOCK_INDEX_STARTUP_OK);
    BOOST_CHECK(successor.record.logicalId == BlockIndexLogicalId(hTip));

    BlockIndexStartupResult sideActive = authority.LookupActiveByHash(BlockIndexLogicalId(hSide));
    BOOST_CHECK_EQUAL(sideActive.status, BLOCK_INDEX_STARTUP_NOT_ACTIVE);
    BOOST_CHECK(!sideActive.HasRecord());

    BlockIndexStartupResult missing = authority.LookupByHash(BlockIndexLogicalId(uint256(9999)));
    BOOST_CHECK_EQUAL(missing.status, BLOCK_INDEX_STARTUP_NOT_FOUND);
    BOOST_CHECK(!missing.HasRecord());

    BlockIndexStartupResult genesisParent = authority.GetParent(BlockIndexLogicalId(hGenesis));
    BOOST_CHECK_EQUAL(genesisParent.status, BLOCK_INDEX_STARTUP_NOT_FOUND);
    BOOST_CHECK(!genesisParent.HasRecord());
}

BOOST_AUTO_TEST_CASE(unavailable_derived_state_is_explicit_not_zero_as_missing)
{
    LOCK(cs_main);
    LegacyBlockIndexStartupAuthority authority;

    BlockIndexStartupResult genesis = authority.LookupByHash(BlockIndexLogicalId(hGenesis));
    BOOST_REQUIRE_EQUAL(genesis.status, BLOCK_INDEX_STARTUP_OK);
    BOOST_CHECK_EQUAL(genesis.record.derived.hasChainTrust, true);
    BOOST_CHECK_EQUAL(genesis.record.derived.hasStakeModifierChecksum, true);
    BOOST_CHECK_EQUAL(genesis.record.derived.stakeModifierChecksum, 0U);
    BOOST_CHECK_EQUAL(genesis.record.derived.hasStakeModifierTime, false);
    BOOST_CHECK_EQUAL(genesis.record.derived.hasBlockSize, false);

    BlockIndexStartupResult required = authority.RequireDerivedState(
        BlockIndexLogicalId(hGenesis),
        BLOCK_INDEX_STARTUP_REQUIRE_STAKE_MODIFIER_TIME |
        BLOCK_INDEX_STARTUP_REQUIRE_BLOCK_SIZE);
    BOOST_CHECK_EQUAL(required.status,
                      BLOCK_INDEX_STARTUP_UNAVAILABLE_DERIVED_STATE);
    BOOST_CHECK(!required.HasRecord());

    BlockIndexStartupResult complete = authority.RequireDerivedState(
        BlockIndexLogicalId(hOne), BLOCK_INDEX_STARTUP_REQUIRE_ALL);
    BOOST_REQUIRE_EQUAL(complete.status, BLOCK_INDEX_STARTUP_OK);
    BOOST_CHECK(complete.HasRecord());
}

BOOST_AUTO_TEST_CASE(active_chain_differential_has_zero_mismatches)
{
    LOCK(cs_main);
    LegacyBlockIndexStartupAuthority authority;
    unsigned int mismatches = 0;
    for (int height = 0; height <= nBestHeight; ++height)
    {
        const CBlockIndex* legacy = FindBlockByHeight(height);
        BlockIndexStartupResult projected = authority.GetActiveByHeight(height);
        if (projected.status != BLOCK_INDEX_STARTUP_OK || legacy == NULL ||
            projected.record.logicalId != BlockIndexLogicalId(legacy->GetBlockHash()) ||
            projected.record.height != legacy->nHeight ||
            projected.record.derived.chainTrust != legacy->nChainTrust)
            ++mismatches;
    }
    BOOST_CHECK_EQUAL(mismatches, 0U);
}

BOOST_AUTO_TEST_SUITE_END()
