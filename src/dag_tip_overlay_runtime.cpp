#include "dag_tip_overlay_runtime.h"
namespace dag_tip_frontier {
DagTipOverlayRuntime::DagTipOverlayRuntime():status_(DAG_TIP_OVERLAY_RUNTIME_UNAVAILABLE),recoveryInvocations_(0){}
DagTipOverlayRuntime::~DagTipOverlayRuntime(){Close();}
void DagTipOverlayRuntime::Close(){ consumer_.reset(); recovery_.reset(); overlay_.Close(); status_=DAG_TIP_OVERLAY_RUNTIME_UNAVAILABLE; recoveryInvocations_=0; }
bool DagTipOverlayRuntime::Start(const DagTipOverlayRuntimeConfig& c,std::string* error){
 Close(); config_=c; if(!c.sourceHealthy||!c.sourceReader||!c.sourceHealthy(c.context)){if(error)*error="DAG overlay runtime source unhealthy";return false;}
 if(!overlay_.Open(c.artifactPath,c.overlayDbDir,c.generation,const_cast<unsigned char*>(c.dagInputDigest),c.cacheCapacity,error)){status_=DAG_TIP_OVERLAY_RUNTIME_IMMUTABLE_FAILURE;return false;}
 consumer_.reset(new DagTipOverlayConsumer(&overlay_,c.sourceReader,c.sourceHealthy,c.context)); recovery_.reset(new DagTipOverlayRecovery(&overlay_,c.dagLinksDir,c.sourceReader,c.sourceHealthy,c.context));
 uint256 token; LiveTipOverlayCheckpoint cp;
 if(c.sourceReader(&token,c.context)&&overlay_.ReadCheckpoint(&cp,error)&&cp.phase==LIVE_OVERLAY_PHASE_CLEAN&&overlay_.IsImmutableBindingValid(cp)&&cp.appliedSourceStateId==token){status_=DAG_TIP_OVERLAY_RUNTIME_AVAILABLE;if(error)error->clear();return true;}
 ++recoveryInvocations_; if(!recovery_->Recover(error)){status_=DAG_TIP_OVERLAY_RUNTIME_UNAVAILABLE;return false;} status_=DAG_TIP_OVERLAY_RUNTIME_AVAILABLE;return true;
}
bool DagTipOverlayRuntime::Available()const{
 if(status_!=DAG_TIP_OVERLAY_RUNTIME_AVAILABLE || !consumer_ || !consumer_->Available() ||
    !config_.sourceHealthy || !config_.sourceReader || !config_.sourceHealthy(config_.context)) return false;
 uint256 token; LiveTipOverlayCheckpoint cp; std::string error;
 return config_.sourceReader(&token,config_.context) && overlay_.ReadCheckpoint(&cp,&error) &&
    cp.phase==LIVE_OVERLAY_PHASE_CLEAN && overlay_.IsImmutableBindingValid(cp) && cp.appliedSourceStateId==token;
}
DagTipOverlayRuntimeStatus DagTipOverlayRuntime::Status()const{return status_;}
bool DagTipOverlayRuntime::ConsumeCommittedDelta(const DagTipCommittedDeltaEvent& event,std::string* error){
 if(!consumer_ || !consumer_->Consume(event,error)){status_=DAG_TIP_OVERLAY_RUNTIME_UNAVAILABLE;return false;} return true;
}
uint64_t DagTipOverlayRuntime::RecoveryInvocations()const{return recoveryInvocations_;} LiveTipFrontierOverlay* DagTipOverlayRuntime::Overlay(){return overlay_.IsOpen()?&overlay_:NULL;} DagTipOverlayConsumer* DagTipOverlayRuntime::Consumer(){return consumer_.get();}
}
