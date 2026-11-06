#pragma once
// Staged production-usable single native draw writer. No F7, IDs, readbacks,
// engine calls, replay, logging or per-draw file I/O. See WRITER_CONTRACT.md.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_Deferred/rsm_flux_mrt_staged/shader_assets.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_Deferred/rsm_flux_mrt_staged/static_geometry.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_Deferred/rsm_runtime_staged/observation_bridge.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_Deferred/rsm_material_packet_staged/d3d9_packet_adapter.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_Deferred/rsm_vertex_bridge_staged/vertex_bridge.h"
#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_Deferred/rsm_transport_staged/rsm_projection.h"
#pragma pop_macro("max")
#pragma pop_macro("min")

namespace WosRsmWriter {
using Route=WosRsmRuntime::Route;
using Family=WosStagedMaterialPacket::Family;
using MaterialScope=WosStagedMaterialPacket::Scope;
using BindPlan=WosStagedPacketD3D9::BindPlan;
using MaterialBindings=WosStagedPacketD3D9::BindScope;
enum class Native {Unknown,VS2,VS3,VS8,VS5,VS7,PS1,PS4,PS6};
inline Route Pair(Native vs,Native ps){
 if(ps==Native::PS1){if(vs==Native::VS2)return Route::NormalVS2;if(vs==Native::VS3)return Route::NormalVS3;if(vs==Native::VS8)return Route::NormalVS8;}
 if(ps==Native::PS4&&vs==Native::VS5)return Route::FoliagePS4VS5;
 if(ps==Native::PS6&&vs==Native::VS5)return Route::FoliagePS6VS5;
 if(ps==Native::PS6&&vs==Native::VS7)return Route::FoliagePS6VS7;
 return Route::Unknown;
}
template<size_t N>inline bool Exact(const void*bytes,size_t size,const uint32_t(&expected)[N]){
 return bytes&&size==sizeof(expected)&&!std::memcmp(bytes,expected,size);
}
inline Native Classify(const void*bytes,size_t size,bool vertex){
 using namespace WosRsmWriterAssets;
 if(vertex){
  if(Exact(bytes,size,vs_0002_input))return Native::VS2;
  if(Exact(bytes,size,vs_0003_input))return Native::VS3;
  if(Exact(bytes,size,vs_0008_input))return Native::VS8;
  if(Exact(bytes,size,vs_0005_input))return Native::VS5;
  if(Exact(bytes,size,vs_0007_input))return Native::VS7;
 }else{
  if(Exact(bytes,size,ps_0001_input))return Native::PS1;
  if(Exact(bytes,size,ps_0004_input))return Native::PS4;
  if(Exact(bytes,size,ps_0006_input))return Native::PS6;
 }return Native::Unknown;
}
// Same verified orthographic convention as WosRsmOrigin::ConfirmTowardSun.
// A per-instance environment facing another sun cannot use this map's area.
inline bool SunAligned(const float*vp,const float*sun){
 using namespace WosRsmProjection;
 if(!sun)return false;Vec3 direction{sun[0],sun[1],sun[2]};
 if(!Finite(direction)||!std::isfinite(sun[3])||sun[3]!=0)return false;
 const double length=Length(direction);if(!(length>.5&&length<2))return false;
 Projection light;if(!Decode(vp,2048,2048,{},light))return false;
 return Dot(Scale(direction,1/length),light.depthRay)<=-1+2e-5;
}
// Exact v11 evidence: 197 VS2 shadow draws (91 near,106 far) have these
// c144..147 rows bit-identical to transpose(sceneVP). Native VS2 uses them
// directly for clip-position dp4; a changed upload invalidates map coordinates.
inline bool VS2Projection(const float*rows,const float*vp){
 if(!rows||!vp)return false;
 for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c){
  if(!std::isfinite(rows[r*4+c])||!std::isfinite(vp[c*4+r])||std::memcmp(rows+r*4+c,vp+c*4+r,4))return false;
 }return true;
}
inline bool EmitterPair(Route route,Family family){
 return (route==Route::NormalVS2&&(family==Family::Old69||family==Family::Old72))||
  (route==Route::NormalVS3&&family==Family::Static111);
}
struct Statistics {uint64_t observed=0,emit69=0,emit72=0,emit111=0,normal=0,foliage=0,unknown=0,planRejected=0,bindingFailures=0,restoreFailures=0,strideSkipped=0,strideQueryFailures=0;};
// Necessary live layout condition only. A positive result grants no material
// trust: final declaration, source identities, current scope and projection stay
// checked at Execute. Failed queries keep the normal/zero-flux writer active.
template<class Device,class Buffer>inline bool PacketStrideCandidate(Device*d,Route route,Statistics&stats){
 if(route!=Route::NormalVS3)return true;
 Buffer*vb=nullptr;UINT offset=0,stride=0;
 const HRESULT hr=d?d->GetStreamSource(0,&vb,&offset,&stride):E_POINTER;
 const bool queried=SUCCEEDED(hr)&&vb!=nullptr;
 if(vb)vb->Release();
 if(!queried){++stats.strideQueryFailures;return false;}
 if(stride!=20){++stats.strideSkipped;return false;}
 return true;
}
template<class T>inline void Release(T*&p){if(p)p->Release();p=nullptr;}

class Assets {
 struct Entry {IUnknown*object=nullptr;bool vertex=false;Native id=Native::Unknown;};
 IDirect3DDevice9*device_=nullptr;std::vector<Entry> cache_;bool tried_=false,ready_=false;
 IDirect3DVertexShader9*vs_[6]{}; // normal2, normal3, normal8, material69, material72, static111
 IDirect3DPixelShader9*ps_[5]{}; // normal/zero-flux, foliage4, foliage6, material69, material72
public:
 static constexpr size_t MaxShaderObjects=512,MaxBytecodeBytes=1024*1024;
 Assets()=default;Assets(const Assets&)=delete;Assets&operator=(const Assets&)=delete;
 ~Assets(){Reset();}
 void Reset(){for(auto&e:cache_)Release(e.object);cache_.clear();for(auto&p:vs_)Release(p);for(auto&p:ps_)Release(p);Release(device_);tried_=ready_=false;}
 bool Ensure(IDirect3DDevice9*d){
  if(d!=device_){Reset();device_=d;if(d)d->AddRef();}if(tried_)return ready_;tried_=true;if(!d)return false;
  D3DCAPS9 c{};if(FAILED(d->GetDeviceCaps(&c))||c.NumSimultaneousRTs<3||c.VertexShaderVersion<D3DVS_VERSION(3,0)||c.PixelShaderVersion<D3DPS_VERSION(3,0)||
   !(c.PrimitiveMiscCaps&D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS)||!(c.PrimitiveMiscCaps&D3DPMISCCAPS_INDEPENDENTWRITEMASKS))return false;
  using namespace WosRsmWriterAssets;
  const uint32_t*vertex[]={vs_0002_mrt,vs_0003_mrt,vs_0008_mrt,vs_0002_material69_mrt,vs_0002_material72_mrt};
  const uint32_t*pixel[]={ps_0001_normal_guard_mrt,ps_0004_guard,ps_0006_guard,ps_0001_material69_mrt,ps_0001_material72_mrt};
  for(unsigned i=0;i<5;++i)if(FAILED(d->CreateVertexShader(reinterpret_cast<const DWORD*>(vertex[i]),&vs_[i]))||FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(pixel[i]),&ps_[i])))return false;
  if(FAILED(d->CreateVertexShader(reinterpret_cast<const DWORD*>(vs_0003_material111_mrt),&vs_[5])))return false;
  cache_.reserve(MaxShaderObjects);ready_=true;return true;
 }
 template<class T>Native Identify(T*object,bool vertex){
  if(!object)return Native::Unknown;
  for(const auto&e:cache_)if(e.object==static_cast<IUnknown*>(object)&&e.vertex==vertex)return e.id;
  if(cache_.size()>=MaxShaderObjects)return Native::Unknown;
  Native result=Native::Unknown;UINT bytes=0;
  if(SUCCEEDED(object->GetFunction(nullptr,&bytes))&&bytes>=8&&bytes<=MaxBytecodeBytes&&!(bytes&3)){
   std::vector<unsigned char>code(bytes);UINT actual=bytes;
   if(SUCCEEDED(object->GetFunction(code.data(),&actual))&&actual==bytes)result=Classify(code.data(),code.size(),vertex);
  }
  object->AddRef();cache_.push_back({object,vertex,result});return result;
 }
 bool Select(Route route,bool emitter,Family family,IDirect3DVertexShader9*&vs,IDirect3DPixelShader9*&ps){
  vs=nullptr;ps=nullptr;if(!ready_)return false;
  if(emitter){if(!EmitterPair(route,family))return false;if(family==Family::Static111){vs=vs_[5];ps=ps_[4];return true;}unsigned i=family==Family::Old69?3:4;vs=vs_[i];ps=ps_[i];return true;}
  if(route==Route::NormalVS2){vs=vs_[0];ps=ps_[0];}
  else if(route==Route::NormalVS3){vs=vs_[1];ps=ps_[0];}
  else if(route==Route::NormalVS8){vs=vs_[2];ps=ps_[0];}
  else if(route==Route::FoliagePS4VS5)ps=ps_[1];
  else if(route==Route::FoliagePS6VS5||route==Route::FoliagePS6VS7)ps=ps_[2];
  return ps!=nullptr;
 }
};
inline bool EmitterDeclaration(IDirect3DDevice9*d,Family family){
 if(!d||(family!=Family::Old69&&family!=Family::Old72&&family!=Family::Static111))return false;
 IDirect3DVertexDeclaration9*decl=nullptr;IDirect3DVertexBuffer9*vb=nullptr;UINT count=MAXD3DDECLLENGTH+1,offset=0,stride=0;
 D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH+1]{};
 const bool read=SUCCEEDED(d->GetVertexDeclaration(&decl))&&decl&&SUCCEEDED(decl->GetDeclaration(elements,&count))&&
  SUCCEEDED(d->GetStreamSource(0,&vb,&offset,&stride))&&vb;
 const bool valid=read&&(family==Family::Static111?WosStatic111::Declaration(elements,count,stride):
  WosVertexBridge::Declaration(family==Family::Old69?WosVertexBridge::Family::Tint69:WosVertexBridge::Family::Transform72,
  reinterpret_cast<const WosVertexBridge::Element*>(elements),count,stride));
 Release(vb);Release(decl);return valid;
}

// Construct BEFORE temporary patches: observation and held shader refs only.
// Execute binds at the last moment after a fresh state/context check. If
// OwnsObservation() is true, skip legacy F7 geometry shader patching. Read-only
// capture may continue. The prepared plan must remain alive and unmoved.
template<class Bridge>class DrawScope {
 Bridge&bridge_;Assets&assets_;Statistics&stats_;IDirect3DDevice9*device_=nullptr;
 WosRsmRuntime::CapturedObservation original_,observed_;WosRsmClear::WriteToken token_{};
 const BindPlan*plan_=nullptr;const MaterialScope*scopeInput_=nullptr;MaterialScope materialScope_{};bool inspectOK_=false;
 IDirect3DVertexShader9*originalVS_=nullptr;IDirect3DPixelShader9*originalPS_=nullptr;
 IDirect3DSurface9*originalRT_[2]{};DWORD originalMask_[2]{};D3DVIEWPORT9 originalViewport_{};
 std::unique_ptr<MaterialBindings> material_;
 bool owns_=false,tokenStarted_=false,saved_=false,touched_=false,bound_=false,executed_=false,restored_=false,restoreOK_=true;
 bool emitter_=false;Family family_=Family::Old69;Route route_=Route::Unknown;
 bool Save(){
  if(FAILED(device_->GetViewport(&originalViewport_)))return false;
  for(unsigned i=0;i<2;++i){HRESULT h=device_->GetRenderTarget(i+1,&originalRT_[i]);
   if(h!=D3DERR_NOTFOUND||originalRT_[i])return false;
   if(FAILED(device_->GetRenderState(i?D3DRS_COLORWRITEENABLE2:D3DRS_COLORWRITEENABLE1,&originalMask_[i])))return false;
  }saved_=true;return true;
 }
 bool Restore(){
  if(restored_)return restoreOK_;restored_=true;bool ok=true;
  if(material_)ok=material_->Restore()&&material_->RestoreSucceeded();
  if(touched_){
   if(FAILED(device_->SetVertexShader(originalVS_)))ok=false;
   if(FAILED(device_->SetPixelShader(originalPS_)))ok=false;
   for(unsigned i=0;i<2;++i){
    if(FAILED(device_->SetRenderTarget(i+1,originalRT_[i])))ok=false;
    if(FAILED(device_->SetRenderState(i?D3DRS_COLORWRITEENABLE2:D3DRS_COLORWRITEENABLE1,originalMask_[i])))ok=false;
   }
   if(FAILED(device_->SetViewport(&originalViewport_)))ok=false;
  }restoreOK_=ok;if(!ok)++stats_.restoreFailures;return ok;
 }
 bool CurrentPlan(const BindPlan&plan,const MaterialScope&scope){
  if(!EmitterPair(route_,plan.family)||!plan.ValidFor(scope)||scope.scene!=observed_.observation.context.scene||observed_.frameSerial!=bridge_.FrameSerial())return false;
  uint32_t scene=0,frame=0;auto&engine=bridge_.EngineForAdapter();
  return engine.Read(0x01115c54,&scene,4)&&engine.Read(0x011184f4,&frame,4)&&scene==scope.scene&&frame==scope.frame&&
   SunAligned(observed_.observation.context.vp,plan.SunDirection())&&EmitterDeclaration(device_,plan.family)&&
   (plan.family!=Family::Static111||WosStatic111::CurrentProjection(engine,device_,scope.scene,scope.frame,scope.instance,observed_.observation.context.vp));
 }
 void Inspect(){
  using WosRsmRuntime::DrawObservationResult;
  const auto result=bridge_.ObserveDraw(device_,original_);
  owns_=result==DrawObservationResult::Observed||result==DrawObservationResult::Failed;
  if(result!=DrawObservationResult::Observed)return;
  inspectOK_=true;
  if(!bridge_.Runtime().FluxEnabled()||!assets_.Ensure(device_)||FAILED(device_->GetVertexShader(&originalVS_))||FAILED(device_->GetPixelShader(&originalPS_)))return;
  route_=Pair(assets_.Identify(originalVS_,true),assets_.Identify(originalPS_,false));
 }
 void Start(){
  using WosRsmRuntime::DrawObservationResult;
  if(!inspectOK_){
   // A legacy scope may enable depth writes or bind a previously untracked DS.
   // No original-route proof was retained for that transition: invalidate it.
   if(bridge_.Active()&&bridge_.ObserveDraw(device_,observed_)==DrawObservationResult::Observed){
    ++stats_.observed;++stats_.unknown;bridge_.BeginWriter(observed_,Route::Unknown,token_);
   }return;
  }
  const auto result=bridge_.ObserveDraw(device_,observed_);
  if(result!=DrawObservationResult::Observed){
   if(result!=DrawObservationResult::NoDepthWrite)bridge_.Runtime().ObservationFailed(original_.observation.context.key);
   return;
  }++stats_.observed;
  const auto&a=original_.observation.context;const auto&b=observed_.observation.context;
  IDirect3DVertexShader9*vs=nullptr;IDirect3DPixelShader9*ps=nullptr;
  const bool shaderSame=SUCCEEDED(device_->GetVertexShader(&vs))&&SUCCEEDED(device_->GetPixelShader(&ps))&&vs==originalVS_&&ps==originalPS_;
  Release(vs);Release(ps);
  if(!shaderSame||original_.frameSerial!=observed_.frameSerial||!(a.key==b.key)||a.color!=b.color||a.scene!=b.scene||std::memcmp(a.vp,b.vp,sizeof(a.vp))){
   bridge_.BeginWriter(observed_,Route::Unknown,token_);bridge_.Runtime().ObservationFailed(a.key);++stats_.unknown;return;
  }
  const BindPlan*plan=plan_;if(scopeInput_)materialScope_=*scopeInput_;const MaterialScope*scope=scopeInput_?&materialScope_:nullptr;
  if(route_==Route::Unknown){bridge_.BeginWriter(observed_,route_,token_);++stats_.unknown;return;}
  if(route_==Route::NormalVS2){float rows[16]{};
   if(FAILED(device_->GetVertexShaderConstantF(144,rows,4))||!VS2Projection(rows,b.vp)){
    bridge_.BeginWriter(observed_,Route::Unknown,token_);++stats_.bindingFailures;return;
   }
  }
  if(!observed_.stateCompatible||!Save()){bridge_.BeginWriter(observed_,Route::Unknown,token_);++stats_.bindingFailures;return;}
  if(plan&&scope&&EmitterPair(route_,plan->family)&&CurrentPlan(*plan,*scope)){
   family_=plan->family;material_=std::make_unique<MaterialBindings>(device_,*plan,*scope);
   if(material_->Ready())emitter_=true;
   else if(!material_->Restore()||!material_->RestoreSucceeded()){
    restoreOK_=false;restored_=true;++stats_.restoreFailures;bridge_.Runtime().ObservationFailed(observed_.observation.context.key);return;
   }else material_.reset();
  }
  if(plan&&!emitter_)++stats_.planRejected;
  vs=nullptr;ps=nullptr;
  if(!assets_.Select(route_,emitter_,family_,vs,ps)){Restore();bridge_.BeginWriter(observed_,Route::Unknown,token_);return;}
  if(!bridge_.BeginWriter(observed_,route_,token_,emitter_)){Restore();return;}tokenStarted_=true;
  const auto view=bridge_.Runtime().StorageForAdapter().GetView(observed_.observation.context.slot);
  if(!view.normalSurface||!view.fluxSurface){++stats_.bindingFailures;Restore();return;}
  touched_=true;
  const bool ok=SUCCEEDED(device_->SetRenderTarget(1,view.normalSurface))&&SUCCEEDED(device_->SetRenderTarget(2,view.fluxSurface))&&
   SUCCEEDED(device_->SetRenderState(D3DRS_COLORWRITEENABLE1,15))&&SUCCEEDED(device_->SetRenderState(D3DRS_COLORWRITEENABLE2,15))&&
   SUCCEEDED(device_->SetVertexShader(vs?vs:originalVS_))&&SUCCEEDED(device_->SetPixelShader(ps))&&SUCCEEDED(device_->SetViewport(&originalViewport_));
  if(!ok){++stats_.bindingFailures;Restore();return;}bound_=true;
 }
public:
 DrawScope(Bridge&bridge,Assets&assets,Statistics&statistics,IDirect3DDevice9*device,const BindPlan*plan=nullptr,const MaterialScope*scope=nullptr)
  :bridge_(bridge),assets_(assets),stats_(statistics),device_(device),plan_(plan),scopeInput_(scope){
  try{Inspect();}catch(...){inspectOK_=false;++stats_.bindingFailures;bridge_.Runtime().ObservationFailed(original_.observation.context.key);}
 }
 DrawScope(const DrawScope&)=delete;DrawScope&operator=(const DrawScope&)=delete;
 ~DrawScope(){
  Restore();if(tokenStarted_&&!executed_)bridge_.Runtime().FinishWriter(token_,false,false,restoreOK_);
  Release(originalVS_);Release(originalPS_);for(auto&p:originalRT_)Release(p);
 }
 bool CanEmitMaterial()const{
  return inspectOK_&&original_.stateCompatible&&(route_==Route::NormalVS2||route_==Route::NormalVS3)&&
   PacketStrideCandidate<IDirect3DDevice9,IDirect3DVertexBuffer9>(device_,route_,stats_);
 }
 bool OwnsObservation()const{return owns_;}bool Patched()const{return bound_;}bool Emitter()const{return bound_&&emitter_;}
 template<class Draw>HRESULT Execute(Draw&&nativeDraw){
  // A repeated Execute must not issue an extra native draw.
  if(executed_){bridge_.Runtime().ObservationFailed(observed_.observation.context.key);return D3DERR_INVALIDCALL;}
  executed_=true;HRESULT nativeResult;
  try{Start();}catch(...){bound_=false;++stats_.bindingFailures;Restore();bridge_.Runtime().ObservationFailed(original_.observation.context.key);}
  try{nativeResult=nativeDraw();}catch(...){Restore();if(tokenStarted_)bridge_.Runtime().FinishWriter(token_,false,false,restoreOK_);throw;}
  const bool restored=Restore();
  if(tokenStarted_){
   const bool complete=bridge_.Runtime().FinishWriter(token_,SUCCEEDED(nativeResult),bound_,restored);
   if(complete){if(emitter_){if(family_==Family::Old69)++stats_.emit69;else if(family_==Family::Old72)++stats_.emit72;else if(family_==Family::Static111)++stats_.emit111;}
    else if(WosRsmRuntime::Geometric(route_))++stats_.normal;else ++stats_.foliage;}
  }return nativeResult;
 }
};
}
