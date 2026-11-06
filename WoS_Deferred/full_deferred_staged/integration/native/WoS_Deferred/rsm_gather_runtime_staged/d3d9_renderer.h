#pragma once
// Include after production native_indirect.h and the persistent lifecycle.
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_Deferred/rsm_gather_runtime_staged/gather_constants.h"
namespace WosRsmGather {
class D3D9Renderer {
 IDirect3DDevice9*owner_=nullptr;IDirect3DPixelShader9*gather_=nullptr,*composite_=nullptr;
 bool attempted_=false;
 template<class T>static void Release(T*&p){if(p)p->Release();p=nullptr;}
 bool Load(IDirect3DDevice9*d){
  if(owner_!=d){Reset();owner_=d;owner_->AddRef();}
  if(attempted_)return gather_&&composite_;attempted_=true;
  const std::string path=WosProbe::Root()+"\\rsm_runtime\\";
  auto a=WosCascadeProbe::ReadFile(path+"rsm_gather.cso"),b=WosCascadeProbe::ReadFile(path+"rsm_composite.cso");
  if(a.empty()||b.empty())return false;
  return SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(a.data()),&gather_))&&SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(b.data()),&composite_));
 }
public:
 uint64_t rendered=0,unavailable=0,failed=0;bool lastMaps[2]={};const char*reason="inactive";
 ~D3D9Renderer(){Reset();}
 void Reset(){Release(gather_);Release(composite_);Release(owner_);attempted_=false;rendered=unavailable=failed=0;lastMaps[0]=lastMaps[1]=false;reason="inactive";}
 template<class Runtime>bool Render(Runtime&runtime,IDirect3DDevice9*d,IDirect3DBaseTexture9*source,IDirect3DBaseTexture9*depth,
  const WosIndirectCamera::Camera&camera,IDirect3DTexture9*normal,IDirect3DTexture9*rho){
  lastMaps[0]=lastMaps[1]=false;
  if(!d||!source||!depth||!normal||!rho||!runtime.Active()){++unavailable;reason="current material or shadow inputs unavailable";return false;}
  typename Runtime::Lease leases[2];NativeMap maps[2];
  for(unsigned i=0;i<2;++i)if(runtime.AcquireFluxMap(i,leases[i])&&runtime.StillCurrent(leases[i])){
   maps[i].current=true;maps[i].width=leases[i].context.width;maps[i].height=leases[i].context.height;std::memcpy(maps[i].vp,leases[i].context.vp,64);
  }
  if(!maps[0].current&&!maps[1].current){++unavailable;reason="no current emitter flux maps";return false;}
  WosRsmOrigin::CameraSpace common;Constants constants;Settings settings;
  settings.width=WosIndirect::width;settings.height=WosIndirect::height;settings.radius=WosIndirect::radius;settings.maxDistance=WosIndirect::maxDistance;settings.bounce=WosIndirect::bounce;settings.environment=WosIndirect::ambient;
  if(!WosRsmOrigin::RetainMainSnapshot(camera.worldOrigin,camera.relativeInverse,common)||!Pack(common,maps,settings,constants)){++unavailable;reason="current light projection unavailable";return false;}
  if(!QuarterDimensions(settings.width,settings.height,WosIndirect::gw,WosIndirect::gh)){++failed;reason="quarter-resolution dimensions mismatch";return false;}
  if(!Load(d)){++failed;reason="native gather shader unavailable";return false;}
  D3DSURFACE_DESC gatherDesc{};
  if(!WosIndirect::gatherRT||FAILED(WosIndirect::gatherRT->GetDesc(&gatherDesc))||gatherDesc.Format!=D3DFMT_A32B32G32R32F||gatherDesc.Width!=WosIndirect::gw||gatherDesc.Height!=WosIndirect::gh){++failed;reason="float32 gather target required";return false;}
  WosIndirect::State state(d);if(!state.valid){++failed;reason="game state capture failed";return false;}
  const bool wasBusy=WosIndirect::busy;WosIndirect::busy=true;bool ok=true;
  auto check=[&](HRESULT h){ok=SUCCEEDED(h)&&ok;};
  for(unsigned i=0;i<16;++i)check(d->SetTexture(i,nullptr));
  check(d->SetDepthStencilSurface(nullptr));for(unsigned i=1;i<state.slots;++i)check(d->SetRenderTarget(i,nullptr));
  check(d->SetRenderTarget(0,WosIndirect::gatherRT));check(d->SetVertexShader(nullptr));check(d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1));
  for(auto rs:{D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHABLENDENABLE,D3DRS_ALPHATESTENABLE,D3DRS_STENCILENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_SRGBWRITEENABLE,D3DRS_FOGENABLE,D3DRS_CLIPPLANEENABLE,D3DRS_SEPARATEALPHABLENDENABLE})check(d->SetRenderState(rs,0));
  check(d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));check(d->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID));check(d->SetRenderState(D3DRS_COLORWRITEENABLE,15));check(d->SetRenderState(D3DRS_MULTISAMPLEMASK,0xffffffff));
  for(unsigned i=0;i<9;++i){check(d->SetSamplerState(i,D3DSAMP_MINFILTER,D3DTEXF_POINT));check(d->SetSamplerState(i,D3DSAMP_MAGFILTER,D3DTEXF_POINT));check(d->SetSamplerState(i,D3DSAMP_MIPFILTER,D3DTEXF_NONE));check(d->SetSamplerState(i,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));check(d->SetSamplerState(i,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));check(d->SetSamplerState(i,D3DSAMP_SRGBTEXTURE,0));}
  check(d->SetTexture(0,depth));check(d->SetTexture(1,normal));check(d->SetTexture(2,rho));
  for(unsigned i=0;i<2;++i){const auto&lease=leases[constants.use[i]?i:1-i];if(!runtime.StillCurrent(lease)){ok=false;break;}
   const auto&v=lease.resources;if(!v.depthTexture||!v.normal||!v.flux){ok=false;break;}
   check(d->SetTexture(3+3*i,v.depthTexture));check(d->SetTexture(4+3*i,v.normal));check(d->SetTexture(5+3*i,v.flux));
  }
  check(d->SetPixelShaderConstantF(0,constants.c[0],48));check(g_original_set_pixel_shader(d,gather_));
  if(ok)ok=WosIndirect::Quad(d,WosIndirect::gw,WosIndirect::gh);
  // The source HDR stays linear until the original bloom/tone-map draw.
  for(unsigned i=0;i<9;++i)check(d->SetTexture(i,nullptr));
  check(d->SetRenderTarget(0,WosIndirect::compositeRT));check(d->SetTexture(0,source));check(d->SetTexture(1,WosIndirect::gather));check(d->SetTexture(2,depth));check(d->SetTexture(3,normal));check(d->SetTexture(4,rho));
  float c[108]={};std::memcpy(c,camera.relativeInverse,64);c[20]=1.f/settings.width;c[21]=1.f/settings.height;c[22]=settings.radius;c[23]=settings.maxDistance;
  c[28]=1.f/WosIndirect::gw;c[29]=1.f/WosIndirect::gh;c[30]=.20f;c[31]=.03f;c[48]=float(WosIndirect::debugView);c[49]=8;std::memcpy(c+104,constants.c[26],16);
  check(d->SetPixelShaderConstantF(0,c,27));check(g_original_set_pixel_shader(d,composite_));if(ok)ok=WosIndirect::Quad(d,settings.width,settings.height);
  ok=state.Restore()&&ok;WosIndirect::busy=wasBusy;
  if(ok){++rendered;lastMaps[0]=constants.use[0];lastMaps[1]=constants.use[1];reason="native shadow-map bounce";
   for(unsigned i=0;i<2;++i)if(constants.use[i]){WosIndirect::nativeRsmInputs.frame=leases[i].stamp.frame;WosIndirect::nativeRsmInputs.eligible[i]=true;WosIndirect::nativeRsmInputs.normalGeneration[i]=leases[i].stamp.stamp.generation;WosIndirect::nativeRsmInputs.fluxGeneration[i]=leases[i].fluxStamp.stamp.generation;}
  }
  else{++failed;reason="native gather or state restoration failed";}
  return ok;
 }
};
}
