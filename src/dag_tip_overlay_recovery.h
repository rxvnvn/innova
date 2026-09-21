// Current-source rebuild for the derived live DAG-tip overlay.
#ifndef INNOVA_DAG_TIP_OVERLAY_RECOVERY_H
#define INNOVA_DAG_TIP_OVERLAY_RECOVERY_H

#include "dag_tip_live_overlay.h"
#include "dag_tip_overlay_consumer.h"
#include "dag_tip_frontier.h"

namespace dag_tip_frontier {

enum DagTipOverlayRecoveryStatus
{
    DAG_TIP_OVERLAY_RECOVERY_INTERNAL_FAILURE = 0,
    DAG_TIP_OVERLAY_RECOVERY_AVAILABLE,
    DAG_TIP_OVERLAY_RECOVERY_SOURCE_UNHEALTHY,
    DAG_TIP_OVERLAY_RECOVERY_SOURCE_STATE_UNAVAILABLE,
    DAG_TIP_OVERLAY_RECOVERY_SOURCE_CHANGED,
    DAG_TIP_OVERLAY_RECOVERY_CURRENT_DERIVATION_FAILURE,
    DAG_TIP_OVERLAY_RECOVERY_IMMUTABLE_BINDING_FAILURE,
    DAG_TIP_OVERLAY_RECOVERY_OVERLAY_WRITE_FAILURE,
    DAG_TIP_OVERLAY_RECOVERY_CHECKPOINT_PUBLICATION_FAILURE
};

struct DagTipOverlayRecoveryResult
{
    DagTipOverlayRecoveryStatus status;
    std::string error;
    uint64_t presentOverrides;
    uint64_t absentOverrides;
    uint64_t currentRunCount;
    uint64_t currentPeakChunkRecords;
    DagTipOverlayRecoveryResult()
        : status(DAG_TIP_OVERLAY_RECOVERY_INTERNAL_FAILURE),
          presentOverrides(0), absentOverrides(0), currentRunCount(0),
          currentPeakChunkRecords(0) {}
};

class DagTipOverlayRecovery
{
public:
    DagTipOverlayRecovery(LiveTipFrontierOverlay* overlay,
                          const std::string& dagLinksDir,
                          DagTipOverlaySourceStateReader sourceReader,
                          DagTipOverlaySourceHealthy sourceHealthy,
                          void* context);
    DagTipOverlayRecovery(LiveTipFrontierOverlay* overlay,
                          const std::string& dagLinksDir,
                          DagTipOverlaySourceStateReader sourceReader,
                          DagTipOverlaySourceHealthy sourceHealthy,
                          void* context,
                          const BuildOptions& options);
    bool Recover(std::string* error);
    bool Recover(DagTipOverlayRecoveryResult* result);
    bool Available() const;
private:
    LiveTipFrontierOverlay* overlay_;
    std::string dagLinksDir_;
    DagTipOverlaySourceStateReader sourceReader_;
    DagTipOverlaySourceHealthy sourceHealthy_;
    void* context_;
    BuildOptions options_;
    bool available_;
};

} // namespace dag_tip_frontier
#endif // INNOVA_DAG_TIP_OVERLAY_RECOVERY_H
