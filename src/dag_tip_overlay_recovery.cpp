// Current-source rebuild for the derived live DAG-tip overlay.
#include "dag_tip_overlay_recovery.h"

#include "dag_tip_frontier.h"

#include <boost/filesystem.hpp>

#include <fstream>

namespace dag_tip_frontier {
namespace {

static bool ReadRawTip(std::ifstream& input, uint256* out, std::string* error)
{
    unsigned char raw[32];
    input.read((char*)raw, sizeof(raw));
    if (input.gcount() == 0)
    {
        if (input.eof()) return false;
        if (error) *error = "current tip stream read failure";
        throw std::runtime_error("current tip stream read failure");
    }
    if (input.gcount() != (std::streamsize)sizeof(raw))
    {
        if (error) *error = "current tip stream truncated";
        throw std::runtime_error("current tip stream truncated");
    }
    memcpy(out->begin(), raw, sizeof(raw));
    return true;
}

static bool Fail(DagTipOverlayRecoveryResult* result,
                 DagTipOverlayRecoveryStatus status,
                 const std::string& error)
{
    result->status = status;
    result->error = error;
    return false;
}

} // namespace

DagTipOverlayRecovery::DagTipOverlayRecovery(LiveTipFrontierOverlay* overlay,
                                             const std::string& dagLinksDir,
                                             DagTipOverlaySourceStateReader sourceReader,
                                             DagTipOverlaySourceHealthy sourceHealthy,
                                             void* context)
    : overlay_(overlay), dagLinksDir_(dagLinksDir), sourceReader_(sourceReader),
      sourceHealthy_(sourceHealthy), context_(context), options_(), available_(false)
{
}

DagTipOverlayRecovery::DagTipOverlayRecovery(LiveTipFrontierOverlay* overlay,
                                             const std::string& dagLinksDir,
                                             DagTipOverlaySourceStateReader sourceReader,
                                             DagTipOverlaySourceHealthy sourceHealthy,
                                             void* context,
                                             const BuildOptions& options)
    : overlay_(overlay), dagLinksDir_(dagLinksDir), sourceReader_(sourceReader),
      sourceHealthy_(sourceHealthy), context_(context), options_(options), available_(false)
{
}

bool DagTipOverlayRecovery::Available() const { return available_; }

bool DagTipOverlayRecovery::Recover(std::string* error)
{
    DagTipOverlayRecoveryResult result;
    const bool ok = Recover(&result);
    if (error) *error = result.error;
    return ok;
}

bool DagTipOverlayRecovery::Recover(DagTipOverlayRecoveryResult* result)
{
    if (!result) return false;
    *result = DagTipOverlayRecoveryResult();
    available_ = false;
    if (!overlay_ || !overlay_->IsOpen())
        return Fail(result, DAG_TIP_OVERLAY_RECOVERY_IMMUTABLE_BINDING_FAILURE,
                    "overlay is not open");
    if (!sourceHealthy_ || !sourceHealthy_(context_))
        return Fail(result, DAG_TIP_OVERLAY_RECOVERY_SOURCE_UNHEALTHY,
                    "source unhealthy before recovery");
    if (!sourceReader_)
        return Fail(result, DAG_TIP_OVERLAY_RECOVERY_SOURCE_STATE_UNAVAILABLE,
                    "source state reader unavailable");

    uint256 start;
    if (!sourceReader_(&start, context_))
        return Fail(result, DAG_TIP_OVERLAY_RECOVERY_SOURCE_STATE_UNAVAILABLE,
                    "source state unavailable before recovery");

    LiveTipOverlayCheckpoint existing;
    std::string detail;
    if (overlay_->ReadCheckpoint(&existing, &detail) &&
        existing.phase == LIVE_OVERLAY_PHASE_CLEAN &&
        overlay_->IsImmutableBindingValid(existing) &&
        existing.appliedSourceStateId == start)
    {
        result->status = DAG_TIP_OVERLAY_RECOVERY_AVAILABLE;
        available_ = true;
        return true;
    }

    LiveTipOverlayCheckpoint applying;
    if (!overlay_->MakeCheckpoint(LIVE_OVERLAY_PHASE_APPLYING, start, &applying, &detail) ||
        !overlay_->WriteCheckpoint(applying, &detail))
        return Fail(result, DAG_TIP_OVERLAY_RECOVERY_CHECKPOINT_PUBLICATION_FAILURE, detail);

    if (!overlay_->ClearPersistentOverrides(&detail))
        return Fail(result, DAG_TIP_OVERLAY_RECOVERY_OVERLAY_WRITE_FAILURE, detail);

    boost::filesystem::path work;
    try
    {
        work = boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path("dag-tip-overlay-recovery-%%%%-%%%%-%%%%");
        boost::filesystem::create_directories(work);
        BuildOptions options = options_;
        if (options.tempParent.empty()) options.tempParent = (work / "derive").string();
        const boost::filesystem::path currentTips = work / "current-tips.raw";
        CurrentDagTipDerivationResult derivation;
        if (!DeriveCurrentDagTipsBounded(dagLinksDir_, currentTips.string(), options, &derivation))
        {
            boost::filesystem::remove_all(work);
            return Fail(result, DAG_TIP_OVERLAY_RECOVERY_CURRENT_DERIVATION_FAILURE,
                        derivation.error);
        }

        result->currentRunCount = derivation.runCount;
        result->currentPeakChunkRecords = derivation.peakChunkRecords;

        TipFrontierReader seed;
        if (!overlay_->OpenImmutableSeedReader(&seed, &detail))
        {
            boost::filesystem::remove_all(work);
            return Fail(result, DAG_TIP_OVERLAY_RECOVERY_IMMUTABLE_BINDING_FAILURE, detail);
        }
        std::ifstream current(currentTips.string().c_str(), std::ios::binary);
        if (!current.good())
        {
            boost::filesystem::remove_all(work);
            return Fail(result, DAG_TIP_OVERLAY_RECOVERY_CURRENT_DERIVATION_FAILURE,
                        "current tip stream unavailable");
        }

        uint256 seedTip, currentTip;
        bool hasSeed = seed.Next(&seedTip);
        bool hasCurrent = ReadRawTip(current, &currentTip, &detail);
        while (hasSeed || hasCurrent)
        {
            if (!hasSeed || (hasCurrent && currentTip < seedTip))
            {
                if (!overlay_->AddTip(currentTip, &detail))
                {
                    boost::filesystem::remove_all(work);
                    return Fail(result, DAG_TIP_OVERLAY_RECOVERY_OVERLAY_WRITE_FAILURE, detail);
                }
                ++result->presentOverrides;
                hasCurrent = ReadRawTip(current, &currentTip, &detail);
            }
            else if (!hasCurrent || seedTip < currentTip)
            {
                if (!overlay_->RemoveTip(seedTip, &detail))
                {
                    boost::filesystem::remove_all(work);
                    return Fail(result, DAG_TIP_OVERLAY_RECOVERY_OVERLAY_WRITE_FAILURE, detail);
                }
                ++result->absentOverrides;
                hasSeed = seed.Next(&seedTip);
            }
            else
            {
                hasSeed = seed.Next(&seedTip);
                hasCurrent = ReadRawTip(current, &currentTip, &detail);
            }
        }
        current.close();
        boost::filesystem::remove_all(work);
    }
    catch (const std::exception& ex)
    {
        if (!work.empty()) { try { boost::filesystem::remove_all(work); } catch (...) {} }
        return Fail(result, DAG_TIP_OVERLAY_RECOVERY_CURRENT_DERIVATION_FAILURE, ex.what());
    }

    if (!sourceHealthy_(context_))
        return Fail(result, DAG_TIP_OVERLAY_RECOVERY_SOURCE_UNHEALTHY,
                    "source unhealthy after recovery derivation");
    uint256 end;
    if (!sourceReader_(&end, context_))
        return Fail(result, DAG_TIP_OVERLAY_RECOVERY_SOURCE_STATE_UNAVAILABLE,
                    "source state unavailable after recovery derivation");
    if (start != end)
        return Fail(result, DAG_TIP_OVERLAY_RECOVERY_SOURCE_CHANGED,
                    "source state changed during recovery");

    LiveTipOverlayCheckpoint clean;
    if (!overlay_->MakeCheckpoint(LIVE_OVERLAY_PHASE_CLEAN, end, &clean, &detail) ||
        !overlay_->IsImmutableBindingValid(clean) ||
        !overlay_->WriteCheckpoint(clean, &detail))
        return Fail(result, DAG_TIP_OVERLAY_RECOVERY_CHECKPOINT_PUBLICATION_FAILURE, detail);

    result->status = DAG_TIP_OVERLAY_RECOVERY_AVAILABLE;
    result->error.clear();
    available_ = true;
    return true;
}

} // namespace dag_tip_frontier
