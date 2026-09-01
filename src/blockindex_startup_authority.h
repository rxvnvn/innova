// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#ifndef INNOVA_BLOCKINDEX_STARTUP_AUTHORITY_H
#define INNOVA_BLOCKINDEX_STARTUP_AUTHORITY_H

#include "blockindex_navigation.h"

#include <stdint.h>

// A.10.1a: typed, by-value semantic boundary for startup block-index state.
//
// This contract does not establish production authority and does not own or
// materialize CBlockIndex objects. The legacy adapter remains authoritative and
// requires caller-held cs_main. Future V2 implementations must preserve these
// result semantics without exposing process-local BlockIndexId or raw pointers.

enum BlockIndexStartupStatus
{
    BLOCK_INDEX_STARTUP_OK = 0,
    BLOCK_INDEX_STARTUP_NOT_FOUND = 1,
    BLOCK_INDEX_STARTUP_NOT_ACTIVE = 2,
    BLOCK_INDEX_STARTUP_UNAVAILABLE_DERIVED_STATE = 3,
};

enum BlockIndexStartupAuthorityKind
{
    BLOCK_INDEX_STARTUP_AUTHORITY_INVALID = 0,
    BLOCK_INDEX_STARTUP_AUTHORITY_LEGACY = 1,
    BLOCK_INDEX_STARTUP_AUTHORITY_V2 = 2,
};

struct BlockIndexStartupAuthorityIdentity
{
    BlockIndexStartupAuthorityKind kind;
    bool generationQualified;
    uint64_t generation;

    BlockIndexStartupAuthorityIdentity()
        : kind(BLOCK_INDEX_STARTUP_AUTHORITY_INVALID),
          generationQualified(false), generation(0) {}
};

struct BlockIndexStartupDerivedState
{
    bool hasChainTrust;
    uint256 chainTrust;
    bool hasStakeModifierChecksum;
    unsigned int stakeModifierChecksum;
    bool hasStakeModifierTime;
    int64_t stakeModifierTime;
    bool hasBlockSize;
    unsigned int blockSize;

    BlockIndexStartupDerivedState()
        : hasChainTrust(false), chainTrust(0),
          hasStakeModifierChecksum(false), stakeModifierChecksum(0),
          hasStakeModifierTime(false), stakeModifierTime(0),
          hasBlockSize(false), blockSize(0) {}
};

struct BlockIndexStartupRecord
{
    BlockIndexLogicalId logicalId;
    int height;
    bool active;
    bool hasParent;
    BlockIndexLogicalId parentLogicalId;
    bool hasActiveSuccessor;
    BlockIndexLogicalId activeSuccessorLogicalId;
    BlockIndexStartupDerivedState derived;

    BlockIndexStartupRecord()
        : logicalId(), height(-1), active(false), hasParent(false),
          parentLogicalId(), hasActiveSuccessor(false),
          activeSuccessorLogicalId(), derived() {}
};

struct BlockIndexStartupResult
{
    BlockIndexStartupStatus status;
    BlockIndexStartupRecord record;

    BlockIndexStartupResult()
        : status(BLOCK_INDEX_STARTUP_NOT_FOUND), record() {}

    bool HasRecord() const { return status == BLOCK_INDEX_STARTUP_OK; }

    static BlockIndexStartupResult Failure(BlockIndexStartupStatus statusIn)
    {
        BlockIndexStartupResult out;
        out.status = statusIn;
        return out;
    }

    static BlockIndexStartupResult Success(const BlockIndexStartupRecord& recordIn)
    {
        BlockIndexStartupResult out;
        out.status = BLOCK_INDEX_STARTUP_OK;
        out.record = recordIn;
        return out;
    }
};

enum BlockIndexStartupDerivedRequirement
{
    BLOCK_INDEX_STARTUP_REQUIRE_CHAIN_TRUST = (1U << 0),
    BLOCK_INDEX_STARTUP_REQUIRE_STAKE_MODIFIER_CHECKSUM = (1U << 1),
    BLOCK_INDEX_STARTUP_REQUIRE_STAKE_MODIFIER_TIME = (1U << 2),
    BLOCK_INDEX_STARTUP_REQUIRE_BLOCK_SIZE = (1U << 3),
    BLOCK_INDEX_STARTUP_REQUIRE_ALL =
        BLOCK_INDEX_STARTUP_REQUIRE_CHAIN_TRUST |
        BLOCK_INDEX_STARTUP_REQUIRE_STAKE_MODIFIER_CHECKSUM |
        BLOCK_INDEX_STARTUP_REQUIRE_STAKE_MODIFIER_TIME |
        BLOCK_INDEX_STARTUP_REQUIRE_BLOCK_SIZE,
};

// Candidate authority and materialization are deliberately separate. A.10.1c
// will provide enumeration/evaluation; A.10.1a only fixes the by-value fields.
struct BlockIndexStartupCandidateAuthority
{
    BlockIndexLogicalId tip;
    bool hasChainTrust;
    uint256 chainTrust;
    bool hasForkPoint;
    BlockIndexLogicalId forkPoint;
    int forkHeight;
    bool validAncestors;

    BlockIndexStartupCandidateAuthority()
        : tip(), hasChainTrust(false), chainTrust(0), hasForkPoint(false),
          forkPoint(), forkHeight(-1), validAncestors(false) {}
};

struct BlockIndexStartupCandidateMaterialization
{
    BlockIndexLogicalId tip;
    bool hasBlockData;

    BlockIndexStartupCandidateMaterialization()
        : tip(), hasBlockData(false) {}
};

class BlockIndexStartupAuthority
{
public:
    virtual ~BlockIndexStartupAuthority() {}

    virtual BlockIndexStartupAuthorityIdentity Identity() const = 0;
    virtual BlockIndexStartupResult GetTip() const = 0;
    virtual BlockIndexStartupResult LookupByHash(const BlockIndexLogicalId& id) const = 0;
    virtual BlockIndexStartupResult LookupActiveByHash(const BlockIndexLogicalId& id) const = 0;
    virtual BlockIndexStartupResult GetActiveByHeight(int height) const = 0;
    virtual BlockIndexStartupResult GetParent(const BlockIndexLogicalId& child) const = 0;
    virtual BlockIndexStartupResult GetNextActive(const BlockIndexLogicalId& current) const = 0;
    virtual BlockIndexStartupResult RequireDerivedState(
        const BlockIndexLogicalId& id, unsigned int requirements) const = 0;
};

class LegacyBlockIndexStartupAuthority : public BlockIndexStartupAuthority
{
public:
    virtual BlockIndexStartupAuthorityIdentity Identity() const;
    virtual BlockIndexStartupResult GetTip() const;
    virtual BlockIndexStartupResult LookupByHash(const BlockIndexLogicalId& id) const;
    virtual BlockIndexStartupResult LookupActiveByHash(const BlockIndexLogicalId& id) const;
    virtual BlockIndexStartupResult GetActiveByHeight(int height) const;
    virtual BlockIndexStartupResult GetParent(const BlockIndexLogicalId& child) const;
    virtual BlockIndexStartupResult GetNextActive(const BlockIndexLogicalId& current) const;
    virtual BlockIndexStartupResult RequireDerivedState(
        const BlockIndexLogicalId& id, unsigned int requirements) const;
};

#endif // INNOVA_BLOCKINDEX_STARTUP_AUTHORITY_H
