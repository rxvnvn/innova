// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// PASSIVE EPOCH-STATE STORAGE RECORD.
// Rehomed from the retired src/dag.h (Legacy DAG retirement Phase 2 / H9CLOSURE).
// CEpochState is the current finality epoch-state storage record owned by
// CFinalityEpochStateStore (epoch-state persistence + curve-tree storage,
// load/reload, lookup). No engine, no globals, no runtime behaviour lives here.

#ifndef INN_EPOCH_STATE_H
#define INN_EPOCH_STATE_H

#include "uint256.h"
#include "serialize.h"

#include <vector>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Per-epoch finality state (persisted to LevelDB)
// ---------------------------------------------------------------------------

struct CEpochState
{
    int nEpoch;
    uint256 hashBoundaryBlock;
    int nHeightStart;
    int nHeightEnd;
    std::vector<uint256> vBlockHashes;   // DAG-ordered block hashes in this epoch
    uint256 hashCurveRoot;
    uint256 hashNullifierRoot;
    uint256 hashFinalityCertificate;
    uint256 nTotalTrust;
    int nBlockCount;
    int nTxCount;
    int nFinalityTier;
    int nConsecutiveHardCount;
    bool fFinalized;

    CEpochState()
    {
        nEpoch = 0;
        hashBoundaryBlock = 0;
        hashCurveRoot = 0;
        hashNullifierRoot = 0;
        hashFinalityCertificate = 0;
        nHeightStart = 0;
        nHeightEnd = 0;
        nTotalTrust = 0;
        nBlockCount = 0;
        nTxCount = 0;
        nFinalityTier = 0;
        nConsecutiveHardCount = 0;
        fFinalized = false;
    }

    IMPLEMENT_SERIALIZE
    (
        READWRITE(nEpoch);
        READWRITE(hashBoundaryBlock);
        READWRITE(nHeightStart);
        READWRITE(nHeightEnd);
        READWRITE(vBlockHashes);
        READWRITE(hashCurveRoot);
        READWRITE(hashNullifierRoot);
        READWRITE(hashFinalityCertificate);
        READWRITE(nTotalTrust);
        READWRITE(nBlockCount);
        READWRITE(nTxCount);
        READWRITE(nFinalityTier);
        READWRITE(nConsecutiveHardCount);
        READWRITE(fFinalized);
    )
};

#endif // INN_EPOCH_STATE_H
