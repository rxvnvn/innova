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
enum AuthoritativeBlockResolutionResult
{
    AUTHORITATIVE_BLOCK_FOUND = 0,
    AUTHORITATIVE_BLOCK_NOT_FOUND,
    AUTHORITATIVE_BLOCK_NOT_ACTIVE,
    AUTHORITATIVE_BLOCK_AUTHORITY_FAILURE,
};

// Typed hash resolution for callers that must distinguish ordinary absence,
// non-active history, and authority failure. Never materializes CBlockIndex.
AuthoritativeBlockResolutionResult ResolveAuthoritativeBlockSnapshotR(
    const uint256& hash, BlockIndexSnapshot* out, std::string* error);

bool ResolveAuthoritativeBlockSnapshot(const uint256& hash,
                                       BlockIndexSnapshot* out,
                                       std::string* error);
bool ResolveAuthoritativeActiveBlock(const uint256& hash,
                                     BlockIndexSnapshot* out,
                                     std::string* error);

// ---------------------------------------------------------------------------
// R4 — AUTHORITY_READY: ONE lifecycle readiness barrier (not a decorative flag).
//
// The four current prerequisites are evaluated against REAL live state at the end of
// InitBlockIndexAuthoritative; READY is published only when every one of them holds. Every
// consensus-sensitive consumer waits on this barrier and must not cross before it. This is
// lifecycle readiness only: FINALITY_EPOCH_OWNER_READY contributes the readiness of its
// already-frozen lifecycle condition and NOTHING finality-semantic (no late-vote semantics,
// no equivocation rule, no certificate denominator, no FINALITY_MIN_VOTERS, no
// irreversibility definition, no private NullStake semantics).
// ---------------------------------------------------------------------------
struct AuthorityReadyPrerequisites
{
    bool durableIndexLoaded;                 // V2 durable index loaded (selected generation)
    bool immutableAuthorityAvailable;        // immutable authority available (live authority open)
    bool trustProjectionReconciled;          // R2 trust projection reconciled
    bool finalityEpochOwnerLifecycleReady;   // FINALITY_EPOCH_OWNER_READY lifecycle condition
    AuthorityReadyPrerequisites()
        : durableIndexLoaded(false), immutableAuthorityAvailable(false),
          trustProjectionReconciled(false), finalityEpochOwnerLifecycleReady(false) {}
    // First unmet prerequisite ("" when all are satisfied).
    std::string WhyNotReady() const;
};

// Publish READY only when every prerequisite holds. Never marks a partially ready node.
bool AuthorityReadyMarkIfSatisfied(const AuthorityReadyPrerequisites& p, std::string* detail);

bool AuthorityReadyIsSet();

// Blocking consumer gate. Returns true only when READY has been published. In legacy
// (non-authoritative) operation the barrier is not applicable and returns true immediately.
bool AuthorityReadyWait(uint64_t timeoutMs, std::string* error);

// Consumer-side gate used by the lifecycle consumers in init.cpp: waits for the barrier and
// returns false when it did not become ready, so the caller fails the startup explicitly.
bool AuthorityReadyConsumerEnter(const char* consumer, std::string* error);

// Test seams (never used on a production path).
void AuthorityReadyResetForTest();
// Refusal detail of the last publication attempt (diagnostics only).
std::string AuthorityReadyRefusalDetail();
std::string AuthorityReadyStateName();

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

// Test-only lifecycle seam for isolated real InitBlockIndexAuthoritative cases.
// Never called by production startup.
void ResetBlockIndexAuthoritativeStartupForTest();
// R2c.2s/S2: authoritative by-value trust provider. Reproduces EXACT legacy
// CBlockIndex::GetBlockTrust semantics for a block described by a by-value
// authoritative snapshot (no resident CBlockIndex). Correctly applies
// GetBlockEntropy at/after FORK_HEIGHT_POEM and the exact legacy entropy input
// selection, plus reciprocal-target trust below POEM.
uint256 GetAuthoritativeBlockTrust(const BlockIndexSnapshot& snap);

// R2c.2s/S2 (repaired): bounded authoritative accumulated chainTrust for the
// REQUESTED HASH'S OWN PRE-DAG ANCESTRY. Returns in *out the exact accumulated
// legacy nChainTrust value that the real ColorBlock pre-DAG parent fallback
// consumes, computed as SUM GetAuthoritativeBlockTrust(X) over the hash-driven
// ancestry genesis -> ... -> hash resolved through the authoritative store's
// persisted hashPrev links (BlockIndexV2Reader::LookupByHash + GetParent).
// It is NOT the active chain at the same height: `PRE-DAG TRUST TRUTH !=
// ACTIVE CHAIN AT SAME HEIGHT`, so a side-branch parent resolves to its own
// branch trust. No all-history cache, no resident mapBlockIndex, no residency
// and no score cache; O(depth) time bounded by the requested pre-DAG height and
// O(1) temporary memory.
// FAILS CLOSED (false) on: reader unavailable; requested hash absent or
// identity-mismatched; requested hash not pre-DAG; any non-FOUND reader status;
// a claimed parent (authoritative hashPrev != 0) that is absent, unreadable, or
// contradicts the child's hash/height. A true chain start is recognised ONLY
// from the persisted authoritative `hashPrev == 0`; absence is never
// reinterpreted as canonical genesis.
bool GetAuthoritativeAccumulatedChainTrust(const uint256& hash,
                                           uint256* out, std::string* error);

#endif // INNOVA_BLOCKINDEX_AUTHORITATIVE_STARTUP_H
