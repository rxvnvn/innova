// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

// A.10.1h..p / D R5 - Authoritative by-value startup cutover.
//
// Implements the BY_VALUE_AUTHORITATIVE startup path: replaces the legacy
// CTxDB::LoadBlockIndex() all-history CBlockIndex construction with a by-value
// flow driven from the validated V2 CURRENT generation. Invoked from init.cpp
// when the explicit authoritative mode is selected.
//
// Flow:
//   1. Build BlockIndexStartupBootstrap (best-tip + genesis permanent anchors,
//      authority + reader + derived + materializer + HotOwner, all generation-
//      coherent).
//   2. Publish exact startup globals (pindexBest, pindexGenesisBlock,
//      hashBestChain, nBestHeight, nBestChainTrust, nBestInvalidTrust) from the
//      bootstrap/by-value state.
//   3. Populate setStakeSeen via BlockIndexStakeSeenBuilder (A.10.1o).
//   4. Install the authoritative by-value staking navigator
//      (RetainBlockIndexAuthoritativeNavigator, A.10.1p) so wallet-depth never
//      falls back to LegacyBlockIndexAccessor.
//   5. Run HReg by-value rebuild + wallet rescan by-value (ca7c7e1).
//   6. DAG trust uses authoritative derived.dat chainTrust (A.9a.1b-corrected;
//      validation, no RestoreDAGTrustIntoChainTrust map scan).
//   7. Build the candidate frontier via BlockIndexCandidateStartupBuilder
//      (A.10.1m); later RebuildCandidateTips() is skipped.
//   8. Continue normal startup.
//
// historical residency = O(0); historical mapBlockIndex population = 0;
// historical pprev/pnext/pskip topology = 0.

#ifndef INNOVA_BLOCKINDEX_AUTHORITATIVE_STARTUP_H
#define INNOVA_BLOCKINDEX_AUTHORITATIVE_STARTUP_H

#include <stdint.h>
#include <string>
#include "blockindex_accessor.h"

class BlockIndexAuthoritativeLive; // fwd (G1 production live-authority accessor)

// Set to true (by InitBlockIndexAuthoritative) while an authoritative by-value
// startup is active. init.cpp uses it to skip the legacy DAG-rebuild /
// candidate-tip mapBlockIndex scans that are replaced by by-value providers.
extern bool g_fAuthoritativeStartup;

// Perform the authoritative by-value startup cutover for a validated V2
// generation at root. Fails closed (returns false + error) on ANY recovery;
// never falls back to legacy LoadBlockIndex. On success the authoritative
// navigator/bootstrap are retained process-lifetime so globals stay valid.
bool InitBlockIndexAuthoritative(const std::string& v2Root, std::string* error);

// Read-only authoritative historical lookup used by legacy startup consumers that
// need one by-value active-chain record; never materializes historical CBlockIndex.
bool AuthoritativeGetActiveSnapshotByHeight(int height, BlockIndexSnapshot* out);
// Resolve one active block by hash through the retained cold/hot authority.
// Returns false for unknown, non-active, closed, or authority-failed results.
bool ResolveAuthoritativeBlockSnapshot(const uint256& hash,
                                       BlockIndexSnapshot* out,
                                       std::string* error);
bool ResolveAuthoritativeActiveBlock(const uint256& hash,
                                     BlockIndexSnapshot* out,
                                     std::string* error);

// init.cpp can build a BlockIndexActiveChainReader for HReg + wallet rescan
// against the SAME selected generation (no second CURRENT open). Empty/0 if not
// in authoritative mode.
std::string AuthoritativeRootPath();
uint64_t AuthoritativeGeneration();

// A.10.1q / Stage1: emit the BLOCKINDEX_RESIDENCY line for the retained
// authoritative context, including the bootstrap HotOwner live metrics.
void PrintAuthoritativeResidency(const char* tag);

// G1: production live-authority accessor. NULL when NOT in authoritative mode.
// The returned object (if any) is retained process-lifetime by the authoritative
// startup context and bound to the single process-open base reader + the mutable
// tip. Callers must NOT free it. Used by the live block path to resolve parents
// by value and persist post-S blocks with bounded residency.
BlockIndexAuthoritativeLive* GetAuthoritativeLiveAuthority();

// G1 test-only arms for the DECISIVE causal closure (real ProcessBlock against
// an authoritative base). A test installs its own open BlockIndexAuthoritativeLive
// (bound to an isolated datadir generation) so the production block path observes
// authoritative mode + a live authority WITHOUT going through InitBlockIndexAuthoritative
// (which would mutate init.cpp process globals). GetAuthoritativeLiveAuthority()
// prefers this test handle while set. MUST be paired with ClearAuthoritativeLiveForTesting().
// These are NOT called by production startup and are inert when unset.
void SetAuthoritativeLiveForTesting(BlockIndexAuthoritativeLive* live);
void ClearAuthoritativeLiveForTesting();

#endif // INNOVA_BLOCKINDEX_AUTHORITATIVE_STARTUP_H
