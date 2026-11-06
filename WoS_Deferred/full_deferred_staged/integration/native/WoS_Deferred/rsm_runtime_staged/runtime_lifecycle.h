#pragma once
// Preparatory CPU lifecycle. Not included by the installed DLL.
// Storage owns resources; this class owns their frame/generation validity.
#if defined(_WIN32)
#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max
#endif
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_Deferred/rsm_clear_staged/clear_generation.h"
#if defined(_WIN32)
#pragma pop_macro("max")
#pragma pop_macro("min")
#endif

namespace WosRsmRuntime {
constexpr uint32_t ImageBase=0x00400000, ShadowClearCaller=0x009e9601;
constexpr uint32_t Argb8=21, Intz=0x5a544e49;
constexpr unsigned ShadowSize=2048;
enum class Route {Unknown,NormalVS2,NormalVS3,NormalVS8,FoliagePS4VS5,FoliagePS6VS5,FoliagePS6VS7};
inline bool Geometric(Route r){return r==Route::NormalVS2||r==Route::NormalVS3||r==Route::NormalVS8;}
inline bool Known(Route r){return Geometric(r)||r==Route::FoliagePS4VS5||r==Route::FoliagePS6VS5||r==Route::FoliagePS6VS7;}

// Values come from complete actual pre-Clear/pre-draw queries, not SetRT
// arguments or cached scene guesses. The adapter supplies the proof booleans
// only after its exact loaded-image/code and bounded-memory checks succeed.
struct Observation {
 WosRsmClear::Context context{};
 uint32_t imageBase=0,caller=0,globalsValid=0,current=0,nearScene=0,farScene=0,shadowActive=0;
 uint32_t colorFormat=0,depthFormat=0;unsigned depthWidth=0,depthHeight=0,colorSamples=0,depthSamples=0;
 bool executableVerified=false,callerCodeVerified=false,globalsStable=false,vpRead=false,targetsKnown=false;
 // Clear-only pre-call proof over ALL RT slots: no private normal/flux target
 // is bound. Even a color-only native Clear can otherwise erase another map.
 bool privateTargetsKnownUnbound=false;
};
inline bool ContextProof(const Observation&o,uint64_t device,bool clear,WosRsmClear::Context&out){
 out={};const auto&c=o.context;
 if(!o.executableVerified||o.imageBase!=ImageBase||!o.globalsStable||o.globalsValid!=7||!o.vpRead||!o.targetsKnown||
    !device||c.key.device!=device||!o.nearScene||!o.farScene||o.nearScene==o.farScene||
    (o.current!=o.nearScene&&o.current!=o.farScene)||c.scene!=o.current||c.slot!=(o.current==o.nearScene?0u:1u)||
    c.width!=ShadowSize||c.height!=ShadowSize||o.depthWidth!=c.width||o.depthHeight!=c.height||
    o.colorFormat!=Argb8||o.depthFormat!=Intz||o.colorSamples||o.depthSamples||
    (clear?(o.shadowActive>1||o.caller!=ShadowClearCaller||!o.callerCodeVerified):(o.shadowActive!=1)))return false;
 out=c;out.verifiedNativeShadow=true;out.identitiesPinned=true;
 if(!WosRsmClear::Valid(out)){out={};return false;}return true;
}

// Storage contract: BeginFrame/EndFrame release native pins but retain private
// allocations. Pin validates actual COM identities/descriptors, and refuses a
// different native binding in an already pinned slot during the same frame.
// EnsureNormal/ZeroNormal touch only private unbound storage. View is borrowed.
// No operation here performs readback, file I/O or shader-bytecode matching.
template<class Storage> class Lifecycle {
 Storage storage_{};WosRsmClear::Tracker tracker_{},fluxTracker_{};uint64_t frame_=0,device_=0;bool fluxEnabled_=false;
 struct PairedWriter {WosRsmClear::WriteToken normal{},flux{};bool active=false;};PairedWriter paired_[2]{};
public:
 using Native=typename Storage::Native;
 struct Lease {WosRsmClear::Snapshot stamp{},fluxStamp{};WosRsmClear::Context context{};typename Storage::View resources{};bool fluxValid=false;};
 Lifecycle()=default;Lifecycle(const Lifecycle&)=delete;Lifecycle&operator=(const Lifecycle&)=delete;
 ~Lifecycle(){Reset();}
 Storage& StorageForAdapter(){return storage_;}
 const Storage& StorageForAdapter()const{return storage_;}
 const WosRsmClear::MapState& State(unsigned slot)const{return tracker_.State(slot);}
 bool Active()const{return tracker_.Active();}
 void Reset(){tracker_.Reset();fluxTracker_.Reset();storage_.Reset();frame_=device_=0;for(auto&p:paired_)p={};}
 void EnableFlux(bool enabled){if(fluxEnabled_!=enabled){Reset();fluxEnabled_=enabled;}}
 bool FluxEnabled()const{return fluxEnabled_;}
 void BeginFrame(uint64_t frame,uint64_t canonicalDevice,bool enabled,bool executableVerified){
  tracker_.Reset();fluxTracker_.Reset();for(auto&p:paired_)p={};storage_.BeginFrame(canonicalDevice);frame_=frame;device_=canonicalDevice;
  tracker_.BeginCapture(frame,enabled&&executableVerified&&canonicalDevice!=0);
  fluxTracker_.BeginCapture(frame,enabled&&executableVerified&&canonicalDevice!=0&&fluxEnabled_);
 }
 void EndFrame(){tracker_.EndCapture();fluxTracker_.EndCapture();for(auto&p:paired_)p={};storage_.EndFrame();frame_=0;}
 // Invoke only AFTER the original native Clear has returned exactly once.
 // An unverified clear on a tracked DS invalidates it, even outside near/far.
 // A verified but partial/unknown/failed clear is fed to the tracker as-is.
 bool OnNativeClear(const Observation&o,const WosRsmClear::ClearEvent&event,const Native&native){
  if(!Active())return false;
  if(!o.privateTargetsKnownUnbound){ObservationFailed();return false;}
  if(!(event.flags&2u))return false;
  WosRsmClear::Context c{};
  if(!ContextProof(o,device_,true,c)){ObservationFailed(o.context.key);return false;}
  if(!storage_.Pin(c.slot,native,c)){ObservationFailed();return false;}
  auto e=event;e.verifiedNativeOrigin=true;WosRsmClear::Stamp stamp{},fluxStamp{};
  const bool normalClear=tracker_.OnNativeClear(c,e,stamp);
  const bool fluxClear=!fluxEnabled_||fluxTracker_.OnNativeClear(c,e,fluxStamp);
  if(!normalClear||!fluxClear)return false;
  const bool allocated=storage_.EnsureNormal(c.slot,c.width,c.height)&&(!fluxEnabled_||storage_.EnsureFlux(c.slot,c.width,c.height));
  const bool zeroed=allocated&&storage_.ZeroNormal(c.slot)&&(!fluxEnabled_||storage_.ZeroFlux(c.slot));
  const bool normalReady=tracker_.ConfirmNormalZero(c,stamp,storage_.NormalKey(c.slot),c.width,c.height,zeroed);
  const bool fluxReady=!fluxEnabled_||fluxTracker_.ConfirmNormalZero(c,fluxStamp,storage_.FluxKey(c.slot),c.width,c.height,zeroed);
  return normalReady&&fluxReady;
 }
 // The existing exact bytecode route matcher supplies Route. Unknown or
 // incompatible writers on a held DS MUST reach this path, never be ignored.
 // non-writers / documented NOTFOUND+null DS use the adapter's no-write path.
 bool BeginWriter(const Observation&o,Route route,bool stateCompatible,WosRsmClear::WriteToken&token,bool fluxEmitter=false){
  token={};if(!Active())return false;WosRsmClear::Context c{};
  if(!ContextProof(o,device_,false,c)||!Known(route)||!stateCompatible||(fluxEmitter&&route!=Route::NormalVS2&&route!=Route::NormalVS3)){UnknownDepthWriter(o.context.key);return false;}
  if(!storage_.Matches(c.slot,c)){ObservationFailed();return false;}
  if(!tracker_.BeginWriter(c,storage_.NormalKey(c.slot),Geometric(route),token)){fluxTracker_.ObservationFailed(c.key);return false;}
  if(fluxEnabled_){auto&p=paired_[c.slot];p={};p.normal=token;
   if(!fluxTracker_.BeginWriter(c,storage_.FluxKey(c.slot),fluxEmitter,p.flux)){ObservationFailed(c.key);token={};return false;}p.active=true;
  }return true;
 }
 bool FinishWriter(const WosRsmClear::WriteToken&token,bool nativeSucceeded,bool auxiliarySucceeded,bool restoreSucceeded){
  bool fluxOK=true;
  if(fluxEnabled_){
   if(token.stamp.slot>1){ObservationFailed();return false;}auto&p=paired_[token.stamp.slot];
   if(!p.active||p.normal.stamp.epoch!=token.stamp.epoch||p.normal.stamp.generation!=token.stamp.generation||p.normal.sequence!=token.sequence){ObservationFailed();return false;}
   fluxOK=fluxTracker_.FinishWriter(p.flux,nativeSucceeded,auxiliarySucceeded,restoreSucceeded);p={};
  }
  const bool normalOK=tracker_.FinishWriter(token,nativeSucceeded,auxiliarySucceeded,restoreSucceeded);
  if(!normalOK||!fluxOK){ObservationFailed(token.stamp.key);return false;}return true;
 }
 void UnknownDepthWriter(WosRsmClear::Key key){tracker_.UncoveredWriter(key);fluxTracker_.UncoveredWriter(key);}
 void ObservationFailed(WosRsmClear::Key key={}){tracker_.ObservationFailed(key);fluxTracker_.ObservationFailed(key);}
 // The bound current scene will usually be main HDR here. Acquire uses the
 // captured light context, never the current main-camera globals.
 bool AcquireMap(unsigned slot,Lease&out)const{
  out={};if(!Active()||slot>1)return false;const auto&m=tracker_.State(slot);
  WosRsmClear::Snapshot stamp{};
  if(!storage_.Matches(slot,m.context)||!tracker_.Get(m.context,storage_.NormalKey(slot),stamp))return false;
  out.stamp=stamp;out.context=m.context;out.resources=storage_.GetView(slot);return true;
 }
 bool AcquireFluxMap(unsigned slot,Lease&out)const{
  out={};if(!fluxEnabled_||!AcquireMap(slot,out))return false;
  if(!fluxTracker_.Get(out.context,storage_.FluxKey(slot),out.fluxStamp)){out={};return false;}out.fluxValid=true;return true;
 }
 bool StillCurrent(const Lease&lease)const{return tracker_.StillCurrent(lease.stamp)&&storage_.Matches(lease.context.slot,lease.context)&&(!lease.fluxValid||(fluxEnabled_&&fluxTracker_.StillCurrent(lease.fluxStamp)));}
};
}
