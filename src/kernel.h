// Copyright (c) 2012-2013 The Peercoin developers
// Copyright (c) 2017-2020 The Denarius developers
// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef INNOVA_KERNEL_H
#define INNOVA_KERNEL_H

#include "main.h"
#include "core.h"

// MODIFIER_INTERVAL: time to elapse before new modifier is computed
extern unsigned int nModifierInterval;

// MODIFIER_INTERVAL_RATIO:
// ratio of group interval length between the last group and the first group
static const int MODIFIER_INTERVAL_RATIO = 3;

// Compute the hash modifier for proof-of-stake
bool ComputeNextStakeModifier(const CBlockIndex* pindexPrev, uint64_t& nStakeModifier, bool& fGeneratedStakeModifier);
// READ-ONLY mainnet differential verify (HARD GATE #1): walks the loaded block
// index (production loader) comparing legacy vs optimized ComputeNextStakeModifier.
extern void VerifyStakeModifierDifferential();

// Check whether stake kernel meets hash target
// Sets hashProofOfStake on success return
bool CheckStakeKernelHash(const CBlockIndex* pindexPrev, unsigned int nBits, const CBlock& blockFrom, unsigned int nTxPrevOffset, const CTransaction& txPrev, const COutPoint& prevout, unsigned int nTimeTx, uint256& hashProofOfStake, uint256& targetProofOfStake, bool fPrintProofOfStake=false);

// Check kernel hash target and coinstake signature
// Sets hashProofOfStake on success return
bool CheckProofOfStake(const CBlockIndex* pindexPrev, const CTransaction& tx, unsigned int nBits, uint256& hashProofOfStake, uint256& targetProofOfStake);

// Resolve a transparent stake source from the candidate branch. The global
// tx index covers connected chain state, so an indexed but unconnected side
// branch may require reading its blocks directly.
bool ReadStakeSourceTransaction(const CBlockIndex* pindexPrev, const COutPoint& prevout,
    CTransaction& txPrev, CTxIndex& txindex, CBlock& blockFrom);
bool ReadStakeSourceTransactionForTesting(const CBlockIndex* pindexPrev,
    const COutPoint& prevout, CTransaction& txPrev, CTxIndex& txindex,
    CBlock& blockFrom, const std::map<uint256, CBlock>& candidateBlocks);

// Check whether the coinstake timestamp meets protocol
bool CheckCoinStakeTimestamp(int64_t nTimeBlock, int64_t nTimeTx);

// Get stake modifier checksum
unsigned int GetStakeModifierChecksum(const CBlockIndex* pindex);

// Check stake modifier hard checkpoints
bool CheckStakeModifierCheckpoints(int nHeight, unsigned int nStakeModifierChecksum);

bool GetKernelStakeModifier(uint256 hashBlockFrom, uint64_t& nStakeModifier, int& nStakeModifierHeight, int64_t& nStakeModifierTime, bool fPrintProofOfStake);
bool GetKernelStakeModifier(uint256 hashBlockFrom, const CBlockIndex* pindexPrev, uint64_t& nStakeModifier, int& nStakeModifierHeight, int64_t& nStakeModifierTime, bool fPrintProofOfStake);

// Get time weight using supplied timestamps
int64_t GetWeight(int64_t nIntervalBeginning, int64_t nIntervalEnd);

#endif // INNOVA_KERNEL_H
