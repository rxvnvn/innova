// Copyright (c) 2019-2026 The Innova developers
// Isolated derived-shadow consumer for committed DAG-tip delta envelopes.
#ifndef INNOVA_DAG_TIP_OVERLAY_CONSUMER_H
#define INNOVA_DAG_TIP_OVERLAY_CONSUMER_H

#include "dag_tip_live_overlay.h"
#include "dag_tips_delta.h"

namespace dag_tip_frontier {

typedef bool (*DagTipOverlaySourceStateReader)(uint256* out, void* ctx);
typedef bool (*DagTipOverlaySourceHealthy)(void* ctx);

class DagTipOverlayConsumer
{
public:
    DagTipOverlayConsumer(LiveTipFrontierOverlay* overlay,
                          DagTipOverlaySourceStateReader sourceReader,
                          DagTipOverlaySourceHealthy sourceHealthy,
                          void* context);
    bool Consume(const DagTipCommittedDeltaEvent& event, std::string* error);
    bool Available() const;
private:
    LiveTipFrontierOverlay* overlay_;
    DagTipOverlaySourceStateReader sourceReader_;
    DagTipOverlaySourceHealthy sourceHealthy_;
    void* context_;
    bool applying_;
    bool unavailable_;
    uint64_t expectedRecords_;
    uint64_t consumedRecords_;
    DagTipDeltaOrigin origin_;
};

} // namespace dag_tip_frontier
#endif
