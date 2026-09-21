#ifndef INNOVA_DAG_TIP_OVERLAY_RUNTIME_H
#define INNOVA_DAG_TIP_OVERLAY_RUNTIME_H

#include "dag_tip_live_overlay.h"
#include "dag_tip_overlay_consumer.h"
#include "dag_tip_overlay_recovery.h"

#include <memory>
#include <cstring>

namespace dag_tip_frontier {
enum DagTipOverlayRuntimeStatus { DAG_TIP_OVERLAY_RUNTIME_UNAVAILABLE=0, DAG_TIP_OVERLAY_RUNTIME_AVAILABLE, DAG_TIP_OVERLAY_RUNTIME_IMMUTABLE_FAILURE };
struct DagTipOverlayRuntimeConfig {
    uint64_t generation; std::string artifactPath, overlayDbDir, dagLinksDir; unsigned char dagInputDigest[32]; size_t cacheCapacity;
    DagTipOverlaySourceStateReader sourceReader; DagTipOverlaySourceHealthy sourceHealthy; void* context;
    DagTipOverlayRuntimeConfig() : generation(0), cacheCapacity(0), sourceReader(NULL), sourceHealthy(NULL), context(NULL) { memset(dagInputDigest,0,32); }
};
class DagTipOverlayRuntime {
public:
    DagTipOverlayRuntime(); ~DagTipOverlayRuntime();
    bool Start(const DagTipOverlayRuntimeConfig& config, std::string* error);
    void Close(); bool Available() const; DagTipOverlayRuntimeStatus Status() const;
    // Synchronous committed-envelope delivery. This touches only the owner-held
    // overlay/consumer; it never opens or retains the mutable source DB itself.
    bool ConsumeCommittedDelta(const DagTipCommittedDeltaEvent& event, std::string* error);
    uint64_t RecoveryInvocations() const; LiveTipFrontierOverlay* Overlay(); DagTipOverlayConsumer* Consumer();
private:
    DagTipOverlayRuntimeConfig config_;
    LiveTipFrontierOverlay overlay_; std::unique_ptr<DagTipOverlayConsumer> consumer_; std::unique_ptr<DagTipOverlayRecovery> recovery_; DagTipOverlayRuntimeStatus status_; uint64_t recoveryInvocations_;
};
} // namespace
#endif
