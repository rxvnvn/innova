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

#include <string>

// Set to true (by InitBlockIndexAuthoritative) while an authoritative by-value
// startup is active. init.cpp uses it to skip the legacy DAG-rebuild /
// candidate-tip mapBlockIndex scans that are replaced by by-value providers.
extern bool g_fAuthoritativeStartup;

// Perform the authoritative by-value startup cutover for a validated V2
// generation at root. Fails closed (returns false + error) on ANY recovery;
// never falls back to legacy LoadBlockIndex. On success the authoritative
// navigator/bootstrap are retained process-lifetime so globals stay valid.
bool InitBlockIndexAuthoritative(const std::string& v2Root, std::string* error);

#endif // INNOVA_BLOCKINDEX_AUTHORITATIVE_STARTUP_H