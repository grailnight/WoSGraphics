#pragma once
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <vector>
#pragma comment(lib,"d3dcompiler.lib")
namespace WosDepthReadback {
using SetPS=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,IDirect3DPixelShader9*);
using SceneFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using DrawUP=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,UINT,const void*,UINT);
inline bool Read(IDirect3DDevice9*d,IDirect3DTexture9*source,UINT size,std::vector<float>&out,SetPS setPS=nullptr,DrawUP draw=nullptr,SceneFn begin=nullptr,SceneFn end=nullptr){
 if(!d||!source||size<1||size>8192)return false;
 IDirect3DStateBlock9 *state=nullptr;IDirect3DSurface9 *old[4]={},*ds=nullptr,*target=nullptr,*cpu=nullptr;IDirect3DPixelShader9*ps=nullptr;ID3DBlob*code=nullptr,*errors=nullptr;D3DVIEWPORT9 vp={};D3DCAPS9 caps={};
 if(FAILED(d->CreateStateBlock(D3DSBT_ALL,&state)))return false;
 bool ok=SUCCEEDED(d->GetViewport(&vp))&&SUCCEEDED(d->GetDeviceCaps(&caps));unsigned slots=caps.NumSimultaneousRTs<4?caps.NumSimultaneousRTs:4;
 for(unsigned i=0;i<slots;++i){HRESULT h=d->GetRenderTarget(i,&old[i]);if(FAILED(h)&&h!=D3DERR_NOTFOUND)ok=false;}
 HRESULT h=d->GetDepthStencilSurface(&ds);if(FAILED(h)&&h!=D3DERR_NOTFOUND)ok=false;
 if(!ok || !slots || !old[0]){for(auto*s:old)if(s)s->Release();if(ds)ds->Release();state->Release();return false;}
 const char*hlsl="sampler2D depthMap:register(s0);float4 main(float2 uv:TEXCOORD0):COLOR0 { return tex2D(depthMap,uv).rrrr; }";
 if(ok)ok=SUCCEEDED(D3DCompile(hlsl,strlen(hlsl),nullptr,nullptr,nullptr,"main","ps_3_0",0,0,&code,&errors));
 if(ok)ok=SUCCEEDED(d->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()),&ps));
 if(ok)ok=SUCCEEDED(d->CreateRenderTarget(size,size,D3DFMT_R32F,D3DMULTISAMPLE_NONE,0,FALSE,&target,nullptr))&&SUCCEEDED(d->CreateOffscreenPlainSurface(size,size,D3DFMT_R32F,D3DPOOL_SYSTEMMEM,&cpu,nullptr));
 if(ok){
  ok=SUCCEEDED(d->SetDepthStencilSurface(nullptr));for(unsigned i=1;i<slots;++i)ok=SUCCEEDED(d->SetRenderTarget(i,nullptr))&&ok;ok=SUCCEEDED(d->SetRenderTarget(0,target))&&ok;
  D3DVIEWPORT9 v={0,0,size,size,0,1};ok=SUCCEEDED(d->SetViewport(&v))&&ok;
  ok=SUCCEEDED(d->SetVertexShader(nullptr))&&SUCCEEDED(d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1))&&ok;
  ok=SUCCEEDED(setPS?setPS(d,ps):d->SetPixelShader(ps))&&ok;
  for(auto s:{D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHABLENDENABLE,D3DRS_ALPHATESTENABLE,D3DRS_STENCILENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_SRGBWRITEENABLE,D3DRS_FOGENABLE,D3DRS_CLIPPLANEENABLE})ok=SUCCEEDED(d->SetRenderState(s,0))&&ok;
  ok=SUCCEEDED(d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE))&&SUCCEEDED(d->SetRenderState(D3DRS_COLORWRITEENABLE,15))&&ok;
  ok=SUCCEEDED(d->SetTexture(0,source))&&ok;
  ok=SUCCEEDED(d->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,0))&&SUCCEEDED(d->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID))&&SUCCEEDED(d->SetRenderState(D3DRS_MULTISAMPLEMASK,0xffffffff))&&ok;
  for(auto s:{D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER})ok=SUCCEEDED(d->SetSamplerState(0,s,D3DTEXF_POINT))&&ok;
  ok=SUCCEEDED(d->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_NONE))&&SUCCEEDED(d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP))&&SUCCEEDED(d->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP))&&ok;
  struct Vertex{float x,y,z,w,u,v;};float e=float(size)-.5f;Vertex quad[]={{-.5f,-.5f,0,1,0,0},{e,-.5f,0,1,1,0},{-.5f,e,0,1,0,1},{e,e,0,1,1,1}};
  bool begun=SUCCEEDED(begin?begin(d):d->BeginScene());if(!begun)ok=false;
  if(ok)ok=SUCCEEDED(draw?draw(d,D3DPT_TRIANGLESTRIP,2,quad,sizeof(Vertex)):d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(Vertex)));
  if(begun)ok=SUCCEEDED(end?end(d):d->EndScene())&&ok;
  if(ok)ok=SUCCEEDED(d->GetRenderTargetData(target,cpu));
  D3DLOCKED_RECT lock={};if(ok)ok=SUCCEEDED(cpu->LockRect(&lock,nullptr,D3DLOCK_READONLY));
  if(ok){std::vector<float> result(size_t(size)*size);for(UINT y=0;y<size;++y)memcpy(result.data()+size_t(y)*size,static_cast<const unsigned char*>(lock.pBits)+size_t(y)*lock.Pitch,size*4);ok=SUCCEEDED(cpu->UnlockRect());if(ok)out=std::move(result);}
 }
 // Restore targets before applying the stateblock (which restores texture bindings).
 bool restored=SUCCEEDED(d->SetDepthStencilSurface(nullptr));for(unsigned i=0;i<slots;++i)restored=SUCCEEDED(d->SetRenderTarget(i,old[i]))&&restored;
 restored=SUCCEEDED(d->SetDepthStencilSurface(ds))&&SUCCEEDED(d->SetViewport(&vp))&&SUCCEEDED(state->Apply())&&restored;
 if(code)code->Release();if(errors)errors->Release();if(ps)ps->Release();if(target)target->Release();if(cpu)cpu->Release();for(auto*s:old)if(s)s->Release();if(ds)ds->Release();state->Release();return ok&&restored;
}
}
