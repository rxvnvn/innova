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

// A.9a.3e (NEW-N6): test-only observation hook, OFF by default. Fires ONLY
// after the by-value HybridSPV maturity authority has satisfied the exact
// legacy maturity gate and CheckProofOfStake is about to enter by-value
// source-block recovery. Read-only: it never alters consensus control flow
// and production always runs with the hook unset. Its absence does not change
// any acceptance/rejection decision.
typedef void (*HybridSvmRecoveryProbeFn)(int byValueDepth);
void SetHybridSvmRecoveryProbe(HybridSvmRecoveryProbeFn fn);

// Get stake modifier checksum
unsigned int GetStakeModifierChecksum(const CBlockIndex* pindex);

// Get stake modifier selection interval (seconds). Pure function of
// nModifierInterval and MODIFIER_INTERVAL_RATIO; exported so by-value
// navigation can mirror legacy GetKernelStakeModifier without kernel-local
// traversal.
int64_t GetStakeModifierSelectionInterval();

// Check stake modifier hard checkpoints
bool CheckStakeModifierCheckpoints(int nHeight, unsigned int nStakeModifierChecksum);

bool GetKernelStakeModifier(uint256 hashBlockFrom, uint64_t& nStakeModifier, int& nStakeModifierHeight, int64_t& nStakeModifierTime, bool fPrintProofOfStake);
bool GetKernelStakeModifier(uint256 hashBlockFrom, const CBlockIndex* pindexPrev, uint64_t& nStakeModifier, int& nStakeModifierHeight, int64_t& nStakeModifierTime, bool fPrintProofOfStake);

// A.9a.3c: production by-value resolution of an active-chain ancestor block's
// identity/metadata, used by wallet source discovery so an arbitrarily old note
// block no longer requires a resident CBlockIndex* / continuous pprev walk.
// A.9a.3d: the result is TYPED so a wallet caller can distinguish a genuine
// absent/not-applicable outcome (legacy fallback is safe pre-A.10) from an
// AUTHORITY_FAILURE (stale generation, corrupt cold record, or divergent seam)
// which MUST never fall back to an arbitrary-depth pprev walk.
enum StakingAncestorStatus
{
    STAKING_ANCESTOR_OK = 0,          // by-value result filled; hash/time authoritative
    STAKING_ANCESTOR_NO_NAVIGATOR,    // no production navigator retained (pre-A.10
                                      // fully-materialized) -> legacy pprev fallback is valid
    STAKING_ANCESTOR_NOT_FOUND,       // genuine absence / out-of-range; note not an ancestor
    STAKING_ANCESTOR_AUTHORITY_FAILURE // stale/corrupt/divergent authority -> NEVER pprev fallback
};
StakingAncestorStatus GetStakingAncestorSnapshot(const CBlockIndex* pindexPrev, int targetHeight,
    uint256* hashOut, unsigned int* nTimeOut, unsigned int* nFlagsOut);

// Get time weight using supplied timestamps
int64_t GetWeight(int64_t nIntervalBeginning, int64_t nIntervalEnd);

#endif // INNOVA_KERNEL_H
