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
#include "dag.h" // CBlockDAGData (Option-R boundary closure records)

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
bool HasDagTipOverlayRuntimeForTest();
namespace dag_tip_frontier { class DagTipOverlayRuntime; }
dag_tip_frontier::DagTipOverlayRuntime* GetDagTipOverlayRuntimeForTest();
uint64_t DagTipOverlayRuntimeGenerationForTest();

// Test-only registration-boundary injection point. Production startup leaves the
// hook NULL (no-op). A test may install a hook that runs after the startup source
// repair + runtime Start but immediately BEFORE the observer health re-check, so
// an integration test can prove that a source authority which turns unhealthy at
// the exact registration point refuses to install the global observer. Only the
// timing is injected; the production IsDAGChildCountIndexHealthy predicate makes
// the decision unmodified.
typedef void (*DagObserverBoundaryHook)();
void SetDagObserverBoundaryHookForTest(DagObserverBoundaryHook hook);

// R2c.2s/S2: authoritative by-value trust provider. Reproduces EXACT legacy
// CBlockIndex::GetBlockTrust semantics for a block described by a by-value
// authoritative snapshot (no resident CBlockIndex). Correctly applies
// GetBlockEntropy at/after FORK_HEIGHT_POEM and the exact legacy entropy input
// selection, plus reciprocal-target trust below POEM.
uint256 GetAuthoritativeBlockTrust(const BlockIndexSnapshot& snap);

// R2c.2s/S2: bounded authoritative accumulated chainTrust for a retained hash.
// Returns in *out the exact accumulated legacy nChainTrust value that the real
// ColorBlock pre-DAG parent fallback consumes, computed by walking the active
// chain (bounded by the fixed pre-DAG boundary; no all-history cache, no
// resident mapBlockIndex). Returns false on authority failure or if the hash is
// not resolvable as an active authoritative vertex.
bool GetAuthoritativeAccumulatedChainTrust(const uint256& hash,
                                           uint256* out, std::string* error);

// Explicit by-value source contract. No borrowed block/DAG objects escape.
// Implementations must expose one stable source view for the call lifetime.
struct BoundaryScoreResult; // Option-R value-only boundary result (defined below)
class CanonicalDAGRecolorSource
{
public:
    virtual ~CanonicalDAGRecolorSource() {}
    virtual bool Block(const uint256&, BlockIndexSnapshot*, std::string*) const = 0;
    virtual bool Parents(const uint256&, std::vector<uint256>*, std::string*) const = 0;
    virtual bool PreDAGTrust(const uint256&, uint256*, std::string*) const = 0;
    // Option-R: reconstruct the exact former DAG-overwritten scalar of a
    // referenced non-retained DAG-era parent (its daglinks record is erased).
    // Must fail closed (false + empty result) when the raw block or any required
    // recursive metadata is unavailable, malformed, or identity-mismatched.
    // Returns the minimal boundary value the absent-DAG branch actually consumes
    // (the exact former nDAGScore); never fabricates topology or borrowed state.
    virtual bool ReconstructBoundaryScore(const uint256&,
                                          BoundaryScoreResult*, std::string*) const = 0;
    // Option-R closure: materialize the bounded DAG-context closure (the erased
    // parent P plus its blue-set/K-sample/anticone ancestors) as REAL canvas
    // records so GetBlueSet(P)/InferLocalK traverse genuine topology instead of
    // an empty set. Outputs: metadata (by-value snapshots for every closure
    // vertex incl pre-DAG), preDAGTrust (accumulated trust for pre-DAG leaves),
    // records (CBlockDAGData with recovered vDAGParents for every DAG-era closure
    // vertex), and ordered (height-sorted closure). Fail closed on any missing/
    // malformed required input. The caller merges these into its isolated canvas
    // and colors the union in height order.
    virtual bool ReconstructBoundaryClosure(
        const uint256&,
        std::map<uint256,BlockIndexSnapshot>* metadata,
        std::map<uint256,uint256>* preDAGTrust,
        std::map<uint256,CBlockDAGData>* records,
        std::vector<std::pair<int32_t,uint256>>* ordered,
        std::string* error) const = 0;
};
class CTxDB;
class AuthoritativeDAGRecolorSource : public CanonicalDAGRecolorSource
{
    CTxDB& db;
    // Optional mutation-scoped pending snapshot overlay (by value, NEVER
    // borrowed pointers). When non-NULL, Block()/height resolution consults
    // these snapshots FIRST (pending mutation-owned snapshot > current
    // certified live retained tail > immutable generation), so a post-generation
    // block that is part of THIS logical mutation but not yet published to the
    // external live authority remains resolvable during a reorg's staged
    // enumeration/recolor. Owned by the enclosing mutation (Reorganize/add);
    // never a global; may be NULL. See EnumerateAuthoritativeStagedScope.
    std::map<uint256,BlockIndexSnapshot> pendingSnapshots_;
public:
    explicit AuthoritativeDAGRecolorSource(CTxDB& source) : db(source) {}
    void SetPendingSnapshots(const std::map<uint256,BlockIndexSnapshot>& p) { pendingSnapshots_ = p; }
    const std::map<uint256,BlockIndexSnapshot>& GetPendingSnapshots() const { return pendingSnapshots_; }
    bool Block(const uint256&, BlockIndexSnapshot*, std::string*) const override;
    bool Parents(const uint256&, std::vector<uint256>*, std::string*) const override;
    bool PreDAGTrust(const uint256&, uint256*, std::string*) const override;
    bool ReconstructBoundaryScore(const uint256&,
                                  BoundaryScoreResult*, std::string*) const override;
    bool ReconstructBoundaryClosure(
        const uint256&,
        std::map<uint256,BlockIndexSnapshot>* metadata,
        std::map<uint256,uint256>* preDAGTrust,
        std::map<uint256,CBlockDAGData>* records,
        std::vector<std::pair<int32_t,uint256>>* ordered,
        std::string* error) const override;
};
// Option-R value-only boundary result. The exact former DAG-overwritten scalar
// a retained child's absent-DAG branch consumes for a non-retained DAG-era parent.
struct BoundaryScoreResult
{
    uint256 hash;         // block identity the result is bound to
    int32_t height;       // authoritative height of that block
    uint256 score;        // exact former DAG-overwritten nChainTrust (former nDAGScore)
    uint64_t sourceGeneration; // authoritative generation the inputs came from
    bool valid;           // false until a fully-reconstructed, certified value is set
    BoundaryScoreResult() : height(-1), sourceGeneration(0), valid(false) {}
};
struct CanonicalDAGRecolorRecord
{
    uint256 hash;
    int32_t height;
    uint256 nDAGScore;
    bool fBlue;
    int nInferredK;
    bool operator==(const CanonicalDAGRecolorRecord& b) const
    {
        return hash==b.hash && height==b.height && nDAGScore==b.nDAGScore &&
               fBlue==b.fBlue && nInferredK==b.nInferredK;
    }
};
struct CanonicalDAGRecolorStats
{
    size_t retainedVertices = 0;
    size_t preDAGBaseVertices = 0;
    size_t boundaryVertices = 0;
    size_t materializedObjects = 0;
    size_t objectBytes = 0; // excludes STL nodes/cache; measured RSS is separate
    // Finer-grained Option-R / C-full temporary structure counters (memory gate):
    size_t boundaryClosureVertices = 0;   // total closure vertices across all boundaries
    size_t boundaryClosureDagRecords = 0; // DAG-era closure records (CBlockDAGData)
    size_t metadataSnapshots = 0;         // BlockIndexSnapshot entries materialized
    size_t preDAGTrustEntries = 0;        // pre-DAG trust memo entries
    size_t parentsMemoEntries = 0;        // parents-of memo entries (boundary + retained)
    size_t localIndexEntries = 0;         // localIndex (by-value CBlockIndex*) entries
    size_t colorOrderEntries = 0;         // coloring-order (retained + closure) entries
    size_t rawBlockBytes = 0;             // cumulative raw CBlock bytes read (peak, sequential)
    size_t estimatedTemporaryBytes = 0;   // approximate peak bytes of all temporaries
};
// Exact scope, sorted (height,hash) output. Failure leaves output empty.
bool ReconstructAuthoritativeDAGFields(
    const std::vector<std::pair<int32_t,uint256>>& scope,
    const CanonicalDAGRecolorSource& source,
    std::vector<CanonicalDAGRecolorRecord>* result,
    CanonicalDAGRecolorStats* stats, std::string* error);

// Compatibility score projection for existing callers. The dagLinks keys are
// the retained scope; extra ordering metadata does not extend that scope.
bool ReconstructAuthoritativeDAGScore(
    const std::vector<std::pair<int32_t,uint256>>& heightSorted,
    const std::map<uint256,std::vector<uint256>>& dagLinks,
    std::map<uint256,uint256>* canonicalScores,
    std::string* error);

// ---------------------------------------------------------------------------
// S3 atomic authoritative full-field persistence.
//
// The staged delta (daglinks writes + tombstones) is applied into the active
// CTxDB WriteBatch by the caller (WriteDAGLinks/EraseDAGLinks) as part of a
// PHYSICAL SOURCE COMMIT. This engine then, inside that SAME batch:
//   1. builds a transaction-owned staged source view
//        (staged write/replacement > staged tombstone > persisted DB)
//      using CTxDB::Read (which resolves activeBatch via ScanBatch) for parents
//      and the by-value authoritative metadata/trust providers for blocks;
//   2. enumerates the retained canonical scope from the merged view
//      (persisted daglinks keys + staged writes - staged tombstones);
//   3. runs the accepted isolated C-full/Option-R recolor against that view
//      (NO global mapBlockIndex/g_dagManager mutation);
//   4. produces full-field (nDAGScore/fBlue/nInferredK) for every affected
//      retained canonical vertex in the C-full affected closure;
//   5. stages those full-field records back into the SAME WriteBatch (the
//      daglinks record carries full-field, so WriteDAGLinks persists it);
//   6. advances the SourceStateId exactly once and stages BOTH certificate
//      markers (child-count + DAG-score) bound to that token, in the batch;
//   7. the caller commits the batch atomically; on any failure the caller
//      aborts and nothing escapes.
//
// This is the batch-only staging path. It does NOT call the standalone
// quiesced-only publishers (EnsureDAGChildCountIndex / PublishDAGScoreCertificateAtomic).
//
// Caller contract: an active transaction must already be open with the topology
// mutation staged (WriteDAGLinks/EraseDAGLinks already applied). `newSourceToken`
// is minted by the caller (MintDAGSourceStateId) BEFORE this call and passed in.
// This engine stages (into the SAME WriteBatch) the authoritative full-field
// records for the affected retained closure, the new SourceStateId, the child-count
// certificate, and the DAG-score certificate - so token + certs + topology +
// full-field are all committed in ONE atomic batch by the caller's TxnCommit.
//
// Outputs:
//   result        - full-field records for every affected retained vertex.
//   affectedHashes- the C-full affected closure (all vertices whose derived
//                   full-field changed) - caller may persist/observe.
// Fail closed: returns false with empty output on any staged-view failure,
// enumeration error, recolor failure, boundary reconstruction failure, or
// missing/malformed staged data. On false the caller must TxnAbort (nothing
// staged by this engine escapes because it only adds to the already-open batch,
// which TxnAbort discards wholesale).
struct AuthoritativeDAGStageResult
{
    std::vector<CanonicalDAGRecolorRecord> fullFields;   // affected retained vertices
    CanonicalDAGRecolorStats stats;
    std::vector<uint256> affectedHashes;                 // C-full affected closure
    size_t stagedTopologyEntries = 0;
    size_t stagedFullFieldRecords = 0;
    size_t writeBatchBytes = 0;                          // approx batch bytes (daglinks+fiel+markers)
};
// S3 batch-only full-field staging. `chainedPending` (NULL when absent) supplies
// mutation-scoped, by-value snapshots for post-generation blocks that are part
// of THIS logical mutation but not yet published to the external live authority
// (e.g. the winning block of a Reorganize that must be resolvable for staged
// recolor while SetBestChain has not yet returned). Consulted FIRST during the
// recolor (pending mutation-owned snapshot > current certified live tail >
// immutable generation). Never a global; owned by the calling mutation.
bool StageAuthoritativeDAGScoreState(
    CTxDB& db,                                  // active transaction (WriteBatch open)
    const std::vector<std::pair<int32_t,uint256>>& stagedScope, // merged retained scope
    const uint256& newSourceToken,
    const std::map<uint256,BlockIndexSnapshot>* chainedPending, // mutation-scoped pending (may be NULL)
    AuthoritativeDAGStageResult* result,
    std::string* error);

// S3 staged view enumeration: return the merged retained scope
// (persisted daglinks keys + staged writes - staged tombstones), height-sorted.
// chainedPending (NULL when absent): same mutation-scoped by-value pending
// snapshots consulted FIRST when resolving retained-vertex heights, so a winning
// post-generation block that is not yet externally published (SetBestChain not
// yet returned) resolves by value during a live Reorganize. Fail closed on any
// read/enumeration error; identity/hash mismatch on a pending snapshot fails.
bool EnumerateAuthoritativeStagedScope(
    CTxDB& db,
    std::vector<std::pair<int32_t,uint256>>* scope,
    const std::map<uint256,BlockIndexSnapshot>* chainedPending, // may be NULL
    std::string* error);

#endif // INNOVA_BLOCKINDEX_AUTHORITATIVE_STARTUP_H
