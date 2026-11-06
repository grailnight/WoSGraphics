#pragma once
// Current native shadow maps -> geometric normal + diffuse sun source -> GI.
// Include after native_indirect/native_material_context/rsm_geometry_capture.
#include <optional>
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/rsm_runtime_staged/production_engine_reader.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/rsm_material_packet_staged/packet_current_draw.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/rsm_flux_mrt_staged/runtime_writer.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/rsm_gather_runtime_staged/d3d9_renderer.h"
namespace WosNativeRSM {
static WosRsmRuntime::ProductionBridge bridge;
static WosStagedCurrentPacket::Gates gates;
static WosStagedPacketD3D9::Registry registry;
static WosRsmWriter::Assets assets;
static WosRsmWriter::Statistics writers;
static WosRsmGather::D3D9Renderer renderer;
static bool inside=false,identityTried=false;
static uint64_t drawSerial=0,packetAttempts=0,packets=0,packetReads=0,packetBytes=0;
static uint64_t packetStatus[24]={};
static ULONGLONG lastStatus=0;
static const char*lastPacketReason="inactive";
static std::string capturedFolder;
struct Guard {bool was=inside;Guard(){inside=true;}~Guard(){inside=was;}};
static bool Suppressed(){return inside||WosProbe::suppress||WosIndirect::busy;}
static void RegisterPS(IDirect3DDevice9*d,IDirect3DPixelShader9*p){if(Suppressed())return;try{registry.RegisterPixelShader(d,p);}catch(...){}}
static void RegisterVS(IDirect3DDevice9*d,IDirect3DVertexShader9*p){if(Suppressed())return;try{registry.RegisterVertexShader(d,p);}catch(...){}}
static void RegisterTexture(IDirect3DDevice9*d,IDirect3DBaseTexture9*p){if(Suppressed())return;try{registry.RegisterTexture(d,p);}catch(...){}}
static void Initialize(IDirect3DDevice9*d){Guard guard;registry.Initialize(d);if(!identityTried){identityTried=true;gates.Initialize();}}
static void Reset(){Guard guard;bridge.Reset();gates.Reset();assets.Reset();renderer.Reset();registry.Reset();writers={};drawSerial=packetAttempts=packets=packetReads=packetBytes=0;std::memset(packetStatus,0,sizeof(packetStatus));capturedFolder.clear();lastStatus=0;lastPacketReason="inactive";}
static void AfterPresent(IDirect3DDevice9*d,HRESULT hr){Guard guard;
 registry.Initialize(d);if(!identityTried){identityTried=true;gates.Initialize();}
 bridge.EnableFlux(true);bridge.AfterPresent(d,hr,WosIndirect::enabled);drawSerial=0;
 if(bridge.Active())gates.BeginFrame(bridge.FrameSerial());
}
static bool Render(IDirect3DDevice9*d,IDirect3DBaseTexture9*source,IDirect3DBaseTexture9*depth,const WosIndirectCamera::Camera&camera,IDirect3DTexture9*normal,IDirect3DTexture9*rho){
 Guard guard;try{return renderer.Render(bridge.Runtime(),d,source,depth,camera,normal,rho);}catch(...){bridge.Runtime().ObservationFailed();return false;}
}
class DrawScope {
 WosStagedCurrentPacket::Prepared packet_;
 using Writer=WosRsmWriter::DrawScope<WosRsmRuntime::ProductionBridge>;
 std::optional<Writer> writer_;
public:
 DrawScope(IDirect3DDevice9*d,WosNativeMaterialContext::Kind kind=WosNativeMaterialContext::Kind::Unsupported,
  uintptr_t caller=0,uintptr_t returnSlot=0,const WosNativeMaterialContext::DrawArgs&args={}){
  if(Suppressed()||!bridge.Active())return;
  try{Guard guard;const uint64_t serial=++drawSerial;
   writer_.emplace(bridge,assets,writers,d,&packet_.plan,&packet_.scope);
   if(kind!=WosNativeMaterialContext::Kind::Unsupported&&writer_->CanEmitMaterial()){
    ++packetAttempts;const auto*tib=reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    WosNativeMaterialContext::Bounds stack{reinterpret_cast<uintptr_t>(tib->StackLimit),reinterpret_cast<uintptr_t>(tib->StackBase)};
    const bool ready=WosStagedCurrentPacket::Build(d,registry,gates,kind,uint32_t(caller),uint32_t(returnSlot),args,stack,bridge.FrameSerial(),serial,packet_);
    packetReads+=packet_.readerCalls;packetBytes+=packet_.readerBytes;lastPacketReason=packet_.reason;
    const unsigned status=unsigned(packet_.diagnostic.status);if(status<24)++packetStatus[status];if(ready)++packets;
   }
  }catch(...){bridge.Runtime().ObservationFailed();}
 }
 bool OwnsObservation()const{return writer_&&writer_->OwnsObservation();}
 template<class F>HRESULT Execute(F&&original){if(!writer_)return original();Guard guard;return writer_->Execute(std::forward<F>(original));}
};
// Copy only on F7. INTZ is sampled into private R32F; all game state is restored.
static IDirect3DTexture9*CopyShadowDepth(IDirect3DDevice9*d,IDirect3DTexture9*source,unsigned w,unsigned h){
 if(!d||!source)return nullptr;
 if(!WosIndirect::depthCopyPS){auto bytes=WosCascadeProbe::ReadFile(WosProbe::Root()+"\\indirect\\indirect_depth_copy.cso");if(bytes.empty()||FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(bytes.data()),&WosIndirect::depthCopyPS)))return nullptr;}
 IDirect3DTexture9*t=nullptr;IDirect3DSurface9*rt=nullptr;
 if(FAILED(d->CreateTexture(w,h,1,D3DUSAGE_RENDERTARGET,D3DFMT_R32F,D3DPOOL_DEFAULT,&t,nullptr)))return nullptr;
 if(FAILED(t->GetSurfaceLevel(0,&rt))){t->Release();return nullptr;}
 WosIndirect::State state(d);bool ok=state.valid;auto check=[&](HRESULT hr){ok=SUCCEEDED(hr)&&ok;};
 if(ok){for(unsigned i=0;i<16;++i)check(d->SetTexture(i,nullptr));check(d->SetDepthStencilSurface(nullptr));for(unsigned i=1;i<state.slots;++i)check(d->SetRenderTarget(i,nullptr));
  check(d->SetRenderTarget(0,rt));check(d->SetVertexShader(nullptr));check(d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1));
  for(auto rs:{D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHABLENDENABLE,D3DRS_ALPHATESTENABLE,D3DRS_STENCILENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_SRGBWRITEENABLE,D3DRS_FOGENABLE,D3DRS_CLIPPLANEENABLE,D3DRS_SEPARATEALPHABLENDENABLE})check(d->SetRenderState(rs,0));
  check(d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));check(d->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID));check(d->SetRenderState(D3DRS_COLORWRITEENABLE,15));check(d->SetRenderState(D3DRS_MULTISAMPLEMASK,0xffffffff));
  check(d->SetTexture(0,source));for(auto ss:{D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER})check(d->SetSamplerState(0,ss,D3DTEXF_POINT));
  check(d->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_NONE));check(d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));check(d->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));check(d->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,0));
  check(g_original_set_pixel_shader(d,WosIndirect::depthCopyPS));if(ok){const HRESULT begin=g_original_begin_scene(d);if(SUCCEEDED(begin)){const bool drawn=WosIndirect::Quad(d,w,h);const HRESULT end=g_original_end_scene(d);ok=drawn&&SUCCEEDED(end);}else ok=false;}ok=state.Restore()&&ok;
 }
 rt->Release();if(!ok){t->Release();return nullptr;}return t;
}
static void Status(FILE*f){
 std::fprintf(f,"\"revision\":18,\"runtime_active\":%s,\"material_gate\":%s,\"host_frame\":%llu,\"observed\":%llu,\"emit69\":%llu,\"emit72\":%llu,\"emit111\":%llu,\"stride_skipped\":%llu,\"stride_query_failures\":%llu,\"normal_guards\":%llu,\"foliage_guards\":%llu,\"unknown\":%llu,\"bind_failures\":%llu,\"restore_failures\":%llu,\"packets\":%llu,\"packet_attempts\":%llu,\"packet_reads\":%llu,\"packet_bytes\":%llu,\"packet_reason\":\"%s\",\"native_gather_frames\":%llu,\"gather_unavailable\":%llu,\"gather_failed\":%llu,\"gather_reason\":\"%s\",\"registered_shaders\":%zu,\"registered_textures\":%zu,\"registered_texture_bytes\":%llu,\"shader_misses\":%llu,\"texture_misses\":%llu,\"packet_status\":[",
  bridge.Active()?"true":"false",gates.ValidFor(bridge.FrameSerial())?"true":"false",bridge.FrameSerial(),writers.observed,writers.emit69,writers.emit72,writers.emit111,writers.strideSkipped,writers.strideQueryFailures,writers.normal,writers.foliage,writers.unknown,writers.bindingFailures,writers.restoreFailures,packets,packetAttempts,packetReads,packetBytes,lastPacketReason,renderer.rendered,renderer.unavailable,renderer.failed,renderer.reason,registry.ShaderCount(),registry.TextureCount(),registry.TextureBytes(),registry.stats.shaderMisses,registry.stats.textureMisses);
 for(unsigned i=0;i<24;++i)std::fprintf(f,"%s%llu",i?",":"",packetStatus[i]);std::fputs("]",f);
 std::fprintf(f,",\"device_identity\":{\"facade\":%llu,\"resource_owner\":%llu,\"stage\":%u},\"device_mismatches\":%llu,\"registration_failures\":%llu,\"texture_evictions\":%llu",registry.FacadeKey(),registry.ResourceOwnerKey(),registry.OwnershipStage(),registry.stats.deviceMismatch,registry.stats.registrationFailure,registry.stats.textureEvictions);
 std::fprintf(f,",\"texture_capacity\":%zu,\"texture_byte_capacity\":%llu",WosStagedPacketD3D9::Registry::TextureCap,WosStagedPacketD3D9::Registry::TextureByteCap);
}
static void SaveCapture(){
 if(!WosProbe::trace||WosProbe::folder.empty()||capturedFolder==WosProbe::folder)return;
 capturedFolder=WosProbe::folder;FILE*f=std::fopen((capturedFolder+"\\native_rsm.json").c_str(),"wb");if(!f)return;
 std::fputs("{",f);Status(f);std::fputs(",\"source_quantity\":\"rho_times_native_effective_sun\",\"source_alpha\":\"emitter_validity\",\"gather_quantity\":\"incoming_diffuse_over_pi_plus_environment\",\"visibility\":\"four_point_light_depth_heightfield\",\"maps\":[",f);
 for(unsigned i=0;i<2;++i){const auto&state=bridge.Runtime().State(i);WosRsmRuntime::D3D9Lifecycle::Lease lease;
  const bool flux=bridge.Runtime().AcquireFluxMap(i,lease);const bool normal=flux||bridge.Runtime().AcquireMap(i,lease);
  std::fprintf(f,"%s{\"slot\":%u,\"flux_valid\":%s,\"normal_valid\":%s,\"reason\":\"%s\",\"scene\":%u,\"generation\":%llu,\"full_clears\":%u,\"normal_draws\":%u,\"guard_draws\":%u,\"vp\":[",i?",":"",i,flux?"true":"false",normal?"true":"false",WosRsmClear::Name(state.reason),state.context.scene,state.generation,state.fullClears,state.normalDraws,state.guardDraws);
  for(unsigned j=0;j<16;++j)std::fprintf(f,"%s%.9g",j?",":"",state.context.vp[j]);std::fprintf(f,"],\"flux_generation\":%llu",flux?lease.fluxStamp.stamp.generation:0ull);
  if(normal){const std::string stem=capturedFolder+"\\native_rsm_"+std::to_string(i);auto&v=lease.resources;
   std::fputs(",",f);WosIndirect::SaveTexture(registry.Device(),v.normal,stem+"_normal.raw",f,"normal");
   std::fputs(",",f);WosIndirect::SaveTexture(registry.Device(),v.flux,stem+"_flux.raw",f,"flux");
   IDirect3DTexture9*depth=CopyShadowDepth(registry.Device(),v.depthTexture,lease.context.width,lease.context.height);
   std::fputs(",",f);WosIndirect::SaveTexture(registry.Device(),depth,stem+"_depth.raw",f,"depth");if(depth)depth->Release();
  }std::fputs("}",f);
 }std::fputs("]}\n",f);std::fclose(f);
}
static void FrameEnd(){Guard guard;SaveCapture();const ULONGLONG now=GetTickCount64();if(now-lastStatus>=1000&&WosIndirect::enabled){lastStatus=now;FILE*f=std::fopen((WosProbe::Root()+"\\native_rsm_status.json").c_str(),"wb");if(f){std::fputs("{",f);Status(f);std::fputs("}\n",f);std::fclose(f);}}bridge.EndFrame();}
}
