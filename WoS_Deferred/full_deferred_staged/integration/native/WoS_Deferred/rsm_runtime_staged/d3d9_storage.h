#pragma once
// Staged real COM storage adapter. Compiled only; not exercised on the GPU and
// not included by production. It performs no native Clear/draw or readback.
#include <d3d9.h>
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_Deferred/rsm_runtime_staged/runtime_lifecycle.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_Deferred/rsm_device_ownership_staged/device_ownership.h"
namespace WosRsmRuntime {
class D3D9Storage {
 template<class T>static void Release(T*&p){if(p)p->Release();p=nullptr;}
 template<class T>static IUnknown* Identity(T*p){IUnknown*q=nullptr;if(!p||FAILED(p->QueryInterface(IID_IUnknown,reinterpret_cast<void**>(&q))))Release(q);return q;}
 static uint64_t Key(IUnknown*p){return uint64_t(reinterpret_cast<uintptr_t>(p));}
 struct Pins {
  IDirect3DSurface9*color=nullptr,*depth=nullptr;IDirect3DTexture9*depthTexture=nullptr;
  IUnknown*colorIdentity=nullptr,*depthIdentity=nullptr;WosRsmClear::Context context{};
 };
 struct Normal {
  IDirect3DTexture9*texture=nullptr;IDirect3DSurface9*surface=nullptr;
  IUnknown*textureIdentity=nullptr,*surfaceIdentity=nullptr;unsigned width=0,height=0;
 };
 Pins pins_[2]{};Normal normals_[2]{},flux_[2]{};IDirect3DDevice9*owner_=nullptr;IUnknown*deviceIdentity_=nullptr;
 uint64_t expectedDevice_=0,resourceOwnerKey_=0;unsigned mrtSlots_=0;WosRsmDeviceOwnership::Proof ownership_;
 static void ReleasePins(Pins&p){Release(p.depthTexture);Release(p.color);Release(p.depth);Release(p.colorIdentity);Release(p.depthIdentity);p.context={};}
 static void ReleaseNormal(Normal&n){Release(n.surfaceIdentity);Release(n.textureIdentity);Release(n.surface);Release(n.texture);n.width=n.height=0;}
 static bool Descriptors(IDirect3DSurface9*c,IDirect3DSurface9*d,const WosRsmClear::Context&context){
  D3DSURFACE_DESC cd{},dd{};
  return SUCCEEDED(c->GetDesc(&cd))&&SUCCEEDED(d->GetDesc(&dd))&&
   cd.Width==context.width&&cd.Height==context.height&&dd.Width==cd.Width&&dd.Height==cd.Height&&
   cd.Format==D3DFMT_A8R8G8B8&&dd.Format==D3DFORMAT(Intz)&&
   cd.MultiSampleType==D3DMULTISAMPLE_NONE&&dd.MultiSampleType==D3DMULTISAMPLE_NONE&&
   cd.MultiSampleQuality==0&&dd.MultiSampleQuality==0;
 }
public:
 struct Native {IDirect3DDevice9*device=nullptr;IDirect3DSurface9*color=nullptr,*depth=nullptr;};
 struct View {
  IDirect3DTexture9*normal=nullptr,*depthTexture=nullptr,*flux=nullptr;
  IDirect3DSurface9*normalSurface=nullptr,*nativeDepth=nullptr,*nativeColor=nullptr,*fluxSurface=nullptr;
 };
 D3D9Storage()=default;D3D9Storage(const D3D9Storage&)=delete;D3D9Storage&operator=(const D3D9Storage&)=delete;
 ~D3D9Storage(){Reset();}
 void EndFrame(){for(auto&p:pins_)ReleasePins(p);}
 void Reset(){EndFrame();for(auto&n:normals_)ReleaseNormal(n);for(auto&n:flux_)ReleaseNormal(n);ownership_.Reset();Release(deviceIdentity_);Release(owner_);expectedDevice_=resourceOwnerKey_=0;mrtSlots_=0;}
 void BeginFrame(uint64_t device){EndFrame();if(expectedDevice_!=device)Reset();expectedDevice_=device;}
 bool Matches(unsigned slot,const WosRsmClear::Context&c)const{
  return slot<2&&pins_[slot].depth&&c.key.device==expectedDevice_&&WosRsmClear::Same(pins_[slot].context,c);
 }
 bool Pin(unsigned slot,const Native&native,const WosRsmClear::Context&c){
  if(slot>1||!native.device||!native.color||!native.depth||c.key.device!=expectedDevice_)return false;
  // The hook device can be a ReShade facade while its bound resources belong
  // to the underlying device. Prove that relation from actual RT0/DS bindings,
  // retain the facade as the frame key, and reject every unrelated owner.
  const bool proved=ownership_.Valid()?ownership_.ValidateBindings():ownership_.Establish(native.device);
  if(!proved||!ownership_.MatchesFacade(native.device)||ownership_.FacadeKey()!=expectedDevice_||
     (resourceOwnerKey_&&ownership_.ResourceOwnerKey()!=resourceOwnerKey_)||
     ownership_.evidence.facadeTarget!=c.color||ownership_.evidence.facadeDepth!=c.key.depth||
     !ownership_.MatchesResourceOwner(native.color)||!ownership_.MatchesResourceOwner(native.depth))return false;
  if(pins_[slot].depth){ // Never release/reuse native IDs inside one frame.
   if(!Matches(slot,c))return false;
   IUnknown*di=Identity(native.device),*ci=Identity(native.color),*zi=Identity(native.depth);
   const bool same=di&&di==deviceIdentity_&&ci&&ci==pins_[slot].colorIdentity&&zi&&zi==pins_[slot].depthIdentity;
   Release(di);Release(ci);Release(zi);return same;
  }
  IUnknown*di=Identity(native.device);Pins p{};p.colorIdentity=Identity(native.color);p.depthIdentity=Identity(native.depth);
  bool ok=di&&Key(di)==expectedDevice_&&p.colorIdentity&&Key(p.colorIdentity)==c.color&&p.depthIdentity&&Key(p.depthIdentity)==c.key.depth&&
   Descriptors(native.color,native.depth,c);
  if(ok&&pins_[1-slot].depth)ok=pins_[1-slot].depthIdentity!=p.depthIdentity;
  if(ok)ok=SUCCEEDED(native.depth->GetContainer(IID_IDirect3DTexture9,reinterpret_cast<void**>(&p.depthTexture)))&&p.depthTexture;
  IDirect3DSurface9*level=nullptr;IUnknown*levelIdentity=nullptr;D3DSURFACE_DESC td{};
  if(ok)ok=SUCCEEDED(p.depthTexture->GetLevelDesc(0,&td))&&td.Width==c.width&&td.Height==c.height&&td.Format==D3DFORMAT(Intz)&&
   td.Pool==D3DPOOL_DEFAULT&&(td.Usage&D3DUSAGE_DEPTHSTENCIL)&&SUCCEEDED(p.depthTexture->GetSurfaceLevel(0,&level))&&level;
  if(ok){levelIdentity=Identity(level);ok=levelIdentity&&levelIdentity==p.depthIdentity;}
  Release(levelIdentity);Release(level);
  D3DCAPS9 caps{};
  if(ok&&!owner_)ok=SUCCEEDED(native.device->GetDeviceCaps(&caps))&&caps.NumSimultaneousRTs>=2&&caps.NumSimultaneousRTs<=4&&
   (caps.PrimitiveMiscCaps&D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS)&&(caps.PrimitiveMiscCaps&D3DPMISCCAPS_INDEPENDENTWRITEMASKS);
  if(ok&&owner_)ok=di==deviceIdentity_;
  if(!ok){Release(di);ReleasePins(p);return false;}
  if(!owner_){owner_=native.device;owner_->AddRef();deviceIdentity_=di;di=nullptr;mrtSlots_=caps.NumSimultaneousRTs;resourceOwnerKey_=ownership_.ResourceOwnerKey();}
  Release(di);p.color=native.color;p.color->AddRef();p.depth=native.depth;p.depth->AddRef();p.context=c;pins_[slot]=p;return true;
 }
 bool EnsureNormal(unsigned slot,unsigned width,unsigned height){
  if(slot>1||!owner_||!pins_[slot].depth||width!=ShadowSize||height!=ShadowSize)return false;
  auto&n=normals_[slot];if(n.texture&&n.width==width&&n.height==height)return true;
  ReleaseNormal(n);
  bool ok=SUCCEEDED(owner_->CreateTexture(width,height,1,D3DUSAGE_RENDERTARGET,D3DFMT_A16B16G16R16F,D3DPOOL_DEFAULT,&n.texture,nullptr))&&n.texture;
  if(ok)ok=SUCCEEDED(n.texture->GetSurfaceLevel(0,&n.surface))&&n.surface;
  if(ok){n.textureIdentity=Identity(n.texture);n.surfaceIdentity=Identity(n.surface);ok=n.textureIdentity&&n.surfaceIdentity;}
  if(ok&&normals_[1-slot].texture)ok=n.textureIdentity!=normals_[1-slot].textureIdentity&&n.surfaceIdentity!=normals_[1-slot].surfaceIdentity;
  if(!ok){ReleaseNormal(n);return false;}n.width=width;n.height=height;return true;
 }
 bool EnsureFlux(unsigned slot,unsigned width,unsigned height){
  if(slot>1||!owner_||mrtSlots_<3||!pins_[slot].depth||width!=ShadowSize||height!=ShadowSize)return false;
  auto&n=flux_[slot];if(n.texture&&n.width==width&&n.height==height)return true;
  ReleaseNormal(n);
  bool ok=SUCCEEDED(owner_->CreateTexture(width,height,1,D3DUSAGE_RENDERTARGET,D3DFMT_A16B16G16R16F,D3DPOOL_DEFAULT,&n.texture,nullptr))&&n.texture;
  if(ok)ok=SUCCEEDED(n.texture->GetSurfaceLevel(0,&n.surface))&&n.surface;
  if(ok){n.textureIdentity=Identity(n.texture);n.surfaceIdentity=Identity(n.surface);ok=n.textureIdentity&&n.surfaceIdentity;}
  if(ok&&flux_[1-slot].texture)ok=n.textureIdentity!=flux_[1-slot].textureIdentity&&n.surfaceIdentity!=flux_[1-slot].surfaceIdentity;
  if(!ok){ReleaseNormal(n);return false;}n.width=width;n.height=height;return true;
 }
 uint64_t NormalKey(unsigned slot)const{return slot<2?Key(normals_[slot].textureIdentity):0;}
 uint64_t FluxKey(unsigned slot)const{return slot<2?Key(flux_[slot].textureIdentity):0;}
 bool OwnsSurface(IUnknown*identity)const{if(!identity)return false;for(const auto&n:normals_)if(identity==n.surfaceIdentity)return true;for(const auto&n:flux_)if(identity==n.surfaceIdentity)return true;return false;}
 bool ZeroNormal(unsigned slot){
  if(slot>1||!owner_||!normals_[slot].surface)return false;
  // ColorFill operates only on private storage. Fail if either private normal
  // is currently an MRT owner; no SetRT, viewport or native-state changes.
  for(unsigned i=0;i<mrtSlots_;++i){
   IDirect3DSurface9*bound=nullptr;const HRESULT hr=owner_->GetRenderTarget(i,&bound);
   if(hr==D3DERR_NOTFOUND&&!bound&&i!=0)continue;
   if(FAILED(hr)||!bound){Release(bound);return false;}
   IUnknown*id=Identity(bound);bool ok=id!=nullptr;
   if(OwnsSurface(id))ok=false;
   Release(id);Release(bound);if(!ok)return false;
  }
  return SUCCEEDED(owner_->ColorFill(normals_[slot].surface,nullptr,0));
 }
 bool ZeroFlux(unsigned slot){
  if(slot>1||!owner_||!flux_[slot].surface)return false;
  // ColorFill operates only on private storage. Fail if either private normal
  // is currently an MRT owner; no SetRT, viewport or native-state changes.
  for(unsigned i=0;i<mrtSlots_;++i){
   IDirect3DSurface9*bound=nullptr;const HRESULT hr=owner_->GetRenderTarget(i,&bound);
   if(hr==D3DERR_NOTFOUND&&!bound&&i!=0)continue;
   if(FAILED(hr)||!bound){Release(bound);return false;}
   IUnknown*id=Identity(bound);bool ok=id!=nullptr;
   if(OwnsSurface(id))ok=false;
   Release(id);Release(bound);if(!ok)return false;
  }
  return SUCCEEDED(owner_->ColorFill(flux_[slot].surface,nullptr,0));
 }
 View GetView(unsigned slot)const{
  if(slot>1||!pins_[slot].depth)return {};
  return {normals_[slot].texture,pins_[slot].depthTexture,flux_[slot].texture,normals_[slot].surface,pins_[slot].depth,pins_[slot].color,flux_[slot].surface};
 }
};
using D3D9Lifecycle=Lifecycle<D3D9Storage>;
}
