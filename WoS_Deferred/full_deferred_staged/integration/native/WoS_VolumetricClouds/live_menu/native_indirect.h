#pragma once
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/indirect_camera.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/indirect_materials.h"
// Screen-space GI before the existing tone mapper: half-resolution ray trace,
// temporal accumulation with reprojection, full-resolution denoise/composite.
// The quarter-resolution gather/composite targets remain for the native RSM path.
namespace WosIndirect {
static bool enabled=false,busy=false,rendered=false,lastNativeRsm=false;
// Native RSM capture ([Indirect] NativeRSM). It hooks every shadow draw, yet its
// gather has never committed in play, so it is off unless asked for; SSGI is
// the fallback either way.
static bool nativeRsmCapture=false;
static bool (*nativeRsmRender)(IDirect3DDevice9*,IDirect3DBaseTexture9*,IDirect3DBaseTexture9*,const WosIndirectCamera::Camera&,IDirect3DTexture9*,IDirect3DTexture9*)=nullptr;
static void (*nativeRsmReset)()=nullptr;
static float bounce=.6f,ambient=.12f,radius=8.f,maxDistance=240.f;
static int debugView=0;
static const char*status="F5: indirect lighting OFF";
static IDirect3DDevice9*owner=nullptr;
static IDirect3DTexture9*gather=nullptr,*composite=nullptr;
static IDirect3DSurface9*gatherRT=nullptr,*compositeRT=nullptr;
static IDirect3DPixelShader9*gatherPS=nullptr,*compositePS=nullptr,*depthCopyPS=nullptr;
static IDirect3DTexture9*captureDepth=nullptr;
static unsigned width=0,height=0,gw=0,gh=0,frames=0,failures=0;
// SSGI: one trace target and two alternating history sets (accumulated light + receiver distance).
static IDirect3DTexture9*ssgiTrace=nullptr,*ssgiAccum[2]={},*ssgiMeta[2]={};
static IDirect3DSurface9*ssgiTraceRT=nullptr,*ssgiAccumRT[2]={},*ssgiMetaRT[2]={};
static IDirect3DPixelShader9*ssgiTracePS=nullptr,*ssgiTemporalPS=nullptr,*ssgiCompositePS=nullptr;
// Persisted in [SSGI]: history length in frames and anti-ghosting strength (0 = no variance clip).
static float ssgiHistory=24.f,ssgiAntiGhost=.75f;static bool ssgiSettingsLoaded=false;
// Share of last frame's accumulated bounce a donor passes on again (0 = one bounce).
static float ssgiMultiBounce=1.f;
static std::string SSGISettingsPath(){return WosProbe::Root()+"\\full_deferred\\settings.ini";}
namespace V2{static void Load();static void Release();}
static void LoadSSGISettings(){
 char v[64]={};const std::string path=SSGISettingsPath();
 GetPrivateProfileStringA("SSGI","History","24",v,sizeof(v),path.c_str());float h=float(std::atof(v));ssgiHistory=std::isfinite(h)?(std::min)((std::max)(h,4.f),48.f):24.f;
 GetPrivateProfileStringA("SSGI","AntiGhosting","0.75",v,sizeof(v),path.c_str());float a=float(std::atof(v));ssgiAntiGhost=std::isfinite(a)?(std::min)((std::max)(a,0.f),1.f):.75f;
 GetPrivateProfileStringA("SSGI","MultiBounce","1",v,sizeof(v),path.c_str());float m=float(std::atof(v));ssgiMultiBounce=std::isfinite(m)?(std::min)((std::max)(m,0.f),1.f):1.f;
 ssgiSettingsLoaded=true;V2::Load();
}
static void SaveSSGISettings(){
 char v[64]={};const std::string path=SSGISettingsPath();
 std::snprintf(v,sizeof(v),"%.9g",ssgiHistory);WritePrivateProfileStringA("SSGI","History",v,path.c_str());
 std::snprintf(v,sizeof(v),"%.9g",ssgiAntiGhost);WritePrivateProfileStringA("SSGI","AntiGhosting",v,path.c_str());
 std::snprintf(v,sizeof(v),"%.9g",ssgiMultiBounce);WritePrivateProfileStringA("SSGI","MultiBounce",v,path.c_str());
}
static unsigned hw=0,hh=0,historyIndex=0,frameIndex=0,historyFrames=0,historyResets=0,lastRenderPresent=0;
static bool historyValid=false;
static float prevRelativeInverse[16]={};static double prevOrigin[3]={};
static double cpuMs=0,cpuSum=0;
static unsigned presentFrames=0,bloomSeen=0,skippedInputs=0,skippedSource=0,lastInputFlags=0,fastGlobalsFrames=0;
static unsigned skippedMasks[16]={},lastCameraScene=0,materialFrames=0;
static WosIndirectCamera::Camera captureCamera;
static ULONGLONG lastLog=0;
static unsigned lastStatusFrames=0;static double observedPresentFps=0;
static IDirect3DBaseTexture9*captureSource=nullptr,*captureAdapt=nullptr;
static IDirect3DTexture9*captureNormal=nullptr,*captureRho=nullptr;
static std::string pendingCapture,lastCapture;
static float captureSettings[5]={};
struct NativeRsmInputs {uint64_t frame=0,normalGeneration[2]={},fluxGeneration[2]={};bool eligible[2]={};};
static NativeRsmInputs nativeRsmInputs,captureNativeRsmInputs;
static bool captureNativeRsm=false;
static void ReleaseCapture(){if(captureNormal)captureNormal->Release();if(captureRho)captureRho->Release();captureNormal=captureRho=nullptr;if(captureDepth)captureDepth->Release();captureDepth=nullptr;if(captureSource)captureSource->Release();if(captureAdapt)captureAdapt->Release();captureSource=captureAdapt=nullptr;pendingCapture.clear();}
static bool SaveTexture(IDirect3DDevice9*d,IDirect3DBaseTexture9*base,const std::string&path,FILE*meta,const char*name){
 if(!base){std::fprintf(meta,"\"%s\":{\"ok\":false}",name);return false;}IDirect3DTexture9*t=nullptr;if(FAILED(base->QueryInterface(IID_IDirect3DTexture9,reinterpret_cast<void**>(&t)))){std::fprintf(meta,"\"%s\":{\"ok\":false}",name);return false;}
 IDirect3DSurface9*src=nullptr,*cpu=nullptr;D3DSURFACE_DESC desc={};bool ok=SUCCEEDED(t->GetSurfaceLevel(0,&src));t->Release();if(ok)ok=SUCCEEDED(src->GetDesc(&desc));
 unsigned stride=desc.Format==D3DFMT_A16B16G16R16F?8:desc.Format==D3DFMT_A32B32G32R32F?16:desc.Format==D3DFMT_R32F?4:0;
 if(!stride||uint64_t(desc.Width)*desc.Height*stride>128*1024*1024)ok=false;
 if(ok)ok=SUCCEEDED(d->CreateOffscreenPlainSurface(desc.Width,desc.Height,desc.Format,D3DPOOL_SYSTEMMEM,&cpu,nullptr))&&SUCCEEDED(d->GetRenderTargetData(src,cpu));
 D3DLOCKED_RECT lock={};if(ok){ok=SUCCEEDED(cpu->LockRect(&lock,nullptr,D3DLOCK_READONLY));if(ok){FILE*f=std::fopen(path.c_str(),"wb");if(!f)ok=false;else{for(unsigned y=0;y<desc.Height;++y)if(std::fwrite(static_cast<unsigned char*>(lock.pBits)+size_t(y)*lock.Pitch,stride,desc.Width,f)!=desc.Width)ok=false;ok=std::fclose(f)==0&&ok;}cpu->UnlockRect();}}
 std::fprintf(meta,"\"%s\":{\"ok\":%s,\"width\":%u,\"height\":%u,\"d3d_format\":%u,\"bytes_per_pixel\":%u}",name,ok?"true":"false",desc.Width,desc.Height,unsigned(desc.Format),stride);
 if(src)src->Release();if(cpu)cpu->Release();return ok;
}
static void SaveCapture(){
 if(pendingCapture.empty())return;FILE*f=std::fopen((pendingCapture+"\\indirect_buffers.json").c_str(),"wb");
 if(f){std::fprintf(f,"{\"revision\":19,\"receiver_grid\":\"%s\",\"native_rsm_used\":%s,\"settings\":{\"debug_view\":%d,\"bounce\":%.3f,\"ambient\":%.3f,\"radius\":%.2f,\"max_distance\":%.2f},",captureNativeRsm?"quarter_vpos_4x4_v1":"ssgi_half_topleft_2x2",captureNativeRsm?"true":"false",int(captureSettings[0]),captureSettings[1],captureSettings[2],captureSettings[3],captureSettings[4]);SaveTexture(owner,captureSource,pendingCapture+"\\indirect_source.raw",f,"source");std::fputs(",",f);SaveTexture(owner,captureNativeRsm?gather:ssgiAccum[historyIndex],pendingCapture+"\\indirect_gather.raw",f,"gather");std::fputs(",",f);SaveTexture(owner,composite,pendingCapture+"\\indirect_composite.raw",f,"composite");std::fputs(",",f);if(captureAdapt)SaveTexture(owner,captureAdapt,pendingCapture+"\\indirect_adaptation.raw",f,"adaptation");else std::fputs("\"adaptation\":null",f);std::fputs(",",f);SaveTexture(owner,captureDepth,pendingCapture+"\\indirect_depth.raw",f,"depth");std::fputs(",",f);SaveTexture(owner,captureNormal,pendingCapture+"\\indirect_normal.raw",f,"normal");std::fputs(",",f);SaveTexture(owner,captureRho,pendingCapture+"\\indirect_rho.raw",f,"rho");std::fprintf(f,",\"camera\":{\"scene\":%u,\"coordinate_space\":\"camera_relative_world_axes\",\"position\":[0,0,0],\"world_origin\":[%.17g,%.17g,%.17g],\"inverse_vp\":[",captureCamera.scene,captureCamera.worldOrigin[0],captureCamera.worldOrigin[1],captureCamera.worldOrigin[2]);for(unsigned i=0;i<16;++i)std::fprintf(f,"%s%.9g",i?",":"",captureCamera.relativeInverse[i]);std::fprintf(f,"]},\"native_rsm_inputs\":{\"host_frame\":%llu,\"maps\":[",captureNativeRsmInputs.frame);for(unsigned i=0;i<2;++i)std::fprintf(f,"%s{\"slot\":%u,\"eligible\":%s,\"normal_generation\":%llu,\"flux_generation\":%llu}",i?",":"",i,captureNativeRsmInputs.eligible[i]?"true":"false",captureNativeRsmInputs.normalGeneration[i],captureNativeRsmInputs.fluxGeneration[i]);std::fputs("]}}\n",f);std::fclose(f);}
 ReleaseCapture();
}
static void Release(){
 ReleaseCapture();
 if(gatherRT)gatherRT->Release();if(compositeRT)compositeRT->Release();if(gather)gather->Release();if(composite)composite->Release();if(gatherPS)gatherPS->Release();if(compositePS)compositePS->Release();if(depthCopyPS)depthCopyPS->Release();
 gatherRT=compositeRT=nullptr;gather=composite=nullptr;gatherPS=compositePS=depthCopyPS=nullptr;owner=nullptr;width=height=gw=gh=0;
 auto drop=[](auto*&p){if(p)p->Release();p=nullptr;};
 drop(ssgiTraceRT);drop(ssgiTrace);for(unsigned i=0;i<2;++i){drop(ssgiAccumRT[i]);drop(ssgiAccum[i]);drop(ssgiMetaRT[i]);drop(ssgiMeta[i]);}
 drop(ssgiTracePS);drop(ssgiTemporalPS);drop(ssgiCompositePS);hw=hh=0;historyValid=false;
 V2::Release();
}
static void Reset(){if(nativeRsmReset)nativeRsmReset();lastNativeRsm=false;Release();WosIndirectMaterials::Reset();WosIndirectMaterials::ResetGIProvider();materialFrames=0;WosIndirectCamera::Reset();lastCameraScene=0;memset(skippedMasks,0,sizeof(skippedMasks));rendered=busy=false;frames=failures=historyFrames=historyResets=frameIndex=presentFrames=bloomSeen=skippedInputs=skippedSource=lastInputFlags=0;lastLog=0;lastStatusFrames=0;observedPresentFps=0;fastGlobalsFrames=0;cpuSum=cpuMs=0;lastCapture.clear();}
static bool Create(IDirect3DDevice9*d,UINT w,UINT h){
 if(owner==d&&width==w&&height==h&&gatherPS&&compositePS&&ssgiTracePS&&ssgiTemporalPS&&ssgiCompositePS)return true;
 Release();width=w;height=h;gw=(w+3)/4;gh=(h+3)/4;hw=(w+1)/2;hh=(h+1)/2;
 auto a=WosCascadeProbe::ReadFile(WosProbe::Root()+"\\indirect\\indirect_gather.cso");auto b=WosCascadeProbe::ReadFile(WosProbe::Root()+"\\indirect\\indirect_composite.cso");
 if(a.empty()||b.empty()||FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(a.data()),&gatherPS))||FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(b.data()),&compositePS))){Release();return false;}
 auto shader=[&](const char*name,IDirect3DPixelShader9*&out){auto bytes=WosCascadeProbe::ReadFile(WosProbe::Root()+"\\indirect\\"+name);return !bytes.empty()&&bytes.size()%4==0&&SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(bytes.data()),&out));};
 if(!shader("indirect_ssgi_trace.cso",ssgiTracePS)||!shader("indirect_ssgi_temporal.cso",ssgiTemporalPS)||!shader("indirect_ssgi_composite.cso",ssgiCompositePS)){Release();return false;}
 if(FAILED(d->CreateTexture(gw,gh,1,D3DUSAGE_RENDERTARGET,D3DFMT_A32B32G32R32F,D3DPOOL_DEFAULT,&gather,nullptr))||FAILED(gather->GetSurfaceLevel(0,&gatherRT))||FAILED(d->CreateTexture(w,h,1,D3DUSAGE_RENDERTARGET,D3DFMT_A16B16G16R16F,D3DPOOL_DEFAULT,&composite,nullptr))||FAILED(composite->GetSurfaceLevel(0,&compositeRT))){Release();return false;}
 auto target=[&](IDirect3DTexture9*&t,IDirect3DSurface9*&s){return SUCCEEDED(d->CreateTexture(hw,hh,1,D3DUSAGE_RENDERTARGET,D3DFMT_A16B16G16R16F,D3DPOOL_DEFAULT,&t,nullptr))&&SUCCEEDED(t->GetSurfaceLevel(0,&s));};
 bool ok=target(ssgiTrace,ssgiTraceRT);for(unsigned i=0;i<2&&ok;++i)ok=target(ssgiAccum[i],ssgiAccumRT[i])&&target(ssgiMeta[i],ssgiMetaRT[i]);
 if(!ok){Release();return false;}
 owner=d;historyValid=false;return true;
}
struct State {
 IDirect3DDevice9*d;IDirect3DStateBlock9*block=nullptr;IDirect3DSurface9*rt[4]={},*ds=nullptr;IDirect3DPixelShader9*ps=nullptr;D3DVIEWPORT9 vp={};unsigned slots=0;bool valid=false;
 explicit State(IDirect3DDevice9*device):d(device){
  D3DCAPS9 caps={};if(FAILED(d->GetDeviceCaps(&caps))||FAILED(d->GetViewport(&vp))||FAILED(d->CreateStateBlock(D3DSBT_ALL,&block))||FAILED(block->Capture()))return;
  slots=(std::min)(caps.NumSimultaneousRTs,DWORD(4));
  for(unsigned i=0;i<slots;++i){HRESULT h=d->GetRenderTarget(i,&rt[i]);if(FAILED(h)&&h!=D3DERR_NOTFOUND)return;}
  HRESULT h=d->GetDepthStencilSurface(&ds);if(FAILED(h)&&h!=D3DERR_NOTFOUND)return;
  if(FAILED(d->GetPixelShader(&ps)))return;valid=true;
 }
 bool Restore(){
  if(!valid)return false;bool ok=SUCCEEDED(d->SetDepthStencilSurface(nullptr));
  for(unsigned i=0;i<slots;++i)ok=SUCCEEDED(d->SetRenderTarget(i,rt[i]))&&ok;
  ok=SUCCEEDED(d->SetDepthStencilSurface(ds))&&ok;ok=SUCCEEDED(block->Apply())&&ok;ok=SUCCEEDED(g_original_set_pixel_shader(d,ps))&&ok;ok=SUCCEEDED(d->SetViewport(&vp))&&ok;valid=false;return ok;
 }
 ~State(){if(valid)Restore();for(auto*r:rt)if(r)r->Release();if(ds)ds->Release();if(ps)ps->Release();if(block)block->Release();}
};
static bool Quad(IDirect3DDevice9*d,unsigned w,unsigned h){
 struct V{float x,y,z,rhw,u,v;};const V q[]={{-.5f,-.5f,0,1,0,0},{float(w)-.5f,-.5f,0,1,1,0},{-.5f,float(h)-.5f,0,1,0,1},{float(w)-.5f,float(h)-.5f,0,1,1,1}};
 D3DVIEWPORT9 vp={0,0,w,h,0,1};return SUCCEEDED(d->SetViewport(&vp))&&SUCCEEDED(g_original_draw_primitive_up(d,D3DPT_TRIANGLESTRIP,2,q,sizeof(V)));
}
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/ssgi_v2.h"
// Called only for F7. The source is the same owned INTZ as the lighting passes;
// the copy is taken before bloom, while State restores every game binding.
static bool CopyDepth(IDirect3DDevice9*d,IDirect3DTexture9*depth){
 if(captureDepth)captureDepth->Release();captureDepth=nullptr;
 if(!depthCopyPS){auto bytes=WosCascadeProbe::ReadFile(WosProbe::Root()+"\\indirect\\indirect_depth_copy.cso");if(bytes.empty()||FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(bytes.data()),&depthCopyPS)))return false;}
 IDirect3DTexture9*t=nullptr;IDirect3DSurface9*rt=nullptr;
 if(FAILED(d->CreateTexture(width,height,1,D3DUSAGE_RENDERTARGET,D3DFMT_R32F,D3DPOOL_DEFAULT,&t,nullptr)))return false;
 if(FAILED(t->GetSurfaceLevel(0,&rt))){t->Release();return false;}
 State state(d);bool ok=state.valid;
 auto check=[&](HRESULT h){ok=SUCCEEDED(h)&&ok;};
 if(ok){
  check(d->SetDepthStencilSurface(nullptr));for(unsigned i=1;i<state.slots;++i)check(d->SetRenderTarget(i,nullptr));
  check(d->SetRenderTarget(0,rt));check(d->SetVertexShader(nullptr));check(d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1));
  for(auto rs:{D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHABLENDENABLE,D3DRS_ALPHATESTENABLE,D3DRS_STENCILENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_SRGBWRITEENABLE,D3DRS_FOGENABLE,D3DRS_CLIPPLANEENABLE,D3DRS_SEPARATEALPHABLENDENABLE})check(d->SetRenderState(rs,0));
  check(d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));check(d->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID));check(d->SetRenderState(D3DRS_COLORWRITEENABLE,15));check(d->SetRenderState(D3DRS_MULTISAMPLEMASK,0xffffffff));
  check(d->SetTexture(0,depth));for(unsigned i=1;i<4;++i)check(d->SetTexture(i,nullptr));
  check(d->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT));check(d->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT));check(d->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_NONE));
  check(d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));check(d->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));check(d->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,0));
  check(g_original_set_pixel_shader(d,depthCopyPS));if(ok)ok=Quad(d,width,height);
  ok=state.Restore()&&ok;
 }
 rt->Release();if(ok)captureDepth=t;else t->Release();return ok;
}
// Packs the SSGI constants. Positions are relative to this frame's camera; the
// previous VP is rebased onto them in double precision, so reprojection does not
// lose the camera translation to float cancellation.
static bool Constants(const WosIndirectCamera::Camera&camera,bool materials,bool history,float c[84]){
 std::memset(c,0,84*sizeof(float));memcpy(c,camera.relativeInverse,64);
 c[20]=1.f/width;c[21]=1.f/height;c[22]=radius;c[23]=maxDistance;c[24]=bounce;c[25]=ssgiMultiBounce;c[26]=4;c[27]=1.f;
 c[28]=1.f/hw;c[29]=1.f/hh;c[30]=float(frameIndex%1024);c[31]=.01f;c[48]=float(debugView);c[49]=8.f;c[52]=materials?1.f:0.f;
 WosShadowTransform::Matrix4 m;memcpy(m.v,camera.relativeInverse,64);double inv[16];if(!WosCascades::Inverse(m,inv))return false;
 for(unsigned i=0;i<16;++i)c[32+i]=static_cast<float>(inv[i]);
 double delta[3];for(unsigned i=0;i<3;++i)delta[i]=camera.worldOrigin[i]-prevOrigin[i];
 if(history&&!(delta[0]*delta[0]+delta[1]*delta[1]+delta[2]*delta[2]<32.*32.))history=false;
 if(history){
  memcpy(m.v,prevRelativeInverse,64);double prev[16];history=WosCascades::Inverse(m,prev);
  for(unsigned r=0;r<4&&history;++r)for(unsigned col=0;col<4;++col){double v=prev[r*4+col];if(col==3)v+=prev[r*4]*delta[0]+prev[r*4+1]*delta[1]+prev[r*4+2]*delta[2];if(!std::isfinite(v)){history=false;break;}c[60+r*4+col]=static_cast<float>(v);}
  for(unsigned i=0;i<3;++i)c[76+i]=static_cast<float>(delta[i]);
 }
 c[56]=history?1.f:0.f;c[57]=ssgiHistory;c[58]=.03f;c[59]=.1f;
 // Variance clip: gamma 3 (lenient) to 1 (strict); sigma floor is a quarter of the history plus 0.01 HDR.
 c[80]=ssgiAntiGhost>0?3.f-2.f*ssgiAntiGhost:0.f;c[81]=.25f;c[82]=.01f;
 return true;
}
static bool Render(IDirect3DDevice9*d,IDirect3DBaseTexture9*source,IDirect3DBaseTexture9*depth,const WosIndirectCamera::Camera&camera,IDirect3DTexture9*normal=nullptr,IDirect3DTexture9*rho=nullptr){
 if(V2::s.version==2){
  const bool ok=V2::Render(d,source,depth,camera,normal,rho,presentFrames==lastRenderPresent+1);
  if(ok)lastRenderPresent=presentFrames;
  return ok;
 }
 State state(d);if(!state.valid||state.slots<2)return false;busy=true;
 const bool materials=normal&&rho;
 float c[84];bool ok=Constants(camera,materials,historyValid&&presentFrames==lastRenderPresent+1,c);
 const bool usedHistory=ok&&c[56]>.5f;const unsigned previous=historyIndex,current=historyIndex^1u;
 auto check=[&](HRESULT h){ok=SUCCEEDED(h)&&ok;};
 check(d->SetDepthStencilSurface(nullptr));for(unsigned i=1;i<state.slots;++i)check(d->SetRenderTarget(i,nullptr));
 check(d->SetVertexShader(nullptr));check(d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1));
 const D3DRENDERSTATETYPE zeros[]={D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHABLENDENABLE,D3DRS_ALPHATESTENABLE,D3DRS_STENCILENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_SRGBWRITEENABLE,D3DRS_FOGENABLE,D3DRS_CLIPPLANEENABLE,D3DRS_SEPARATEALPHABLENDENABLE};for(auto s:zeros)check(d->SetRenderState(s,0));
 check(d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));check(d->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID));check(d->SetRenderState(D3DRS_COLORWRITEENABLE,15));check(d->SetRenderState(D3DRS_COLORWRITEENABLE1,15));check(d->SetRenderState(D3DRS_MULTISAMPLEMASK,0xffffffff));
 for(unsigned i=0;i<6;++i){check(d->SetTexture(i,nullptr));check(d->SetSamplerState(i,D3DSAMP_MINFILTER,D3DTEXF_POINT));check(d->SetSamplerState(i,D3DSAMP_MAGFILTER,D3DTEXF_POINT));check(d->SetSamplerState(i,D3DSAMP_MIPFILTER,D3DTEXF_NONE));check(d->SetSamplerState(i,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));check(d->SetSamplerState(i,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));check(d->SetSamplerState(i,D3DSAMP_SRGBTEXTURE,0));}
 check(d->SetPixelShaderConstantF(0,c,21));
 // 1. Trace at half resolution.
 check(d->SetRenderTarget(0,ssgiTraceRT));check(d->SetTexture(0,source));check(d->SetTexture(1,depth));check(d->SetTexture(2,materials?normal:nullptr));
 // Multi-bounce: a donor also re-emits the bounce it gathered last frame. The
 // trace reads it only when c14.x says the history is valid.
 check(d->SetTexture(3,usedHistory?ssgiAccum[previous]:nullptr));check(d->SetTexture(4,materials?rho:nullptr));
 check(g_original_set_pixel_shader(d,ssgiTracePS));if(ok)ok=Quad(d,hw,hh);
 // 2. Accumulate into the current history set from the previous one.
 check(d->SetRenderTarget(0,ssgiAccumRT[current]));check(d->SetRenderTarget(1,ssgiMetaRT[current]));
 check(d->SetTexture(0,ssgiTrace));check(d->SetTexture(3,ssgiAccum[previous]));check(d->SetTexture(4,ssgiMeta[previous]));
 check(g_original_set_pixel_shader(d,ssgiTemporalPS));if(ok)ok=Quad(d,hw,hh);
 // 3. Denoise, upsample and add to the scene.
 check(d->SetRenderTarget(1,nullptr));check(d->SetRenderTarget(0,compositeRT));
 check(d->SetTexture(0,source));check(d->SetTexture(1,ssgiAccum[current]));check(d->SetTexture(2,depth));check(d->SetTexture(3,ssgiMeta[current]));check(d->SetTexture(4,materials?rho:nullptr));check(d->SetTexture(5,materials?normal:nullptr));
 check(g_original_set_pixel_shader(d,ssgiCompositePS));if(ok)ok=Quad(d,width,height);
 for(unsigned i=0;i<6;++i)check(d->SetTexture(i,nullptr));
 ok=state.Restore()&&ok;busy=false;
 if(ok){historyIndex=current;historyValid=true;lastRenderPresent=presentFrames;memcpy(prevRelativeInverse,camera.relativeInverse,sizeof(prevRelativeInverse));for(unsigned i=0;i<3;++i)prevOrigin[i]=camera.worldOrigin[i];++frameIndex;if(usedHistory)++historyFrames;else ++historyResets;}
 else historyValid=false;
 return ok;
}
// World axes are retained, but positions are relative to the camera. The
// double-precision rebasing happens in Decode before conversion to float.
static bool RenderCamera(IDirect3DDevice9*d,IDirect3DBaseTexture9*source,IDirect3DBaseTexture9*depth,const WosIndirectCamera::Camera&camera,IDirect3DTexture9*normal=nullptr,IDirect3DTexture9*rho=nullptr){
 nativeRsmInputs={};lastNativeRsm=nativeRsmRender&&nativeRsmRender(d,source,depth,camera,normal,rho);
 if(lastNativeRsm){historyValid=false;return true;}
 return Render(d,source,depth,camera,normal,rho);
}
struct DrawScope {
 IDirect3DDevice9*d=nullptr;IDirect3DBaseTexture9*original=nullptr;IDirect3DTexture9*sceneDepth=nullptr;bool changed=false;
 explicit DrawScope(IDirect3DDevice9*dev){
  if(!enabled||busy||rendered||WosProbe::suppress||ClassifyShader(g_current_shader)!=kShaderBloom)return;
  ++bloomSeen;
  if(FAILED(dev->GetTexture(0,&original))||!original)return;
  IDirect3DTexture9*t=nullptr;D3DSURFACE_DESC desc={};if(FAILED(original->QueryInterface(IID_IDirect3DTexture9,reinterpret_cast<void**>(&t))))return;HRESULT h=t->GetLevelDesc(0,&desc);t->Release();
  if(FAILED(h)||desc.Format!=D3DFMT_A16B16G16R16F||desc.Width<320||desc.Height<200||desc.Width>4096||desc.Height>4096){++skippedSource;return;}
  WosIndirectCamera::Camera camera;
  bool matched=WosIndirectCamera::Acquire(desc.Width,desc.Height,camera,sceneDepth,WosIndirectCamera::CurrentScene());
  lastCameraScene=matched?camera.scene:0;
  lastInputFlags=(matched?1:0)|(sceneDepth?2:0)|(WosIndirectCamera::frameReady?4:0)|(sceneDepth?8:0);
  if(lastInputFlags!=15){++skippedInputs;++skippedMasks[lastInputFlags];status="Indirect waiting for matching current scene camera/depth";return;}
  IDirect3DTexture9*normal=nullptr,*rho=nullptr;bool materialInput=WosIndirectMaterials::ResolveForGI(dev,sceneDepth,camera.scene,normal,rho);
  cpuMs=0;WosCascadeProbe::CpuTimer timer(cpuMs);
  if(!ssgiSettingsLoaded)LoadSSGISettings();
  if(!Create(dev,desc.Width,desc.Height)||!RenderCamera(dev,original,sceneDepth,camera,normal,rho)){++failures;status="Indirect pass failed; original image retained";return;}
  if(WosProbe::trace&&lastCapture!=WosProbe::folder){ReleaseCapture();pendingCapture=lastCapture=WosProbe::folder;captureSource=original;captureSource->AddRef();dev->GetTexture(2,&captureAdapt);captureCamera=camera;captureNativeRsm=lastNativeRsm;captureNativeRsmInputs=nativeRsmInputs;captureSettings[0]=float(debugView);captureSettings[1]=bounce;captureSettings[2]=ambient;captureSettings[3]=radius;captureSettings[4]=maxDistance;if(materialInput){captureNormal=normal;captureRho=rho;normal->AddRef();rho->AddRef();}CopyDepth(dev,sceneDepth);}
  d=dev;if(SUCCEEDED(d->SetTexture(0,composite))){changed=true;rendered=true;if(materialInput)++materialFrames;status=lastNativeRsm?"F5: indirect ON (native shadow-map bounce + environment fill)":"F5: SSGI ON (traced screen-space bounce, temporal accumulation)";}else ++failures;
 }
 ~DrawScope(){if(sceneDepth)sceneDepth->Release();if(changed)d->SetTexture(0,original);if(original)original->Release();}
};
static void WriteStatus(bool force=false){
 ULONGLONG now=GetTickCount64();if(!force&&now-lastLog<1000)return;if(lastLog&&now>lastLog)observedPresentFps=1000.0*(presentFrames-lastStatusFrames)/double(now-lastLog);lastStatusFrames=presentFrames;lastLog=now;
 FILE*f=std::fopen((WosProbe::Root()+"\\indirect_status.json").c_str(),"wb");if(!f)return;
 std::fprintf(f,"{\"revision\":19,\"technique\":\"%s\",\"trace_width\":%u,\"trace_height\":%u,\"history_frames\":%u,\"history_resets\":%u,\"max_history\":%u,\"enabled\":%s,\"status\":\"%s\",\"present_frames\":%u,\"rendered_frames\":%u,\"bloom_draws\":%u,\"skipped_inputs\":%u,\"skipped_source\":%u,\"last_input_flags\":%u,\"width\":%u,\"height\":%u,\"gather_width\":%u,\"gather_height\":%u,\"mean_cpu_submission_ms\":%.4f,\"failed_passes\":%u,\"bounce\":%.3f,\"multi_bounce\":%.3f,\"ambient\":%.3f,\"radius\":%.2f,\"max_distance\":%.2f,\"debug_view\":%d,",V2::s.version==2?"ssgi_v2_visibility_mask":"ssgi_trace_temporal",V2::s.version==2?V2::tw:hw,V2::s.version==2?V2::th:hh,historyFrames,historyResets,unsigned(ssgiHistory),enabled?"true":"false",status,presentFrames,frames,bloomSeen,skippedInputs,skippedSource,lastInputFlags,width,height,gw,gh,frames?cpuSum/frames:0,failures,bounce,ssgiMultiBounce,ambient,radius,maxDistance,debugView);
 std::fprintf(f,"\"fixed_globals_fast_frames\":%u,\"game_foreground\":%s,",fastGlobalsFrames,GetForegroundWindow()==g_window?"true":"false");
 std::fprintf(f,"\"native_material_frames\":%u,\"native_material_draws\":%u,\"native_material_guards\":%u,\"native_material_replays\":%u,\"native_material_unknown\":%u,\"native_material_failures\":%u,\"native_material_status\":\"%s\",\"observed_present_fps\":%.2f,",materialFrames,WosIndirectMaterials::draws,WosIndirectMaterials::guardDraws,WosIndirectMaterials::replayDraws,WosIndirectMaterials::unknownDraws,WosIndirectMaterials::failures,WosIndirectMaterials::status,observedPresentFps);
 std::fprintf(f,"\"dynamic_material_guards\":%u,\"dynamic_material_guard_failures\":%u,\"last_dynamic_guard_reason\":\"%s\",",WosIndirectMaterials::dynamicGuards,WosIndirectMaterials::dynamicGuardFailures,WosIndirectMaterials::lastGuardReason.c_str());
 std::fprintf(f,"\"depth_source\":\"current_scene_bound_INTZ\",\"camera_source\":\"engine_scene_190\",\"coordinate_space\":\"camera_relative_world_axes\",\"camera_scene\":%u,\"camera_captures\":%u,\"camera_rejected\":%u,\"camera_status\":\"%s\",\"skipped_input_masks\":[",lastCameraScene,WosIndirectCamera::captures,WosIndirectCamera::rejected,WosIndirectCamera::status);
 for(unsigned i=0;i<16;++i)std::fprintf(f,"%s%u",i?",":"",skippedMasks[i]);std::fputs("]}\n",f);std::fclose(f);
}
static void FrameEnd(){
 if(enabled&&WosIndirectCamera::FixedGlobalsReady())++fastGlobalsFrames;
 WosIndirectCamera::Clear();if(enabled)++presentFrames;if(rendered){++frames;cpuSum+=cpuMs;}SaveCapture();rendered=false;
 bool toggled=false;if(GetForegroundWindow()==g_window&&(GetAsyncKeyState(VK_F5)&1)){enabled=!enabled;Reset();if(enabled)WosCascadeProbe::Reset();status=enabled?"F5: indirect armed; waiting for current camera and scene depth":"F5: indirect lighting OFF";toggled=true;}
 if(enabled||toggled)WriteStatus(toggled);
}
}
