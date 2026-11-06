#pragma once
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_Deferred/rsm_runtime_staged/d3d9_storage.h"
#include <utility>
namespace WosRsmRuntime {
struct EngineGlobals {uint32_t current=0,root=0,nearScene=0,farScene=0,shadowActive=0,valid=0;};
constexpr unsigned char ClearCode[32]={0x96,0x90,0x04,0x00,0x00,0xd9,0x1c,0x24,0x52,0xe8,0xc1,0xab,0xe9,0xff,0x83,0xc4,0x04,0x50,0x8b,0x83,0xac,0x00,0x00,0x00,0x57,0x6a,0x00,0x6a,0x00,0x55,0xff,0xd0};
enum class DrawObservationResult {Inactive,NoDepthWrite,UntrackedDepth,Observed,Failed};
template<class T>inline void DropReference(T*&p){if(p)p->Release();p=nullptr;}
template<class T>inline IUnknown*Canonical(T*p){IUnknown*q=nullptr;if(!p||FAILED(p->QueryInterface(IID_IUnknown,reinterpret_cast<void**>(&q))))DropReference(q);return q;}
inline uint64_t IdentityValue(IUnknown*p){return uint64_t(reinterpret_cast<uintptr_t>(p));}
struct CapturedObservation {
 Observation observation{};WosRsmClear::ClearEvent clear{};D3DVIEWPORT9 viewport{};
 D3D9Storage::Native native{};IUnknown*deviceIdentity=nullptr,*colorIdentity=nullptr,*depthIdentity=nullptr;
 bool stateCompatible=false,noDepth=false,consumed=false;uint64_t frameSerial=0;
 CapturedObservation()=default;CapturedObservation(const CapturedObservation&)=delete;CapturedObservation&operator=(const CapturedObservation&)=delete;
 ~CapturedObservation(){DropReference(depthIdentity);DropReference(colorIdentity);DropReference(deviceIdentity);DropReference(native.depth);DropReference(native.color);}
};
// Engine is an injected read-only exact-image/memory adapter. Production and
// test adapters share this state machine; F7 enabled/resource IDs are absent.
template<class Engine>class ObservationBridge {
 Engine engine_{};D3D9Lifecycle runtime_{};bool verificationTried_=false,verified_=false;
 uint64_t frameSerial_=0,deviceKey_=0;unsigned mrtSlots_=0;
 bool Tracked(WosRsmClear::Key key)const{for(unsigned i=0;i<2;++i){const auto&s=runtime_.State(i);if(s.occupied&&s.context.key==key)return true;}return false;}
 bool EngineObservation(uintptr_t caller,bool clear,CapturedObservation&p){
  auto&o=p.observation;o.imageBase=engine_.ImageAddress();o.executableVerified=verified_;o.caller=uint32_t(caller);
  if(!verified_||o.imageBase!=ImageBase)return false;
  const auto before=engine_.ReadGlobals();uint32_t arena[3]{},afterArena[3]{};size_t used=0;
  o.globalsValid=before.valid;o.current=before.current;o.nearScene=before.nearScene;o.farScene=before.farScene;o.shadowActive=before.shadowActive;
  o.context.scene=before.current;o.context.slot=before.current==before.nearScene?0u:1u;
  const bool inArena=engine_.Read(ImageBase+0xd15c64,arena,sizeof(arena))&&engine_.ArenaBounds(arena,used)&&
   before.valid==7&&before.current>=arena[0]&&uint64_t(before.current)+0x4e0<=arena[2];
  if(inArena)o.vpRead=engine_.Read(uintptr_t(before.current)+0x190,o.context.vp,sizeof(o.context.vp));
  const auto after=engine_.ReadGlobals();
  o.globalsStable=!std::memcmp(&before,&after,sizeof(before))&&engine_.Read(ImageBase+0xd15c64,afterArena,sizeof(afterArena))&&!std::memcmp(arena,afterArena,sizeof(arena));
  if(clear&&caller==ShadowClearCaller){unsigned char bytes[32]{};o.callerCodeVerified=engine_.Read(caller-32,bytes,32)&&!std::memcmp(bytes,ClearCode,32);}
  return inArena&&o.vpRead&&o.globalsStable;
 }
 bool DeviceAndDepth(IDirect3DDevice9*d,CapturedObservation&p){
  p.native.device=d;p.deviceIdentity=Canonical(d);p.observation.context.key.device=IdentityValue(p.deviceIdentity);
  if(!p.deviceIdentity||p.observation.context.key.device!=deviceKey_)return false;
  HRESULT hr=d->GetDepthStencilSurface(&p.native.depth);
  if(hr==D3DERR_NOTFOUND&&!p.native.depth){p.noDepth=true;return true;}
  if(FAILED(hr)||!p.native.depth)return false;p.depthIdentity=Canonical(p.native.depth);
  p.observation.context.key.depth=IdentityValue(p.depthIdentity);return p.depthIdentity!=nullptr;
 }
 bool ColorAndDescriptors(IDirect3DDevice9*d,CapturedObservation&p){
  if(FAILED(d->GetRenderTarget(0,&p.native.color))||!p.native.color)return false;
  p.colorIdentity=Canonical(p.native.color);if(!p.colorIdentity)return false;
  p.observation.context.color=IdentityValue(p.colorIdentity);D3DSURFACE_DESC cd{},dd{};
  if(FAILED(p.native.color->GetDesc(&cd)))return false;
  auto&o=p.observation;o.context.width=cd.Width;o.context.height=cd.Height;o.colorFormat=uint32_t(cd.Format);o.colorSamples=unsigned(cd.MultiSampleType);
  if(!p.noDepth){if(FAILED(p.native.depth->GetDesc(&dd)))return false;o.depthWidth=dd.Width;o.depthHeight=dd.Height;o.depthFormat=uint32_t(dd.Format);o.depthSamples=unsigned(dd.MultiSampleType);}
  o.targetsKnown=!p.noDepth&&cd.MultiSampleQuality==0&&dd.MultiSampleQuality==0;return true;
 }
 bool ReadTargetsUnbound(IDirect3DDevice9*d,CapturedObservation&p,bool requireEmptyAux){
  bool ok=true;
  for(unsigned slot=0;slot<mrtSlots_;++slot){IDirect3DSurface9*s=nullptr;const HRESULT hr=d->GetRenderTarget(slot,&s);
   if(hr==D3DERR_NOTFOUND&&!s&&slot)continue;
   if(FAILED(hr)||!s){DropReference(s);return false;}
   IUnknown*id=Canonical(s);if(!id||runtime_.StorageForAdapter().OwnsSurface(id)||(requireEmptyAux&&slot))ok=false;
   DropReference(id);DropReference(s);
  }
  p.observation.privateTargetsKnownUnbound=ok;return ok;
 }
 void ReadArea(IDirect3DDevice9*d,DWORD count,const D3DRECT*rects,CapturedObservation&p){
  auto&a=p.clear.area;a.width=p.observation.context.width;a.height=p.observation.context.height;
  const bool vp=SUCCEEDED(d->GetViewport(&p.viewport));
  a.viewportKnown=vp&&uint64_t(p.viewport.X)+p.viewport.Width<=INT32_MAX&&uint64_t(p.viewport.Y)+p.viewport.Height<=INT32_MAX;
  if(a.viewportKnown)a.viewport={int32_t(p.viewport.X),int32_t(p.viewport.Y),int32_t(p.viewport.X+p.viewport.Width),int32_t(p.viewport.Y+p.viewport.Height)};
  DWORD enabled=0;RECT scissor{};const HRESULT state=d->GetRenderState(D3DRS_SCISSORTESTENABLE,&enabled),rect=d->GetScissorRect(&scissor);
  a.scissorKnown=SUCCEEDED(state)&&(!enabled||SUCCEEDED(rect));a.scissorEnabled=enabled!=0;a.scissor={scissor.left,scissor.top,scissor.right,scissor.bottom};
  a.count=count;a.pointerWasNull=rects==nullptr;a.rectsCopied=!count&&!rects;
  if(count&&count<=WosRsmClear::MaxRects&&rects){D3DRECT raw[WosRsmClear::MaxRects]{};
   a.rectsCopied=engine_.Read(reinterpret_cast<uintptr_t>(rects),raw,size_t(count)*sizeof(D3DRECT));
   if(a.rectsCopied)for(unsigned i=0;i<count;++i)a.rects[i]={raw[i].x1,raw[i].y1,raw[i].x2,raw[i].y2};
  }
 }
public:
 ObservationBridge()=default;ObservationBridge(const ObservationBridge&)=delete;ObservationBridge&operator=(const ObservationBridge&)=delete;
 Engine& EngineForAdapter(){return engine_;}D3D9Lifecycle& Runtime(){return runtime_;}
 bool Active()const{return runtime_.Active();}bool Verified()const{return verified_;}
 uint64_t FrameSerial()const{return Active()?frameSerial_:0;}
 void EnableFlux(bool enabled)noexcept{runtime_.EnableFlux(enabled);}
 void Reset()noexcept{runtime_.Reset();deviceKey_=0;mrtSlots_=0;}
 void EndFrame()noexcept{runtime_.EndFrame();}
 // Wire immediately after original Present and before optional F7 Begin.
 // Existing frameEnd finishes capture/readback BEFORE original Present.
 void AfterPresent(IDirect3DDevice9*d,HRESULT nativeResult,bool enabled)noexcept{
  try{
   if(!enabled||FAILED(nativeResult)||!d){Reset();return;}
   if(!verificationTried_){verificationTried_=true;verified_=engine_.VerifyImage();}
   if(!verified_||engine_.ImageAddress()!=ImageBase||!engine_.VerifyLoaded()){verified_=false;Reset();return;}
   IUnknown*id=Canonical(d);D3DCAPS9 caps{};
   const bool good=id&&SUCCEEDED(d->GetDeviceCaps(&caps))&&caps.NumSimultaneousRTs>=2&&caps.NumSimultaneousRTs<=4;
   const uint64_t key=IdentityValue(id);DropReference(id);
   if(!good||frameSerial_==UINT64_MAX){Reset();return;}
   deviceKey_=key;mrtSlots_=caps.NumSimultaneousRTs;runtime_.BeginFrame(++frameSerial_,key,true,true);
  }catch(...){verified_=false;Reset();}
 }
 bool ObserveClear(IDirect3DDevice9*d,DWORD count,const D3DRECT*rects,DWORD flags,float z,uintptr_t caller,CapturedObservation&p)noexcept{
  if(!Active())return false;
  if(p.frameSerial){runtime_.ObservationFailed();return false;}p.frameSerial=frameSerial_;
  try{
   p.clear.flags=flags;p.clear.depthValue=z;
   if(!d||!DeviceAndDepth(d,p)||!ColorAndDescriptors(d,p)){runtime_.ObservationFailed(p.observation.context.key);return false;}
   ReadTargetsUnbound(d,p,false);ReadArea(d,count,rects,p);EngineObservation(caller,true,p);return true;
  }catch(...){runtime_.ObservationFailed();return false;}
 }
 void CompleteClear(CapturedObservation&p,HRESULT nativeResult)noexcept{
  try{if(p.consumed||p.frameSerial!=frameSerial_||!Active()){runtime_.ObservationFailed();return;}p.consumed=true;p.clear.nativeSucceeded=SUCCEEDED(nativeResult);runtime_.OnNativeClear(p.observation,p.clear,p.native);}
  catch(...){runtime_.ObservationFailed();}
 }
 DrawObservationResult ObserveDraw(IDirect3DDevice9*d,CapturedObservation&p)noexcept{
  if(!Active())return DrawObservationResult::Inactive;
  if(p.frameSerial){runtime_.ObservationFailed();return DrawObservationResult::Failed;}p.frameSerial=frameSerial_;
  try{
   DWORD z=0,write=0;
   if(!d||FAILED(d->GetRenderState(D3DRS_ZENABLE,&z))||FAILED(d->GetRenderState(D3DRS_ZWRITEENABLE,&write))){runtime_.ObservationFailed();return DrawObservationResult::Failed;}
   if(!z||!write)return DrawObservationResult::NoDepthWrite;
   if(!DeviceAndDepth(d,p)){runtime_.ObservationFailed();return DrawObservationResult::Failed;}
   if(p.noDepth)return DrawObservationResult::NoDepthWrite;
   if(!Tracked(p.observation.context.key))return DrawObservationResult::UntrackedDepth;
   if(!ColorAndDescriptors(d,p)){runtime_.ObservationFailed(p.observation.context.key);return DrawObservationResult::Failed;}
   EngineObservation(0,false,p);ReadArea(d,0,nullptr,p);DWORD blend=0,srgb=0,stencil=0;
   p.stateCompatible=z==D3DZB_TRUE&&SUCCEEDED(d->GetRenderState(D3DRS_ALPHABLENDENABLE,&blend))&&!blend&&
    SUCCEEDED(d->GetRenderState(D3DRS_SRGBWRITEENABLE,&srgb))&&!srgb&&SUCCEEDED(d->GetRenderState(D3DRS_STENCILENABLE,&stencil))&&!stencil&&
    p.clear.area.viewportKnown&&p.viewport.X==0&&p.viewport.Y==0&&p.viewport.Width==ShadowSize&&p.viewport.Height==ShadowSize&&p.viewport.MinZ==0&&p.viewport.MaxZ==1&&
    p.clear.area.scissorKnown&&ReadTargetsUnbound(d,p,true);
   return DrawObservationResult::Observed;
  }catch(...){runtime_.ObservationFailed();return DrawObservationResult::Failed;}
 }
 bool BeginWriter(CapturedObservation&p,Route route,WosRsmClear::WriteToken&token,bool fluxEmitter=false)noexcept{
  try{if(p.consumed||p.frameSerial!=frameSerial_||!Active()){runtime_.ObservationFailed();token={};return false;}p.consumed=true;return runtime_.BeginWriter(p.observation,route,p.stateCompatible,token,fluxEmitter);}catch(...){runtime_.ObservationFailed();token={};return false;}
 }
 class NativeClearScope {
  ObservationBridge&bridge_;CapturedObservation packet_{};bool observed_=false,completed_=false;
 public:
  NativeClearScope(ObservationBridge&b,IDirect3DDevice9*d,DWORD count,const D3DRECT*rects,DWORD flags,float z,uintptr_t caller)noexcept:bridge_(b){observed_=b.ObserveClear(d,count,rects,flags,z,caller,packet_);}
  void Complete(HRESULT nativeResult)noexcept{if(completed_){bridge_.runtime_.ObservationFailed();return;}completed_=true;if(observed_)bridge_.CompleteClear(packet_,nativeResult);}
  ~NativeClearScope(){if(observed_&&!completed_)bridge_.runtime_.ObservationFailed();}
 };
 template<class F>HRESULT InvokeClear(F&&original,IDirect3DDevice9*d,DWORD count,const D3DRECT*rects,DWORD flags,D3DCOLOR color,float z,DWORD stencil,uintptr_t caller){
  NativeClearScope scope(*this,d,count,rects,flags,z,caller);
  const HRESULT hr=original(d,count,rects,flags,color,z,stencil); // Outside all probe catch blocks, exactly once.
  scope.Complete(hr);return hr;
 }
};
}
