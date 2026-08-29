// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CANDIDATE_FRONTIER_H
#define BITCOIN_CANDIDATE_FRONTIER_H

#include "main.h"
#include "txdb.h"

/** Rebuild the candidate tips set from a full mapBlockIndex scan.
 *  Called on startup if no persisted tips exist or generation is stale. */
bool RebuildCandidateTips();

/**
 * Evaluate the bounded candidate frontier and return the best eligible tip
 * using the EXACT same predicate as ActivateBestEligibleChain but operating
 * on the bounded candidate tips instead of scanning all mapBlockIndex.
 *
 * Returns NULL if no eligible candidate above nBestChainTrust exists.
 */
CBlockIndex* EvaluateCandidateFrontier();

/**
 * Shadow comparator: runs bounded frontier alongside legacy full scan and
 * reports any mismatch via printf.  Legacy scan remains authoritative.
 * Call after ActivateBestEligibleChain or on restart.
 */
void ShadowCompareCandidateSelection();

#endif // BITCOIN_CANDIDATE_FRONTIER_H