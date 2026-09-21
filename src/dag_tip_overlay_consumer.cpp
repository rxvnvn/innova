// Copyright (c) 2019-2026 The Innova developers
#include "dag_tip_overlay_consumer.h"

namespace dag_tip_frontier {

DagTipOverlayConsumer::DagTipOverlayConsumer(LiveTipFrontierOverlay* overlay,
                                             DagTipOverlaySourceStateReader sourceReader,
                                             DagTipOverlaySourceHealthy sourceHealthy,
                                             void* context)
    : overlay_(overlay), sourceReader_(sourceReader), sourceHealthy_(sourceHealthy),
      context_(context), applying_(false), unavailable_(false),
      expectedRecords_(0), consumedRecords_(0), origin_(DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX)
{
}

bool DagTipOverlayConsumer::Available() const
{
    return !unavailable_ && !applying_;
}

bool DagTipOverlayConsumer::Consume(const DagTipCommittedDeltaEvent& event, std::string* error)
{
    if (error) error->clear();
    if (unavailable_ || !overlay_ || !overlay_->IsOpen())
    {
        unavailable_ = true;
        if (error) *error = "overlay consumer unavailable";
        return false;
    }
    if (event.kind == DagTipCommittedDeltaEvent::BEGIN)
    {
        if (applying_)
        {
            unavailable_ = true;
            if (error) *error = "overlay consumer nested BEGIN";
            return false;
        }
        LiveTipOverlayCheckpoint checkpoint;
        if (!overlay_->ReadCheckpoint(&checkpoint, error) ||
            checkpoint.phase != LIVE_OVERLAY_PHASE_CLEAN ||
            !overlay_->IsImmutableBindingValid(checkpoint) ||
            !event.hasInitialSourceStateId ||
            checkpoint.appliedSourceStateId != event.initialSourceStateId)
        {
            unavailable_ = true;
            return false;
        }
        checkpoint.phase = LIVE_OVERLAY_PHASE_APPLYING;
        if (!overlay_->WriteCheckpoint(checkpoint, error))
        {
            unavailable_ = true;
            return false;
        }
        expectedRecords_ = event.expectedRecordCount;
        consumedRecords_ = 0;
        origin_ = event.origin;
        applying_ = true;
        return true;
    }
    if (event.kind == DagTipCommittedDeltaEvent::RECORD)
    {
        if (!applying_ || event.origin != origin_ || consumedRecords_ >= expectedRecords_)
        {
            unavailable_ = true;
            if (error) *error = "overlay consumer RECORD outside APPLYING";
            return false;
        }
        const bool ok = event.record.op == DagTipDeltaRecord::TIP_ADD
            ? overlay_->AddTip(event.record.hash, error)
            : (event.record.op == DagTipDeltaRecord::TIP_REMOVE
               ? overlay_->RemoveTip(event.record.hash, error) : false);
        if (!ok)
        {
            unavailable_ = true;
            if (error && error->empty()) *error = "overlay consumer invalid RECORD";
            return false;
        }
        ++consumedRecords_;
        return true;
    }
    if (event.kind == DagTipCommittedDeltaEvent::END)
    {
        if (!applying_ || event.origin != origin_ ||
            consumedRecords_ != expectedRecords_ || event.expectedRecordCount != expectedRecords_ ||
            !event.hasFinalSourceStateId || !sourceReader_ || !sourceHealthy_ ||
            !sourceHealthy_(context_))
        {
            unavailable_ = true;
            if (error) *error = "overlay consumer END precondition failed";
            return false;
        }
        uint256 current;
        if (!sourceReader_(&current, context_) || current != event.finalSourceStateId)
        {
            unavailable_ = true;
            if (error) *error = "overlay consumer END source checkpoint mismatch";
            return false;
        }
        LiveTipOverlayCheckpoint checkpoint;
        if (!overlay_->ReadCheckpoint(&checkpoint, error) ||
            checkpoint.phase != LIVE_OVERLAY_PHASE_APPLYING ||
            !overlay_->IsImmutableBindingValid(checkpoint))
        {
            unavailable_ = true;
            return false;
        }
        checkpoint.phase = LIVE_OVERLAY_PHASE_CLEAN;
        checkpoint.appliedSourceStateId = event.finalSourceStateId;
        if (!overlay_->WriteCheckpoint(checkpoint, error))
        {
            unavailable_ = true;
            return false;
        }
        applying_ = false;
        return true;
    }
    unavailable_ = true;
    if (error) *error = "overlay consumer invalid event kind";
    return false;
}

} // namespace dag_tip_frontier
