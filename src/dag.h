// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef INN_DAG_H
#define INN_DAG_H

#include "uint256.h"
#include "dag_tips_delta.h"
#include "serialize.h"
#include "sync.h"
#include "script.h"
#include "curvetree.h"

#include <vector>
#include <map>
#include <set>
#include <stdint.h>

class CBlockIndex;
class CTxDB;

// R3 / C6 section 6 — test seam for the FINAL positive prune predicate. It wraps the
// same static predicate dag.cpp uses at the consensus site (ResolveParentScoreAuthoritative);
// there is no second implementation and no test-only semantics.
struct BlockIndexSnapshot;
bool DAGRowObjectivelyPrunedForTest(CTxDB& db, const uint256& hash, int32_t height, std::string* why);

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static const int MAX_DAG_PARENTS = 32;          // max parents per block (1 primary + 31 merge)
static const unsigned char DAG_PARENT_TAG[4] = { 0x49, 0x44, 0x41, 0x47 }; // "IDAG"
static const int GHOSTDAG_K = 18;               // anticone tolerance for blue coloring (pre-DAGKNIGHT)
static const int DAG_MERGE_DEPTH = 64;          // merge parents within this depth of primary (~64s at 1s blocks)
static const int DAG_PRUNE_DEPTH = 100000;      // prune DAG data older than this (~28h at 1s blocks)

// IDAG Phase 4: DAGKNIGHT adaptive ordering constants
static const int DAGKNIGHT_MAX_ANTICONE_WINDOW = 64;  // max window for adaptive k estimation
static const int DAGKNIGHT_MIN_CONFIDENCE = 3;         // min supporting mass difference for confident ordering
static const int DAGKNIGHT_K_SAMPLE_DEPTH = 16;        // blocks to sample for k inference

// DAGKNIGHT k calibration bounds
// Floor: minimum k to tolerate network jitter at 1s block intervals
// Ceiling: maximum k to prevent overly permissive blue sets under attack
// At 1s blocks with ~2s propagation delay, expected parallelism ~2-3 blocks
// k should be at least 2x expected parallelism for safety margin
static const int DAGKNIGHT_K_FLOOR = 3;                // min inferred k (1s blocks, low latency)
static const int DAGKNIGHT_K_CEILING = 32;             // max inferred k (caps attack surface)
// Exponential moving average smoothing factor for k calibration (fixed-point, /256)
static const int DAGKNIGHT_K_EMA_ALPHA = 64;           // ~25% weight to new sample


// ---------------------------------------------------------------------------
// DAG Parent Commitment (coinbase OP_RETURN)
// ---------------------------------------------------------------------------

/** Extract DAG parent hashes from a coinbase OP_RETURN output.
 *  Returns empty vector if no DAG commitment found. */
std::vector<uint256> ExtractDAGParents(const CScript& scriptCoinbase);

/** Build a coinbase OP_RETURN script committing to DAG parents.
 *  Format: OP_RETURN <IDAG tag(4) || count(1) || hash1(32) || hash2(32) || ...> */
CScript BuildDAGParentScript(const std::vector<uint256>& vParents);


// ---------------------------------------------------------------------------
// Per-epoch DAG state (persisted to LevelDB)
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


// ---------------------------------------------------------------------------
// Per-block DAG metadata (memory only — persisted separately via LevelDB)
// ---------------------------------------------------------------------------

struct CBlockDAGData
{
    std::vector<uint256> vDAGParents;    // parent block hashes (index 0 = primary parent)
    std::vector<uint256> vDAGChildren;   // children that reference this block as parent
    bool fBlue;                          // GHOSTDAG/DAGKNIGHT blue/red coloring
    uint256 nDAGScore;                   // cumulative blue-set trust score
    int nDAGOrder;                       // position in DAG linear order
    int nInferredK;                      // Phase 4: DAGKNIGHT-inferred k (-1 = GHOSTDAG era)

    CBlockDAGData()
    {
        fBlue = true;
        nDAGScore = 0;
        nDAGOrder = -1;
        nInferredK = -1;
    }

    IMPLEMENT_SERIALIZE
    (
        READWRITE(vDAGParents);
        READWRITE(vDAGChildren);
        READWRITE(fBlue);
        READWRITE(nDAGScore);
        READWRITE(nDAGOrder);
        READWRITE(nInferredK);
    )
};


// ---------------------------------------------------------------------------
// DAG Manager — holds all DAG state, drives GHOSTDAG/DAGKNIGHT coloring + ordering
// ---------------------------------------------------------------------------

struct BlockIndexSnapshot; // forward declaration (defined in blockindex_authoritative_startup.h)

// Envelope-scoped pre-image of an authoritative (S3) DAG prune. The caller
// (AddToBlockIndex) owns the instance for the duration of the ADD envelope so
// a later SetBestChain-failure rollback can restore the exact pre-prune source:
// every deleted durable record byte-for-byte plus the durable clean-height
// marker state. Never a process-global; never touched in legacy mode.
// path; a NULL pointer means "no rollback pre-image requested".
struct DagPruneRollbackCapture
{
    std::vector<std::pair<uint256, CBlockDAGData> > records; // exact deleted rows
    // F2-B1-R: exact pre-image of the row-erase provenance record per deleted row
    // (parallel to `records`; -1 = no record existed). A prune erase clears a stale
    // non-prune de-materialization record for the row it physically erases, so the
    // rollback must be able to put the marker state back exactly.
    std::vector<int> rowEraseOrigins;
    bool cleanHeightPresent;
    int cleanHeight;
    bool pruneFloorPresent; // exact pre-image of the erase-provenance marker
    int pruneFloor;
    bool committed; // the prune's physical source commit succeeded
    DagPruneRollbackCapture() : cleanHeightPresent(false), cleanHeight(-1),
        pruneFloorPresent(false), pruneFloor(-1), committed(false) {}
};

class DagMutationPreview; // R2c.2s/S5 owned transaction-scoped preview seam

class CDAGManager
{
public:
    mutable CCriticalSection cs_dag;

    CDAGManager() : nPrunedBelowHeight(-1), recolorBlockIndex(NULL) {}
    // Isolated maintenance canvas. The caller owns this lookup for the entire
    // canvas lifetime; coloring never falls back to the live index when set.
    explicit CDAGManager(std::map<uint256, CBlockIndex*>& index)
        : nPrunedBelowHeight(-1), recolorBlockIndex(&index) {}
    void LoadRecolorCanvas(const std::map<uint256, CBlockDAGData>& records);

    /** Initialize DAG data for a newly accepted block.
     *  Must be called under cs_main. Sets parents, registers children, updates tips.
     *  mutationPreview (R2c.2s/S5): the owning envelope's transaction-scoped
     *  preview, explicitly threaded to mutation-internal consumers; NULL keeps
     *  the accepted legacy behavior. */
    bool InitBlockDAGData(CBlockIndex* pindex, const std::vector<uint256>& vParents,
                          const DagMutationPreview* mutationPreview = NULL);

    /** Get current DAG tips (blocks with no children). */
    std::vector<uint256> GetDAGTips() const;

    /** Select the best DAG tip by score. */
    CBlockIndex* SelectBestDAGTip() const;

    /** Get DAG linear ordering from a given tip back to genesis.
     *  nMaxBlocks limits computation (0 = unlimited). */
    std::vector<uint256> GetDAGLinearOrder(const uint256& hashTip, int nMaxBlocks = 0) const;

    /** Compute DAG score for a block: sum of GetBlockTrust() for all blue ancestors. */
    uint256 ComputeDAGScore(CBlockIndex* pindex);

    /** GHOSTDAG blue-set coloring (used below FORK_HEIGHT_DAGKNIGHT). */
    void ColorBlock(CBlockIndex* pindex);

    /** DAGKNIGHT adaptive coloring for a block (Phase 4). */
    void ColorBlockDAGKnight(CBlockIndex* pindex);

    // --- F2: authoritative DAG parent-score cutover -------------------------
    // INVARIANT: DAG PARENT SCORE TRUTH != mapDAGData RESIDENCY and
    //            DAG PARENT SCORE TRUTH != mapBlockIndex RESIDENCY.
    // A valid score of exactly ZERO is a legitimate FOUND value and is never
    // conflated with a resolution failure.
    enum class DAGParentScoreStatus
    {
        FOUND,      // a definitive score (possibly zero) was resolved
        NOT_FOUND,  // the parent has no score source (authority vertex absent)
        FAILURE     // the authoritative source is unhealthy/unreadable/inconsistent
    };

    /** B-1: WHICH authoritative source produced a FOUND score. The status is
     *  unchanged (consumers only test FAILURE); this records the typed absence
     *  classification so a FOUND value is never silently attributable to the
     *  wrong authority:
     *    CANONICAL_ROW        = a certified-current canonical daglinks row
     *                           (FOUND_CANONICAL_SCORE),
     *    PRUNED_BOUNDARY      = the row is legitimately gone under the accepted
     *                           prune/erase lifecycle and the exact former scalar
     *                           was reconstructed by value (FOUND_PRUNED_BOUNDARY),
     *    DEGRADED_CERTIFICATE_BOUNDARY = the certificate does not bind the
     *                           retained set in this session (uncertified, or a
     *                           rebuild-required revocation the accepting
     *                           mutation itself re-certifies at commit) and the
     *                           exact former scalar was reconstructed by value.
     */
    enum class DAGParentScoreSource
    {
        NONE,
        CANONICAL_ROW,
        PRUNED_BOUNDARY,
        DEGRADED_CERTIFICATE_BOUNDARY
    };

    struct DAGParentScoreResult
    {
        DAGParentScoreStatus status;
        uint256 score;
        DAGParentScoreSource source;
        DAGParentScoreResult() : status(DAGParentScoreStatus::NOT_FOUND), score(0),
                                 source(DAGParentScoreSource::NONE) {}
    };

    /** One logical DAG parent-score resolver (F2).
     *
     *  fAuthoritativeParentScore == false (legacy live mode and every isolated
     *  recolor canvas) preserves TODAY's resident semantics byte-for-byte: the
     *  block's own mapDAGData entry, then the (non post-DAG proof-of-stake)
     *  resident mapBlockIndex chainTrust fallback; a miss yields NOT_FOUND with
     *  score 0, exactly as before.
     *
     *  fAuthoritativeParentScore == true (the authoritative accept path) resolves
     *  the parent score from the authority, never from residency:
     *    - post-DAG proof-of-stake parent  -> FOUND(0) (consensus exclusion),
     *    - pre-DAG parent                  -> entropy-correct accumulated trust
     *                                         (GetAuthoritativeAccumulatedChainTrust),
     *    - post-DAG proof-of-work parent   -> the certified persisted DAG score
     *                                         row (ReadDAGLinks) gated by
     *                                         IsDAGScoreAuthorityHealthy.
     *  Any unavailable/unhealthy/unreadable authority returns FAILURE; score 0 is
     *  NEVER substituted on failure and there is NO resident fallback.
     */
    /** B-1 mutation-vs-query policy for the authoritative parent-score rule.
     *
     *  QUERY (default): the accepted direct-resolution contract. A certificate
     *  that does not bind the retained set (uncertified) or that is positively
     *  degraded (revoked/corrupt/unavailable) is FAILURE, never a value.
     *
     *  MUTATION: the authoritative coloring of a block being ACCEPTED. The
     *  mutation's own commit republishes the score certificate at its new source
     *  token (StageAuthoritativeDAGScoreState -> StageDAGScoreCertificateInBatch),
     *  so a non-binding certificate must not brick the accept path: the exact
     *  former scalar is reconstructed by value under the same provenance and
     *  source-binding rules, and the mutation re-certifies at commit. A corrupt or
     *  unavailable authority still fails closed in BOTH policies, because that is
     *  positive evidence of damage rather than of a certificate that simply is not
     *  binding yet.
     */
    enum class DAGParentScorePolicy { QUERY, MUTATION };

    DAGParentScoreResult ResolveDagParentScore(const uint256& hashParent,
                                               bool fAuthoritativeParentScore,
                                               std::string* error,
                                               DAGParentScorePolicy policy =
                                                   DAGParentScorePolicy::QUERY) const;

    /** Authoritative-mode coloring of a newly accepted block (F2).
     *  Returns false (fail closed) when a parent score cannot be resolved
     *  authoritatively; the DAG data of `pindex` is left unmodified on failure.
     *  When g_fAuthoritativeStartup is false this is exactly the legacy
     *  void ColorBlock/ColorBlockDAGKnight behaviour and never fails. */
    bool ColorBlockAuthoritative(CBlockIndex* pindex, std::string* error);
    bool ColorBlockDAGKnightAuthoritative(CBlockIndex* pindex, std::string* error);

    /** Write DAG links for a block to LevelDB. */
    bool WriteDAGLinks(CTxDB& txdb, const uint256& hash);

    /** Load all DAG links from LevelDB into memory. */
    bool LoadDAGLinks(CTxDB& txdb);

    /** Load persisted epoch states and curve-tree snapshots from LevelDB. */
    bool LoadEpochStates(CTxDB& txdb);

    /** Prune DAG data below nHeight - DAG_PRUNE_DEPTH, preserving epoch boundaries.
     *  rollbackCapture (optional, S3 authoritative mode only) receives the exact
     *  pre-image of every deleted durable record + the durable clean-height
     *  marker so a failed outer envelope can restore the pre-prune source.
     *  chainedPending (optional) supplies the mutation-scoped pending overlay for
     *  vertices committed by the CURRENT envelope (e.g. the block being added),
     *  which are not yet resolvable through generation/live authority - exactly
     *  like the S3 enumerate/stage callers. */
    bool PruneDAGData(CTxDB& txdb, int nHeight, DagPruneRollbackCapture* rollbackCapture = NULL,
                      const std::map<uint256,BlockIndexSnapshot>* chainedPending = NULL);

    /** Compute epoch state for a completed epoch. */
    bool ComputeEpochState(int nEpoch, int nEpochInterval, const DagMutationPreview* mutationPreview = NULL);

    /** Write epoch state to LevelDB. */
    bool WriteEpochState(CTxDB& txdb, int nEpoch);

    /** Get epoch state (from memory cache). */
    bool GetEpochState(int nEpoch, CEpochState& stateOut) const;

    // Read-only selection introspection; cannot manufacture an exemption.
    bool IsEpochBoundaryForTest(const uint256& hash) const
    {
        LOCK(cs_dag);
        return setEpochBoundaryBlocks.count(hash) != 0;
    }

    /** Get the most recent finalized epoch state known to the DAG manager. */
    bool GetLastFinalizedEpochState(CEpochState& stateOut) const;

    /** Get the number of in-memory DAG entries. */
    int GetDAGEntryCount() const;


    /** Set pruned below height (used on startup to restore from LevelDB). */
    void SetPrunedBelowHeight(int nHeight);

    /** Check if a block has DAG data. */
    bool HasDAGData(const uint256& hash) const;

    /** Get DAG data for a block (returns false if not found). */
    bool GetDAGData(const uint256& hash, CBlockDAGData& dataOut) const;

    /** Test helper: directly insert DAG data into mapDAGData. */
    void SetDAGDataForTest(const uint256& hash, const CBlockDAGData& data);

    /** Test helper: clear all DAG data (for test cleanup). */
    void ClearDAGDataForTest();

    /** Get the set of blocks that are DAG siblings of a given block
     *  (blocks at similar height that share some parents). */
    std::set<uint256> GetDAGSiblingBlocks(const uint256& hashBlock) const;

    /** Get the selected parent (highest-scoring parent) of a block. */
    uint256 GetSelectedParent(const uint256& hashBlock) const;

    /** Remove DAG data for a block (used during reorg). */
    void RemoveBlockDAGData(const uint256& hashBlock);

private:
    std::map<uint256, CBlockDAGData> mapDAGData;
    std::set<uint256> setDAGTips;
    std::map<uint256, std::set<uint256>> mapPendingChildrenByParent;
    std::map<int, CEpochState> mapEpochState;
    std::map<int, CCurveTree> mapEpochCurveTrees;
    std::set<uint256> setEpochBoundaryBlocks;
    int nPrunedBelowHeight;

    std::map<uint256, CBlockIndex*>* recolorBlockIndex;
    std::map<uint256, CBlockIndex*>& RecolorBlockIndex() const;

    // F2 resolver internals (see ResolveDagParentScore for the contract).
    DAGParentScoreResult ResolveParentScoreLegacy(const uint256& hashParent) const;
    DAGParentScoreResult ResolveParentScoreAuthoritative(const uint256& hashParent,
                                                         std::string* error,
                                                         DAGParentScorePolicy policy =
                                                             DAGParentScorePolicy::QUERY) const;
    bool ColorBlockImpl(CBlockIndex* pindex, bool fAuthoritativeParentScore,
                        std::string* error);
    bool ColorBlockDAGKnightImpl(CBlockIndex* pindex, bool fAuthoritativeParentScore,
                                 std::string* error);

    // Performance: LRU cache for blue sets (avoids recomputing expensive BFS)
    mutable std::map<uint256, std::set<uint256>> mapBlueSetCache;
    static const int BLUESET_CACHE_MAX = 128;

    /** Internal: get blue set with caching */
    std::set<uint256> GetBlueSetCached(const uint256& hashBlock) const;

    /** Internal: child/pending-child and cache maintenance helpers. */
    void AddChildNoDuplicate(std::vector<uint256>& vChildren, const uint256& hashChild) const;
    void InvalidateBlueSetCacheForBlock(const uint256& hashBlock) const;
    void RebuildPendingChildIndex();

    // R2c.1d1: only incremental live mutations use these membership-aware
    // helpers. Startup bulk reconstruction deliberately remains raw/untracked.
    bool TrackedInsertTip(const uint256& hash);
    bool TrackedEraseTip(const uint256& hash);

    /** DAGKNIGHT: Infer local k from DAG neighborhood. */
    int InferLocalK(const uint256& hashBlock) const;


    /** Internal: get anticone of block X relative to a blue set */
    int AnticoneSize(const uint256& hashBlock, const std::set<uint256>& blueSet) const;

    /** Internal: collect blue set reachable from a block */
    std::set<uint256> GetBlueSet(const uint256& hashBlock) const;

    /** Internal: get all blocks reachable from a hash (bounded by depth) */
    std::set<uint256> GetPastSet(const uint256& hashBlock, int nMaxDepth) const;
};


extern CDAGManager g_dagManager;


#endif // INN_DAG_H
