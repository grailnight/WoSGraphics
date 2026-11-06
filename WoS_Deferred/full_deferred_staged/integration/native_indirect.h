#pragma once
#include "indirect_camera.h"
#include "indirect_materials.h"
// Two fullscreen passes before the existing tone mapper. No scene replay or temporal history.
namespace WosIndirect {
static bool enabled=false,busy=false,rendered=false,lastNativeRsm=false;
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
 if(f){std::fprintf(f,"{\"revision\":18,\"receiver_grid\":\"%s\",\"native_rsm_used\":%s,\"settings\":{\"debug_view\":%d,\"bounce\":%.3f,\"ambient\":%.3f,\"radius\":%.2f,\"max_distance\":%.2f},",captureNativeRsm?"quarter_vpos_4x4_v1":"legacy_uv_floor",captureNativeRsm?"true":"false",int(captureSettings[0]),captureSettings[1],captureSettings[2],captureSettings[3],captureSettings[4]);SaveTexture(owner,captureSource,pendingCapture+"\\indirect_source.raw",f,"source");std::fputs(",",f);SaveTexture(owner,gather,pendingCapture+"\\indirect_gather.raw",f,"gather");std::fputs(",",f);SaveTexture(owner,composite,pendingCapture+"\\indirect_composite.raw",f,"composite");std::fputs(",",f);if(captureAdapt)SaveTexture(owner,captureAdapt,pendingCapture+"\\indirect_adaptation.raw",f,"adaptation");else std::fputs("\"adaptation\":null",f);std::fputs(",",f);SaveTexture(owner,captureDepth,pendingCapture+"\\indirect_depth.raw",f,"depth");std::fputs(",",f);SaveTexture(owner,captureNormal,pendingCapture+"\\indirect_normal.raw",f,"normal");std::fputs(",",f);SaveTexture(owner,captureRho,pendingCapture+"\\indirect_rho.raw",f,"rho");std::fprintf(f,",\"camera\":{\"scene\":%u,\"coordinate_space\":\"camera_relative_world_axes\",\"position\":[0,0,0],\"world_origin\":[%.17g,%.17g,%.17g],\"inverse_vp\":[",captureCamera.scene,captureCamera.worldOrigin[0],captureCamera.worldOrigin[1],captureCamera.worldOrigin[2]);for(unsigned i=0;i<16;++i)std::fprintf(f,"%s%.9g",i?",":"",captureCamera.relativeInverse[i]);std::fprintf(f,"]},\"native_rsm_inputs\":{\"host_frame\":%llu,\"maps\":[",captureNativeRsmInputs.frame);for(unsigned i=0;i<2;++i)std::fprintf(f,"%s{\"slot\":%u,\"eligible\":%s,\"normal_generation\":%llu,\"flux_generation\":%llu}",i?",":"",i,captureNativeRsmInputs.eligible[i]?"true":"false",captureNativeRsmInputs.normalGeneration[i],captureNativeRsmInputs.fluxGeneration[i]);std::fputs("]}}\n",f);std::fclose(f);}
 ReleaseCapture();
}
static void Release(){
 ReleaseCapture();
 if(gatherRT)gatherRT->Release();if(compositeRT)compositeRT->Release();if(gather)gather->Release();if(composite)composite->Release();if(gatherPS)gatherPS->Release();if(compositePS)compositePS->Release();if(depthCopyPS)depthCopyPS->Release();
 gatherRT=compositeRT=nullptr;gather=composite=nullptr;gatherPS=compositePS=depthCopyPS=nullptr;owner=nullptr;width=height=gw=gh=0;
}
static void Reset(){if(nativeRsmReset)nativeRsmReset();lastNativeRsm=false;Release();WosIndirectMaterials::Reset();materialFrames=0;WosIndirectCamera::Reset();lastCameraScene=0;memset(skippedMasks,0,sizeof(skippedMasks));rendered=busy=false;frames=failures=presentFrames=bloomSeen=skippedInputs=skippedSource=lastInputFlags=0;lastLog=0;lastStatusFrames=0;observedPresentFps=0;fastGlobalsFrames=0;cpuSum=cpuMs=0;lastCapture.clear();}
static bool Create(IDirect3DDevice9*d,UINT w,UINT h){
 if(owner==d&&width==w&&height==h&&gatherPS&&compositePS)return true;
 Release();width=w;height=h;gw=(w+3)/4;gh=(h+3)/4;
 auto a=WosCascadeProbe::ReadFile(WosProbe::Root()+"\\indirect\\indirect_gather.cso");auto b=WosCascadeProbe::ReadFile(WosProbe::Root()+"\\indirect\\indirect_composite.cso");
 if(a.empty()||b.empty()||FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(a.data()),&gatherPS))||FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(b.data()),&compositePS))){Release();return false;}
 if(FAILED(d->CreateTexture(gw,gh,1,D3DUSAGE_RENDERTARGET,D3DFMT_A32B32G32R32F,D3DPOOL_DEFAULT,&gather,nullptr))||FAILED(gather->GetSurfaceLevel(0,&gatherRT))||FAILED(d->CreateTexture(w,h,1,D3DUSAGE_RENDERTARGET,D3DFMT_A16B16G16R16F,D3DPOOL_DEFAULT,&composite,nullptr))||FAILED(composite->GetSurfaceLevel(0,&compositeRT))){Release();return false;}
 owner=d;return true;
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
static bool Render(IDirect3DDevice9*d,IDirect3DBaseTexture9*source,IDirect3DBaseTexture9*depth,const float inverse[16],const float camera[3],IDirect3DTexture9*normal=nullptr,IDirect3DTexture9*rho=nullptr){
 State state(d);if(!state.valid)return false;busy=true;
 float c[56]={};c[52]=normal&&rho?1.f:0.f;memcpy(c,inverse,64);memcpy(c+16,camera,12);c[20]=1.f/width;c[21]=1.f/height;c[22]=radius;c[23]=maxDistance;c[24]=bounce;c[25]=ambient;c[26]=4;c[27]=1.f;c[28]=1.f/gw;c[29]=1.f/gh;c[30]=.20f;c[31]=.03f;c[48]=float(debugView);c[49]=8.f;
 WosShadowTransform::Matrix4 m;memcpy(m.v,inverse,64);double inv[16];bool ok=WosCascades::Inverse(m,inv);for(unsigned i=0;i<16&&ok;++i)c[32+i]=static_cast<float>(inv[i]);
 auto check=[&](HRESULT h){ok=SUCCEEDED(h)&&ok;};
 check(d->SetDepthStencilSurface(nullptr));for(unsigned i=1;i<state.slots;++i)check(d->SetRenderTarget(i,nullptr));
 check(d->SetRenderTarget(0,gatherRT));check(d->SetVertexShader(nullptr));check(d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1));
 const D3DRENDERSTATETYPE zeros[]={D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHABLENDENABLE,D3DRS_ALPHATESTENABLE,D3DRS_STENCILENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_SRGBWRITEENABLE,D3DRS_FOGENABLE,D3DRS_CLIPPLANEENABLE,D3DRS_SEPARATEALPHABLENDENABLE};for(auto s:zeros)check(d->SetRenderState(s,0));
 check(d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));check(d->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID));check(d->SetRenderState(D3DRS_COLORWRITEENABLE,15));check(d->SetRenderState(D3DRS_MULTISAMPLEMASK,0xffffffff));
 for(unsigned i=0;i<4;++i){check(d->SetTexture(i,nullptr));check(d->SetSamplerState(i,D3DSAMP_MINFILTER,D3DTEXF_POINT));check(d->SetSamplerState(i,D3DSAMP_MAGFILTER,D3DTEXF_POINT));check(d->SetSamplerState(i,D3DSAMP_MIPFILTER,D3DTEXF_NONE));check(d->SetSamplerState(i,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));check(d->SetSamplerState(i,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));check(d->SetSamplerState(i,D3DSAMP_SRGBTEXTURE,0));}
 check(d->SetPixelShaderConstantF(0,c,14));check(d->SetTexture(0,source));check(d->SetTexture(1,depth));check(d->SetTexture(2,normal));check(d->SetTexture(3,rho));check(g_original_set_pixel_shader(d,gatherPS));if(ok)ok=Quad(d,gw,gh);
 check(d->SetTexture(1,nullptr));check(d->SetRenderTarget(0,compositeRT));check(d->SetTexture(1,gather));check(d->SetTexture(2,depth));check(g_original_set_pixel_shader(d,compositePS));if(ok)ok=Quad(d,width,height);
 ok=state.Restore()&&ok;busy=false;return ok;
}
// World axes are retained, but positions are relative to the camera. The
// double-precision rebasing happens in Decode before conversion to float.
static bool RenderCamera(IDirect3DDevice9*d,IDirect3DBaseTexture9*source,IDirect3DBaseTexture9*depth,const WosIndirectCamera::Camera&camera,IDirect3DTexture9*normal=nullptr,IDirect3DTexture9*rho=nullptr){
 nativeRsmInputs={};lastNativeRsm=nativeRsmRender&&nativeRsmRender(d,source,depth,camera,normal,rho);
 if(lastNativeRsm)return true;
 const float origin[3]={0,0,0};return Render(d,source,depth,camera.relativeInverse,origin,normal,rho);
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
  IDirect3DTexture9*normal=nullptr,*rho=nullptr;bool materialInput=WosIndirectMaterials::Get(sceneDepth,camera.scene,normal,rho);
  cpuMs=0;WosCascadeProbe::CpuTimer timer(cpuMs);
  if(!Create(dev,desc.Width,desc.Height)||!RenderCamera(dev,original,sceneDepth,camera,normal,rho)){++failures;status="Indirect pass failed; original image retained";return;}
  if(WosProbe::trace&&lastCapture!=WosProbe::folder){ReleaseCapture();pendingCapture=lastCapture=WosProbe::folder;captureSource=original;captureSource->AddRef();dev->GetTexture(2,&captureAdapt);captureCamera=camera;captureNativeRsm=lastNativeRsm;captureNativeRsmInputs=nativeRsmInputs;captureSettings[0]=float(debugView);captureSettings[1]=bounce;captureSettings[2]=ambient;captureSettings[3]=radius;captureSettings[4]=maxDistance;if(materialInput){captureNormal=normal;captureRho=rho;normal->AddRef();rho->AddRef();}CopyDepth(dev,sceneDepth);}
  d=dev;if(SUCCEEDED(d->SetTexture(0,composite))){changed=true;rendered=true;if(materialInput)++materialFrames;status=lastNativeRsm?"F5: indirect ON (native shadow-map bounce + environment fill)":"F5: indirect lighting ON (screen-space bounce + environment fill)";}else ++failures;
 }
 ~DrawScope(){if(sceneDepth)sceneDepth->Release();if(changed)d->SetTexture(0,original);if(original)original->Release();}
};
static void WriteStatus(bool force=false){
 ULONGLONG now=GetTickCount64();if(!force&&now-lastLog<1000)return;if(lastLog&&now>lastLog)observedPresentFps=1000.0*(presentFrames-lastStatusFrames)/double(now-lastLog);lastStatusFrames=presentFrames;lastLog=now;
 FILE*f=std::fopen((WosProbe::Root()+"\\indirect_status.json").c_str(),"wb");if(!f)return;
 std::fprintf(f,"{\"revision\":18,\"enabled\":%s,\"status\":\"%s\",\"present_frames\":%u,\"rendered_frames\":%u,\"bloom_draws\":%u,\"skipped_inputs\":%u,\"skipped_source\":%u,\"last_input_flags\":%u,\"width\":%u,\"height\":%u,\"gather_width\":%u,\"gather_height\":%u,\"mean_cpu_submission_ms\":%.4f,\"failed_passes\":%u,\"bounce\":%.3f,\"ambient\":%.3f,\"radius\":%.2f,\"max_distance\":%.2f,\"debug_view\":%d,",enabled?"true":"false",status,presentFrames,frames,bloomSeen,skippedInputs,skippedSource,lastInputFlags,width,height,gw,gh,frames?cpuSum/frames:0,failures,bounce,ambient,radius,maxDistance,debugView);
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
