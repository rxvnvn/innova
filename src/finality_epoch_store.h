// Current finality epoch-state / curve-tree storage (Legacy DAG retirement,
// Phase 2 / H8). Narrow, current-state owner extracted from the retired engine:
// it owns ONLY the persisted epoch-state map, the epoch curve-tree map and the
// epoch-boundary index that current finality reads (GetEpochState /
// GetLastFinalizedEpochState / LoadEpochStates + curve-tree reload).
// It contains NO DAG responsibility: no tips, parents, siblings, score,
// colouring, frontier, selector, pruning, source-state or DAG mutation.
// Disk format/keys are unchanged (ownership move only).
#ifndef INNOVA_FINALITY_EPOCH_STORE_H
#define INNOVA_FINALITY_EPOCH_STORE_H

#include <memory>
#include "uint256.h"

struct CEpochState;
class CCurveTree;
class CTxDB;

class CFinalityEpochStateStore
{
public:
    CFinalityEpochStateStore();
    ~CFinalityEpochStateStore();

    CFinalityEpochStateStore(const CFinalityEpochStateStore&) = delete;
    CFinalityEpochStateStore& operator=(const CFinalityEpochStateStore&) = delete;

    /** Load persisted epoch states + curve-tree snapshots (same DB keys/codecs
     *  and iteration semantics as before the ownership extraction). */
    bool LoadEpochStates(CTxDB& txdb);

    /** In-memory lookup by epoch (current finality read path). */
    bool GetEpochState(int nEpoch, CEpochState& stateOut) const;

    /** In-memory curve-tree snapshot lookup by epoch (reload/lookup proof). */
    bool GetEpochCurveTree(int nEpoch, CCurveTree& treeOut) const;

    /** Most recent finalized epoch state, as current finality resolves it. */
    bool GetLastFinalizedEpochState(CEpochState& stateOut) const;

    /** Epoch-boundary index (loaded with the states; used by boundary queries). */
    bool IsEpochBoundary(const uint256& hash) const;
    void InsertEpochBoundary(const uint256& hash);

    /** Mutable accessors used only by the (zero-caller) Legacy DAG epoch
     *  calculation path; the store remains the single owner of the state. */
    void SetEpochState(int nEpoch, const CEpochState& state);
    void SetEpochCurveTree(int nEpoch, const CCurveTree& tree);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/** Current finality epoch-state storage owner (single global instance). */
CFinalityEpochStateStore& GetFinalityEpochStateStore();

#endif // INNOVA_FINALITY_EPOCH_STORE_H
