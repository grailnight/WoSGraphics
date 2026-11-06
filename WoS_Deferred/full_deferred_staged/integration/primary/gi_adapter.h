#pragma once
// Include after native_indirect.h / renderer_probe.h. Independent conversion,
// called within BeginScene/EndScene at each Flush, before the batch RT1 clear.
#include <cstdint>
namespace WosPrimaryGI {
// The publisher must invalidate at BeginFrame/Reset/GI off and on any depth
// writer not represented by a primary material/guard. Generation increases on
// batches. current means this batch is valid for the exact scene/depth/frame.
// RT1 tag0 is untouched this batch; positive tags are valid; negative tags clear
// older receivers where an invalid/unlit/unknown depth writer won.
struct Snapshot {
 uint64_t frame=0,generation=0;uint32_t scene=0;UINT width=0,height=0;
 IDirect3DTexture9*depth=nullptr,*normalShadow=nullptr,*diffuseFog=nullptr;
 bool current=false;
};
class Adapter {
 IDirect3DDevice9*owner_=nullptr;
 IDirect3DTexture9*normal_=nullptr,*rho_=nullptr;
 IDirect3DSurface9*normalRT_=nullptr,*rhoRT_=nullptr;
 IUnknown*depthIdentity_=nullptr;
 IDirect3DTexture9*sourceNormal_=nullptr,*sourceRho_=nullptr;
 IDirect3DPixelShader9*shader_=nullptr;
 uint64_t frame_=0,generation_=0;uint32_t scene_=0;UINT width_=0,height_=0;
 bool ready_=false,stateRestoreFailed_=false;
 template<class T>static void Release(T*&p){if(p)p->Release();p=nullptr;}
 template<class T>static bool OwnedBy(IDirect3DDevice9*d,T*p){IDirect3DDevice9*actual=nullptr;bool ok=p&&SUCCEEDED(p->GetDevice(&actual))&&actual==d;Release(actual);return ok;}
 static bool Desc(IDirect3DTexture9*t,UINT w,UINT h,D3DFORMAT format){D3DSURFACE_DESC desc{};return t&&SUCCEEDED(t->GetLevelDesc(0,&desc))&&desc.Width==w&&desc.Height==h&&desc.Format==format&&desc.MultiSampleType==D3DMULTISAMPLE_NONE;}
 bool Targets(IDirect3DDevice9*d,UINT w,UINT h){
  if(width_==w&&height_==h&&normalRT_&&rhoRT_)return true;
  Release(normalRT_);Release(rhoRT_);Release(normal_);Release(rho_);width_=height_=0;
  bool ok=SUCCEEDED(d->CreateTexture(w,h,1,D3DUSAGE_RENDERTARGET,D3DFMT_A16B16G16R16F,D3DPOOL_DEFAULT,&normal_,nullptr))&&SUCCEEDED(normal_->GetSurfaceLevel(0,&normalRT_))&&SUCCEEDED(d->CreateTexture(w,h,1,D3DUSAGE_RENDERTARGET,D3DFMT_A16B16G16R16F,D3DPOOL_DEFAULT,&rho_,nullptr))&&SUCCEEDED(rho_->GetSurfaceLevel(0,&rhoRT_));
  if(ok){width_=w;height_=h;}return ok;
 }
 struct Suppress {
  bool probe=WosProbe::suppress,indirect=WosIndirect::busy;
  Suppress(){WosProbe::suppress=true;WosIndirect::busy=true;}
  ~Suppress(){WosProbe::suppress=probe;WosIndirect::busy=indirect;}
 };
public:
 uint64_t conversions=0;const char*reason="not converted";
 Adapter()=default;Adapter(const Adapter&)=delete;Adapter&operator=(const Adapter&)=delete;
 ~Adapter(){Reset();}
 void Invalidate(){ready_=false;frame_=generation_=0;scene_=0;Release(depthIdentity_);Release(sourceNormal_);Release(sourceRho_);Release(shader_);reason="snapshot invalidated";}
 void Reset(){stateRestoreFailed_=false;Invalidate();Release(normalRT_);Release(rhoRT_);Release(normal_);Release(rho_);Release(owner_);width_=height_=0;conversions=0;reason="reset";}
 bool Get(IDirect3DDevice9*d,uint64_t frame,uint32_t scene,IDirect3DTexture9*depth,IDirect3DTexture9*&normal,IDirect3DTexture9*&rho)const{
  normal=rho=nullptr;
  if(!ready_||!d||d!=owner_||!frame||frame!=frame_||!scene||scene!=scene_||!depth)return false;
  IUnknown*identity=nullptr;bool ok=SUCCEEDED(depth->QueryInterface(IID_IUnknown,reinterpret_cast<void**>(&identity)))&&identity==depthIdentity_;Release(identity);
  if(!ok)return false;normal=normal_;rho=rho_;return true; // Borrowed until Invalidate/Reset or the next Merge.
 }
 uint64_t MergedGeneration()const{return generation_;}
 bool StateRestoreFailed()const{return stateRestoreFailed_;}
 bool Merge(IDirect3DDevice9*d,const Snapshot&s,IDirect3DPixelShader9*shader){
  stateRestoreFailed_=false;
  if(!d||!s.current||!s.frame||!s.generation||!s.scene||!s.width||!s.height||s.width>4096||s.height>4096||!s.depth||!s.normalShadow||!s.diffuseFog||!shader){Invalidate();reason="invalid current batch";return false;}
  IDirect3DTexture9*n=nullptr,*r=nullptr;
  const bool cumulative=width_==s.width&&height_==s.height&&Get(d,s.frame,s.scene,s.depth,n,r);
  if(cumulative&&s.generation<generation_){reason="stale batch generation";return false;}
  if(cumulative&&s.generation==generation_){
   if(sourceNormal_==s.normalShadow&&sourceRho_==s.diffuseFog&&shader_==shader)return true;
   Invalidate();reason="same generation changed source";return false;
  }
  if(!OwnedBy(d,s.depth)||!OwnedBy(d,s.normalShadow)||!OwnedBy(d,s.diffuseFog)||!OwnedBy(d,shader)||!Desc(s.depth,s.width,s.height,D3DFORMAT(MAKEFOURCC('I','N','T','Z')))||!Desc(s.normalShadow,s.width,s.height,D3DFMT_A16B16G16R16F)||!Desc(s.diffuseFog,s.width,s.height,D3DFMT_A16B16G16R16F)){Invalidate();reason="foreign or mismatched source";return false;}
  if(!cumulative)Invalidate();
  if(owner_!=d){Reset();owner_=d;owner_->AddRef();}
  D3DCAPS9 caps{};if(FAILED(d->GetDeviceCaps(&caps))||caps.NumSimultaneousRTs<2||caps.PixelShaderVersion<D3DPS_VERSION(3,0)){Invalidate();reason="conversion requires SM3 and two MRTs";return false;}
  if(!Targets(d,s.width,s.height)){Invalidate();reason="conversion target allocation failed";return false;}
  IUnknown*identity=nullptr;if(FAILED(s.depth->QueryInterface(IID_IUnknown,reinterpret_cast<void**>(&identity)))){Invalidate();reason="depth identity unavailable";return false;}
  WosIndirect::State state(d);bool ok=state.valid;Suppress suppress;
  auto check=[&](HRESULT hr){ok=SUCCEEDED(hr)&&ok;};
  if(ok){
   for(unsigned i=0;i<16;++i)check(d->SetTexture(i,nullptr));
   check(d->SetDepthStencilSurface(nullptr));for(unsigned i=1;i<state.slots;++i)check(d->SetRenderTarget(i,nullptr));
   check(d->SetRenderTarget(0,normalRT_));check(d->SetRenderTarget(1,rhoRT_));
   D3DVIEWPORT9 viewport={0,0,s.width,s.height,0,1};check(d->SetViewport(&viewport));
   check(d->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE));
   if(ok&&!cumulative)check(d->Clear(0,nullptr,D3DCLEAR_TARGET,0,1,0));
   check(d->SetVertexShader(nullptr));check(d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1));check(d->SetRenderState(D3DRS_WRAP0,0));
   for(auto rs:{D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHABLENDENABLE,D3DRS_ALPHATESTENABLE,D3DRS_STENCILENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_SRGBWRITEENABLE,D3DRS_FOGENABLE,D3DRS_CLIPPLANEENABLE,D3DRS_SEPARATEALPHABLENDENABLE,D3DRS_DITHERENABLE})check(d->SetRenderState(rs,0));
   check(d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));check(d->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID));check(d->SetRenderState(D3DRS_COLORWRITEENABLE,15));check(d->SetRenderState(D3DRS_COLORWRITEENABLE1,15));check(d->SetRenderState(D3DRS_MULTISAMPLEMASK,0xffffffff));
   for(unsigned i=0;i<2;++i){check(d->SetSamplerState(i,D3DSAMP_MINFILTER,D3DTEXF_POINT));check(d->SetSamplerState(i,D3DSAMP_MAGFILTER,D3DTEXF_POINT));check(d->SetSamplerState(i,D3DSAMP_MIPFILTER,D3DTEXF_NONE));check(d->SetSamplerState(i,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));check(d->SetSamplerState(i,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));check(d->SetSamplerState(i,D3DSAMP_SRGBTEXTURE,0));}
   check(d->SetTexture(0,s.normalShadow));check(d->SetTexture(1,s.diffuseFog));check(g_original_set_pixel_shader(d,shader));
   if(ok)ok=WosIndirect::Quad(d,s.width,s.height);
   check(d->SetTexture(0,nullptr));check(d->SetTexture(1,nullptr));
  }
  if(state.valid){const bool restored=state.Restore();stateRestoreFailed_=!restored;ok=restored&&ok;}
  if(!ok){Release(identity);Invalidate();reason="conversion draw or restore failed";return false;}
  Release(depthIdentity_);Release(sourceNormal_);Release(sourceRho_);Release(shader_);
  depthIdentity_=identity;sourceNormal_=s.normalShadow;sourceNormal_->AddRef();sourceRho_=s.diffuseFog;sourceRho_->AddRef();shader_=shader;shader_->AddRef();
  frame_=s.frame;generation_=s.generation;scene_=s.scene;ready_=true;++conversions;reason="current cumulative primary normals and rho";return true;
 }
};
}
