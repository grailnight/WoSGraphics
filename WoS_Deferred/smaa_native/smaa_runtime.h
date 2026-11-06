#pragma once
#include <d3d9.h>
#include <cstring>
#include "smaa_data.h"
namespace WosSmaa {
static bool enabled=true, done=false, faulted=false;
static unsigned long frames=0, failures=0, skipped=0;
static const char* status="Waiting for native composite";
template<class T> static void Release(T*&p){if(p){p->Release();p=nullptr;}}
struct Resources {
    IDirect3DDevice9* owner=nullptr; // Borrowed; released before Reset.
    IDirect3DTexture9 *color=nullptr,*edges=nullptr,*weights=nullptr,*output=nullptr,*area=nullptr,*search=nullptr;
    IDirect3DSurface9 *colorRT=nullptr,*edgesRT=nullptr,*weightsRT=nullptr,*outputRT=nullptr;
    IDirect3DPixelShader9 *edgePS=nullptr,*weightPS=nullptr,*blendPS=nullptr;
    UINT width=0,height=0; D3DFORMAT format=D3DFMT_UNKNOWN;
    void Clear(){
        Release(colorRT);Release(edgesRT);Release(weightsRT);Release(outputRT);
        Release(color);Release(edges);Release(weights);Release(output);Release(area);Release(search);
        Release(edgePS);Release(weightPS);Release(blendPS);owner=nullptr;width=height=0;
    }
} static r;
static void Reset(){r.Clear();done=false;faulted=false;status="Waiting for native composite";}
static void FrameEnd(){done=false;}
static bool Lookup(IDirect3DDevice9*d,UINT w,UINT h,const unsigned char*data,IDirect3DTexture9**out){
    if(FAILED(d->CreateTexture(w,h,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,out,nullptr)))return false;
    D3DLOCKED_RECT lock={};if(FAILED((*out)->LockRect(0,&lock,nullptr,0)))return false;
    for(UINT y=0;y<h;++y)std::memcpy(static_cast<char*>(lock.pBits)+y*lock.Pitch,data+y*w*4,w*4);
    return SUCCEEDED((*out)->UnlockRect(0));
}
static bool Target(IDirect3DDevice9*d,UINT w,UINT h,D3DFORMAT f,IDirect3DTexture9**tex,IDirect3DSurface9**surf){
    return SUCCEEDED(d->CreateTexture(w,h,1,D3DUSAGE_RENDERTARGET,f,D3DPOOL_DEFAULT,tex,nullptr))&&SUCCEEDED((*tex)->GetSurfaceLevel(0,surf));
}
static bool Ensure(IDirect3DDevice9*d,const D3DSURFACE_DESC&desc){
    if(r.owner==d&&r.width==desc.Width&&r.height==desc.Height&&r.format==desc.Format)return true;
    r.Clear();
    using namespace WosSmaaData;
    bool ok=Target(d,desc.Width,desc.Height,desc.Format,&r.color,&r.colorRT)
        &&Target(d,desc.Width,desc.Height,D3DFMT_A8R8G8B8,&r.edges,&r.edgesRT)
        &&Target(d,desc.Width,desc.Height,D3DFMT_A8R8G8B8,&r.weights,&r.weightsRT)
        &&Target(d,desc.Width,desc.Height,desc.Format,&r.output,&r.outputRT)
        &&Lookup(d,AreaTexWidth,AreaTexHeight,AreaTex,&r.area)
        &&Lookup(d,SearchTexWidth,SearchTexHeight,SearchTex,&r.search)
        &&SUCCEEDED(d->CreatePixelShader(Edges,&r.edgePS))
        &&SUCCEEDED(d->CreatePixelShader(Weights,&r.weightPS))
        &&SUCCEEDED(d->CreatePixelShader(Blend,&r.blendPS));
    if(!ok){r.Clear();return false;}
    r.owner=d;r.width=desc.Width;r.height=desc.Height;r.format=desc.Format;return true;
}
// D3DSBT_ALL omits render targets and depth surfaces. Keep those explicitly.
struct State {
    IDirect3DDevice9*d;IDirect3DStateBlock9*block=nullptr;
    IDirect3DSurface9*rt[4]={},*depth=nullptr;
    D3DVIEWPORT9 vp={};RECT scissor={};bool valid=false,dirty=false;
    State(IDirect3DDevice9*dev):d(dev){
        for(UINT i=0;i<4;++i){HRESULT hr=d->GetRenderTarget(i,&rt[i]);if(FAILED(hr)&&(i==0||hr!=D3DERR_NOTFOUND))return;}
        HRESULT hr=d->GetDepthStencilSurface(&depth);if(FAILED(hr)&&hr!=D3DERR_NOTFOUND)return;
        if(FAILED(d->GetViewport(&vp))||FAILED(d->GetScissorRect(&scissor)))return;
        valid=SUCCEEDED(d->CreateStateBlock(D3DSBT_ALL,&block))&&SUCCEEDED(block->Capture());
    }
    bool Restore(){
        if(!dirty)return true;
        bool ok=true;auto check=[&](HRESULT hr){ok=SUCCEEDED(hr)&&ok;};
        for(UINT i=0;i<16;++i)check(d->SetTexture(i,nullptr));
        check(d->SetDepthStencilSurface(nullptr));
        for(UINT i=1;i<4;++i)check(d->SetRenderTarget(i,nullptr));
        for(UINT i=0;i<4;++i)check(d->SetRenderTarget(i,rt[i]));
        check(d->SetDepthStencilSurface(depth));check(block->Apply());
        check(d->SetViewport(&vp));check(d->SetScissorRect(&scissor));
        if(ok)dirty=false;return ok;
    }
    ~State(){if(dirty)Restore();Release(block);for(auto*&p:rt)Release(p);Release(depth);}
};
static bool Pass(IDirect3DDevice9*d,IDirect3DSurface9*target,IDirect3DPixelShader9*ps,IDirect3DTexture9*t0,IDirect3DTexture9*t1=nullptr,IDirect3DTexture9*t2=nullptr){
    if(FAILED(d->SetTexture(0,nullptr))||FAILED(d->SetTexture(1,nullptr))||FAILED(d->SetTexture(2,nullptr))||FAILED(d->SetRenderTarget(0,target)))return false;
    D3DVIEWPORT9 vp={0,0,r.width,r.height,0,1};
    if(FAILED(d->SetViewport(&vp))||FAILED(d->Clear(0,nullptr,D3DCLEAR_TARGET,0,1,0))||FAILED(d->SetPixelShader(ps)))return false;
    if(FAILED(d->SetTexture(0,t0))||FAILED(d->SetTexture(1,t1))||FAILED(d->SetTexture(2,t2)))return false;
    struct Vertex{float x,y,z,w,u,v;};
    const float w=static_cast<float>(r.width),h=static_cast<float>(r.height);
    const Vertex q[]={{-.5f,-.5f,0,1,0,0},{w-.5f,-.5f,0,1,1,0},{-.5f,h-.5f,0,1,0,1},{w-.5f,h-.5f,0,1,1,1}};
    return SUCCEEDED(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,q,sizeof(Vertex)));
}
static bool Render(IDirect3DDevice9*d){
    if(!enabled||done||faulted)return false;
    State state(d);D3DSURFACE_DESC desc={};
    if(!state.valid||!state.rt[0]||FAILED(state.rt[0]->GetDesc(&desc))){++skipped;return false;}
    // Never reinterpret an atlas/subviewport, MSAA surface, or depth target.
    if(desc.MultiSampleType!=D3DMULTISAMPLE_NONE||state.vp.X||state.vp.Y||state.vp.Width!=desc.Width||state.vp.Height!=desc.Height||desc.Width<64||desc.Height<64
       ||(desc.Format!=D3DFMT_A8R8G8B8&&desc.Format!=D3DFMT_X8R8G8B8&&desc.Format!=D3DFMT_A16B16G16R16F)){
        ++skipped;status="Composite target unsupported; native image preserved";return false;
    }
    done=true;
    if(!Ensure(d,desc)){++failures;faulted=true;status="SMAA resource creation failed";return false;}
    if(FAILED(d->StretchRect(state.rt[0],nullptr,r.colorRT,nullptr,D3DTEXF_NONE))){++failures;faulted=true;status="SMAA scene copy failed";return false;}
    state.dirty=true;
    bool ok=true;auto check=[&](HRESULT hr){ok=SUCCEEDED(hr)&&ok;};
    for(UINT i=0;i<16;++i)check(d->SetTexture(i,nullptr));
    check(d->SetDepthStencilSurface(nullptr));for(UINT i=1;i<4;++i)check(d->SetRenderTarget(i,nullptr));
    check(d->SetVertexShader(nullptr));check(d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1));
    check(d->SetStreamSourceFreq(0,1));check(d->SetIndices(nullptr));
    const D3DRENDERSTATETYPE off[]={D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHATESTENABLE,D3DRS_ALPHABLENDENABLE,D3DRS_SEPARATEALPHABLENDENABLE,D3DRS_STENCILENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_SRGBWRITEENABLE,D3DRS_FOGENABLE,D3DRS_LIGHTING,D3DRS_CLIPPLANEENABLE};
    for(auto s:off)check(d->SetRenderState(s,0));
    check(d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));check(d->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID));
    check(d->SetRenderState(D3DRS_COLORWRITEENABLE,15));check(d->SetRenderState(D3DRS_MULTISAMPLEMASK,0xffffffff));
    check(d->SetRenderState(D3DRS_WRAP0,0));
    for(UINT s=0;s<3;++s){
        check(d->SetSamplerState(s,D3DSAMP_MINFILTER,D3DTEXF_LINEAR));check(d->SetSamplerState(s,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR));
        check(d->SetSamplerState(s,D3DSAMP_MIPFILTER,D3DTEXF_NONE));check(d->SetSamplerState(s,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));check(d->SetSamplerState(s,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));
        check(d->SetSamplerState(s,D3DSAMP_SRGBTEXTURE,0));check(d->SetSamplerState(s,D3DSAMP_MAXMIPLEVEL,0));check(d->SetSamplerState(s,D3DSAMP_MIPMAPLODBIAS,0));
    }
    const float metrics[]={1.f/r.width,1.f/r.height,static_cast<float>(r.width),static_cast<float>(r.height)};
    check(d->SetPixelShaderConstantF(0,metrics,1));
    ok=ok&&Pass(d,r.edgesRT,r.edgePS,r.color)&&Pass(d,r.weightsRT,r.weightPS,r.edges,r.area,r.search)&&Pass(d,r.outputRT,r.blendPS,r.color,r.weights);
    // Commit only after every pass succeeded. Source alpha is retained.
    if(ok)ok=SUCCEEDED(d->StretchRect(r.outputRT,nullptr,state.rt[0],nullptr,D3DTEXF_NONE));
    bool restored=state.Restore();
    if(!ok||!restored){++failures;faulted=true;status="SMAA failed; disabled for this session";return false;}
    ++frames;status="SMAA 1x High / native composite";return true;
}
}
