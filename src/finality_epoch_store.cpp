// Current finality epoch-state / curve-tree storage owner (Legacy DAG
// retirement, Phase 2 / H8). The code moved here verbatim (semantics, DB keys,
// codecs and iteration semantics unchanged) from the retired engine; only ownership
// changed. No DAG responsibility lives here.
#include "finality_epoch_store.h"
#include "epoch_state.h"
#include "curvetree.h"
#include "txdb.h"
#include "main.h"

#include <map>
#include <set>
#include <stdio.h>

struct CFinalityEpochStateStore::Impl
{
    // Locking mirrors the previous DAG-state locking protection of this state;
    // it is a dedicated lock so the storage no longer depends on the DAG manager.
    mutable CCriticalSection cs_epochStore;
    std::map<int, CEpochState> mapEpochState;
    std::map<int, CCurveTree> mapEpochCurveTrees;
    std::set<uint256> setEpochBoundaryBlocks;
};

CFinalityEpochStateStore::CFinalityEpochStateStore() : impl(new Impl()) {}
CFinalityEpochStateStore::~CFinalityEpochStateStore() {}

bool CFinalityEpochStateStore::LoadEpochStates(CTxDB& txdb)
{
    LOCK(impl->cs_epochStore);

    std::map<int, CEpochState> mapStates;
    if (!txdb.IterateEpochStates(mapStates))
        return false;

    std::map<int, CCurveTree> mapTrees;
    if (!txdb.IterateCurveTreeEpochs(mapTrees))
        return false;

    impl->mapEpochState = mapStates;
    impl->mapEpochCurveTrees = mapTrees;

    for (const auto& pair : impl->mapEpochState)
    {
        if (pair.second.hashBoundaryBlock != 0)
            impl->setEpochBoundaryBlocks.insert(pair.second.hashBoundaryBlock);
    }

    if (!impl->mapEpochState.empty() || !impl->mapEpochCurveTrees.empty())
        printf("LoadEpochStates: loaded %d epoch states and %d curve-tree snapshots\n",
               (int)impl->mapEpochState.size(), (int)impl->mapEpochCurveTrees.size());

    return true;
}

bool CFinalityEpochStateStore::GetEpochState(int nEpoch, CEpochState& stateOut) const
{
    LOCK(impl->cs_epochStore);

    std::map<int, CEpochState>::const_iterator it = impl->mapEpochState.find(nEpoch);
    if (it == impl->mapEpochState.end())
        return false;

    stateOut = it->second;
    return true;
}

bool CFinalityEpochStateStore::GetEpochCurveTree(int nEpoch, CCurveTree& treeOut) const
{
    LOCK(impl->cs_epochStore);

    std::map<int, CCurveTree>::const_iterator it = impl->mapEpochCurveTrees.find(nEpoch);
    if (it == impl->mapEpochCurveTrees.end())
        return false;

    treeOut = it->second;
    return true;
}

bool CFinalityEpochStateStore::GetLastFinalizedEpochState(CEpochState& stateOut) const
{
    int nFinalizedEpoch = GetEpochForHeight(g_finalityTracker.GetFinalizedHeight());

    LOCK(impl->cs_epochStore);

    for (int nEpoch = nFinalizedEpoch; nEpoch >= 0; nEpoch--)
    {
        std::map<int, CEpochState>::const_iterator it = impl->mapEpochState.find(nEpoch);
        if (it == impl->mapEpochState.end())
            continue;
        if (it->second.hashCurveRoot == 0)
            continue;
        stateOut = it->second;
        return true;
    }

    return false;
}

bool CFinalityEpochStateStore::IsEpochBoundary(const uint256& hash) const
{
    LOCK(impl->cs_epochStore);
    return impl->setEpochBoundaryBlocks.count(hash) != 0;
}

void CFinalityEpochStateStore::InsertEpochBoundary(const uint256& hash)
{
    LOCK(impl->cs_epochStore);
    impl->setEpochBoundaryBlocks.insert(hash);
}

void CFinalityEpochStateStore::SetEpochState(int nEpoch, const CEpochState& state)
{
    LOCK(impl->cs_epochStore);
    impl->mapEpochState[nEpoch] = state;
}

void CFinalityEpochStateStore::SetEpochCurveTree(int nEpoch, const CCurveTree& tree)
{
    LOCK(impl->cs_epochStore);
    impl->mapEpochCurveTrees[nEpoch] = tree;
}

CFinalityEpochStateStore& GetFinalityEpochStateStore()
{
    static CFinalityEpochStateStore s_epochStateStore;
    return s_epochStateStore;
}
