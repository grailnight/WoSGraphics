#pragma once
#include <windows.h>
#include <d3d9.h>
namespace WosShadowTargets {
struct Set {
 IDirect3DTexture9 *textures[4]={};IDirect3DSurface9 *depth[4]={};IDirect3DSurface9 *color=nullptr;UINT resolution=0;
 Set()=default;Set(const Set&)=delete;Set&operator=(const Set&)=delete;
 void Reset(){for(auto &s:depth){if(s)s->Release();s=nullptr;}for(auto &t:textures){if(t)t->Release();t=nullptr;}if(color)color->Release();color=nullptr;resolution=0;}
 ~Set(){Reset();}
 bool Create(IDirect3DDevice9 *d,UINT size){
  if(!d||size<16||size>8192)return false;D3DCAPS9 caps={};if(FAILED(d->GetDeviceCaps(&caps))||size>caps.MaxTextureWidth||size>caps.MaxTextureHeight)return false;
  Set pending;
  for(unsigned i=0;i<4;++i){if(FAILED(d->CreateTexture(size,size,1,D3DUSAGE_DEPTHSTENCIL,static_cast<D3DFORMAT>(MAKEFOURCC('I','N','T','Z')),D3DPOOL_DEFAULT,&pending.textures[i],nullptr))||FAILED(pending.textures[i]->GetSurfaceLevel(0,&pending.depth[i])))return false;}
  if(FAILED(d->CreateRenderTarget(size,size,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&pending.color,nullptr)))return false;
  Reset();for(unsigned i=0;i<4;++i){textures[i]=pending.textures[i];pending.textures[i]=nullptr;depth[i]=pending.depth[i];pending.depth[i]=nullptr;}
  color=pending.color;pending.color=nullptr;resolution=size;return true;
 }
};
// Stateblocks don't restore render targets. Bindings and viewport are retained
// explicitly, including MRT slots which may already contain a diagnostic buffer.
struct Binding {
 IDirect3DDevice9 *d=nullptr;IDirect3DSurface9 *saved[4]={},*ds=nullptr;D3DVIEWPORT9 vp={};unsigned slots=0;bool active=false;
 Binding()=default;Binding(const Binding&)=delete;Binding&operator=(const Binding&)=delete;
 bool Select(const Set &set,unsigned index){
  if(!active||!d||index>=4||!set.depth[index])return false;
  // All four maps in Set share color target and dimensions; only depth changes.
  return SUCCEEDED(d->SetDepthStencilSurface(set.depth[index]));
 }
 HRESULT End(){
  if(!active)return S_OK;HRESULT result=S_OK;
  auto check=[&](HRESULT h){if(FAILED(h)&&SUCCEEDED(result))result=h;};
  check(d->SetDepthStencilSurface(nullptr));
  for(unsigned i=0;i<slots;++i)check(d->SetRenderTarget(i,saved[i]));
  check(d->SetDepthStencilSurface(ds));check(d->SetViewport(&vp));
  for(auto &s:saved){if(s)s->Release();s=nullptr;}if(ds)ds->Release();ds=nullptr;active=false;d=nullptr;return result;
 }
 ~Binding(){End();}
 bool Begin(IDirect3DDevice9 *device,const Set &set,unsigned index){
  if(active||!device||index>=4||!set.color||!set.depth[index])return false;
  d=device;D3DCAPS9 caps={};if(FAILED(d->GetDeviceCaps(&caps))||FAILED(d->GetViewport(&vp)))return false;
  slots=caps.NumSimultaneousRTs<4?caps.NumSimultaneousRTs:4;if(!slots)return false;
  for(unsigned i=0;i<slots;++i){HRESULT h=d->GetRenderTarget(i,&saved[i]);if(FAILED(h)&&h!=D3DERR_NOTFOUND){for(auto &s:saved){if(s)s->Release();s=nullptr;}return false;}}
  HRESULT h=d->GetDepthStencilSurface(&ds);if(FAILED(h)&&h!=D3DERR_NOTFOUND){for(auto &s:saved){if(s)s->Release();s=nullptr;}return false;}
  active=true;bool ok=SUCCEEDED(d->SetDepthStencilSurface(nullptr));
  for(unsigned i=1;i<slots;++i)ok=SUCCEEDED(d->SetRenderTarget(i,nullptr))&&ok;
  ok=SUCCEEDED(d->SetRenderTarget(0,set.color))&&ok;ok=SUCCEEDED(d->SetDepthStencilSurface(set.depth[index]))&&ok;
  D3DVIEWPORT9 target={0,0,set.resolution,set.resolution,0,1};ok=SUCCEEDED(d->SetViewport(&target))&&ok;
  if(!ok){End();return false;}return true;
 }
};
}
