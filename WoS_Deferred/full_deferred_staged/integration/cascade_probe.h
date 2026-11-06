#pragma once
#include "shadow_packets.h"
#include "shadow_cascades.h"
#include "shadow_targets.h"
#include "shadow_readback.h"
#include <unordered_set>
namespace WosCascadeProbe {
using WosShadowTransform::Matrix4;
static bool active=false,ready=false,busy=false,continuous=false;
static const char *status="F10: capture four experimental shadow maps";
static IDirect3DDevice9 *device=nullptr;
static bool verified=false;
static WosShadowTargets::Set targets;
static WosCascades::Plan plan,historyPlan;
static bool historyValid=false;
static unsigned updateMask=15,updatePair=0;
static float historyDepthPlane[4]={};
static WosCascades::V3 cameraPosition={},historyPosition={};
static unsigned refreshReason=0;
static bool CameraPosition(const Matrix4&vp,WosCascades::V3&out){
 double inv[16];WosCascades::V3 a,b;
 if(!WosCascades::Inverse(vp,inv)||!WosCascades::Unproject(inv,0,0,0,a)||!WosCascades::Unproject(inv,0,0,1,b))return false;
 auto w=[&](WosCascades::V3 p){return p.x*vp.v[3]+p.y*vp.v[7]+p.z*vp.v[11]+vp.v[15];};
 double wa=w(a),wb=w(b);if(!std::isfinite(wb-wa)||std::abs(wb-wa)<1e-8)return false;
 out=a-(b-a)*(wa/(wb-wa));return std::isfinite(out.x)&&std::isfinite(out.y)&&std::isfinite(out.z);
}
static unsigned gpuDraws=0,lastGpuDraws=0;
static double prepareCpuMs=0,replayCpuMs=0;
struct CpuTimer {
 double&total;LARGE_INTEGER start={};
 explicit CpuTimer(double&value):total(value){QueryPerformanceCounter(&start);}
 ~CpuTimer(){LARGE_INTEGER end,freq;QueryPerformanceCounter(&end);QueryPerformanceFrequency(&freq);if(freq.QuadPart>0)total+=1000.0*double(end.QuadPart-start.QuadPart)/freq.QuadPart;}
};

static WosShadowPackets::Pass source,passes[4];
static std::unordered_map<std::string,size_t> instanceIndex;
static std::vector<unsigned char> vsCodes[2];
static std::unordered_map<IDirect3DVertexShader9*,int> shaderKinds;
static std::string folder;
static uint32_t farScene=0,mainScene=0;
static float cameraDepthPlane[4]={};
static void SelectUpdates(){
 updateMask=15;refreshReason=1;
 if(!continuous||active||!historyValid)return;
 double alignment=0;for(unsigned i=0;i<3;++i)alignment+=double(cameraDepthPlane[i])*historyDepthPlane[i];
 refreshReason=2;if(alignment<.995)return;
 auto movement=cameraPosition-historyPosition;refreshReason=3;if(WosCascades::Dot(movement,movement)>16)return;
 refreshReason=4;if(std::abs(plan.cameraNear-historyPlan.cameraNear)>.001)return;
 refreshReason=0;
 updateMask=1u<<(updatePair&3);
 for(unsigned i=0;i<4;++i)if(!(updateMask&(1u<<i)))plan.cascades[i]=historyPlan.cascades[i];
}
static unsigned skippedAnimated=0;
static unsigned replayed=0,skippedShader=0,skippedInstance=0,failed=0,sourceNodes=0,excludedNodes=0;
static void Release(){historyValid=false;updateMask=15;updatePair=0;verified=false;targets.Reset();source={};for(auto&p:passes)p={};instanceIndex.clear();for(auto&e:shaderKinds)e.first->Release();shaderKinds.clear();for(auto&v:vsCodes)v.clear();if(device)device->Release();device=nullptr;ready=false;}
static void Reset(){active=continuous=false;Release();}
static std::vector<unsigned char> ReadFile(const std::string &path){FILE*f=std::fopen(path.c_str(),"rb");if(!f)return{};std::fseek(f,0,SEEK_END);long n=std::ftell(f);std::rewind(f);std::vector<unsigned char>b;if(n>0&&n<1048576){b.resize(n);if(std::fread(b.data(),1,n,f)!=size_t(n))b.clear();}std::fclose(f);return b;}
static int ShaderKind(IDirect3DVertexShader9*vs){
 auto it=shaderKinds.find(vs);if(it!=shaderKinds.end())return it->second;
 int kind=-1;UINT n=0;if(vs&&SUCCEEDED(vs->GetFunction(nullptr,&n))&&n<1048576){std::vector<unsigned char>b(n);if(n&&SUCCEEDED(vs->GetFunction(b.data(),&n)))for(int i=0;i<2;++i)if(b==vsCodes[i])kind=i;}
 if(vs){vs->AddRef();shaderKinds.emplace(vs,kind);}return kind;
}
static void Prepare(IDirect3DDevice9*d,uintptr_t caller){
 if((!active&&!continuous)||ready||busy||caller!=0x009dcf3a)return;
 prepareCpuMs=replayCpuMs=0;CpuTimer preparationTimer(prepareCpuMs);
 gpuDraws=skippedAnimated=0;replayed=skippedShader=skippedInstance=failed=sourceNodes=excludedNodes=0;
 source={};for(auto&p:passes)p={};instanceIndex.clear();
 std::string reason;if(!verified&&!WosEngineProbe::Verify(reason)){active=false;status="Cascade probe: executable verification failed";return;}
 verified=true;
 for(const auto &signature:WosEngineProbe::signatures){unsigned char bytes[32]={};if(!WosEngineProbe::Read(WosEngineProbe::imageBase+signature.rva,bytes,32)||memcmp(bytes,signature.bytes,32)){Reset();status="Cascades disabled: code signature changed";return;}}
 auto g=WosEngineProbe::ReadGlobals();uint32_t bounds[3]={};size_t used=0;
 if(g.valid!=7||g.current!=g.root||!g.farScene||!WosEngineProbe::Read(0x01115c64,bounds,sizeof(bounds))||!WosEngineProbe::ArenaBounds(bounds,used)){active=false;status="Cascade probe: missing scene arena";return;}
 std::vector<unsigned char> arena(used);if(!WosEngineProbe::Read(bounds[0],arena.data(),used)){active=false;status="Cascade probe: arena read failed";return;}
 auto read=[&](uint32_t p,void*out,size_t n){if(p<bounds[0]||uint64_t(p-bounds[0])+n>arena.size())return false;memcpy(out,arena.data()+p-bounds[0],n);return true;};
 Matrix4 light={},camera={},nativeFar={};uint32_t head=0;
 if(!read(g.nearScene+0x190,&light,sizeof(light))||!read(g.farScene+0x190,&nativeFar,sizeof(nativeFar))||!read(g.farScene+0x414,&head,4)){active=false;status="Cascade probe: bad shadow scene";return;}
 WosShadowPackets::Error err; if(!WosShadowPackets::Build(read,head,nativeFar,source,err)){active=false;status="Cascade probe: unsupported material queue";return;}
 std::vector<uint32_t>todo={g.root};std::unordered_set<uint32_t>seen;size_t best=0;
 while(!todo.empty()&&seen.size()<128){uint32_t p=todo.back();todo.pop_back();if(!p||!seen.insert(p).second)continue;unsigned char bytes[0x4e0];if(!read(p,bytes,sizeof(bytes)))continue;
  auto u=[&](unsigned off){uint32_t v;memcpy(&v,bytes+off,4);return v;};todo.push_back(u(0x394));todo.push_back(u(0x398));
  if(p==g.farScene)excludedNodes=u(0x3ec)+u(0x3f0);
  if(p==g.nearScene||p==g.farScene||bytes[0x4a6])continue;
  uint32_t node=u(0x414);size_t count=0;std::unordered_set<uint32_t>visited;
  while(node&&count<100000&&visited.insert(node).second){uint32_t next;if(!read(node+4,&next,4))break;node=next;++count;}
  if(count>best){best=count;mainScene=p;memcpy(&camera,bytes+0x190,sizeof(camera));}
 }
 WosCascades::Config cfg;cfg.resolution=continuous?1024:2048;
 if(!best||!WosCascades::Build(camera,light,cfg,plan)){active=false;status="Cascade probe: camera fitting failed";Release();return;}
 double planeLength=std::sqrt(double(camera.v[3])*camera.v[3]+double(camera.v[7])*camera.v[7]+double(camera.v[11])*camera.v[11]);
 if(!std::isfinite(planeLength)||planeLength<1e-8){Reset();status="Cascade camera depth invalid";return;}
 for(unsigned i=0;i<4;++i)cameraDepthPlane[i]=static_cast<float>(camera.v[i*4+3]/planeLength);
 if(!CameraPosition(camera,cameraPosition)){Reset();status="Cascade camera position invalid";return;}
 SelectUpdates();
 for(unsigned i=0;i<4;++i)if((updateMask&(1u<<i))&&!WosShadowPackets::Reproject(source,plan.cascades[i].vp,passes[i])){active=false;status="Cascade probe: reproject failed";Release();return;}
 if(vsCodes[0].empty())vsCodes[0]=ReadFile(WosProbe::Root()+"\\cascade_probe\\vs_0002.cso");if(vsCodes[1].empty())vsCodes[1]=ReadFile(WosProbe::Root()+"\\cascade_probe\\vs_0003.cso");
 if(vsCodes[0].empty()||vsCodes[1].empty()||((device!=d||targets.resolution!=cfg.resolution)&&!targets.Create(d,cfg.resolution))){active=false;status="Cascade probe: shader references or GPU targets unavailable";Release();return;}
 for(unsigned i=0;i<4;++i){if(!(updateMask&(1u<<i)))continue;WosShadowTargets::Binding binding;if(!binding.Begin(d,targets,i)||FAILED(d->Clear(0,nullptr,D3DCLEAR_ZBUFFER,0,1,0))||FAILED(binding.End())){active=false;status="Cascade probe: target initialization failed";Release();return;}}
 for(size_t i=0;i<source.instances.size();++i)instanceIndex.emplace(std::string(reinterpret_cast<const char*>(&source.instances[i].shaderWVP),64),i);
 farScene=g.farScene;sourceNodes=static_cast<unsigned>(source.nodes.size());if(device!=d){if(device)device->Release();device=d;d->AddRef();}ready=true;status="Capturing experimental cascades...";
}
template<class Draw>static void Replay(IDirect3DDevice9*d,Draw draw){
 if((!active&&!continuous)||!ready||busy||WosProbe::suppress)return;
 auto g=WosEngineProbe::ReadGlobals();if(g.valid!=7||g.current!=farScene||g.shadowActive!=1)return;
 CpuTimer replayTimer(replayCpuMs);
 DWORD zwrite=0,blend=0;if(FAILED(d->GetRenderState(D3DRS_ZWRITEENABLE,&zwrite))||!zwrite||FAILED(d->GetRenderState(D3DRS_ALPHABLENDENABLE,&blend))||blend)return;
 IDirect3DVertexShader9*vs=nullptr;if(FAILED(d->GetVertexShader(&vs)))return;int kind=ShaderKind(vs);if(vs)vs->Release();if(kind<0){++skippedShader;return;}
 // BoneArrayWorld casters cannot be retained across frames without stale silhouettes.
 // Their original game draw still runs; exclude them only from the live cached maps.
 if(continuous&&kind==0){++skippedAnimated;return;}

 UINT reg=kind==0?144:0;Matrix4 original; if(FAILED(d->GetVertexShaderConstantF(reg,original.v,4))){++failed;return;}
 size_t instance=0;if(kind==1){auto it=instanceIndex.find(std::string(reinterpret_cast<const char*>(&original),64));if(it==instanceIndex.end()){++skippedInstance;return;}instance=it->second;}
 const D3DRENDERSTATETYPE states[]={D3DRS_SCISSORTESTENABLE,D3DRS_STENCILENABLE,D3DRS_COLORWRITEENABLE,D3DRS_CLIPPLANEENABLE};DWORD saved[4]={};for(unsigned i=0;i<4;++i)if(FAILED(d->GetRenderState(states[i],&saved[i]))){++failed;return;}
 busy=true;bool ok=true;for(auto s:states)ok=SUCCEEDED(d->SetRenderState(s,0))&&ok;
 WosShadowTargets::Binding binding;
 for(unsigned i=0;i<4&&ok;++i){if(!(updateMask&(1u<<i)))continue;Matrix4 m;if(kind==0){for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)m.v[r*4+c]=plan.cascades[i].vp.v[c*4+r];}else m=passes[i].instances[instance].shaderWVP;
  if(!(binding.active?binding.Select(targets,i):binding.Begin(d,targets,i))){ok=false;break;}
  ok=SUCCEEDED(d->SetVertexShaderConstantF(reg,m.v,4))&&SUCCEEDED(draw());++gpuDraws;
 }
 ok=SUCCEEDED(binding.End())&&ok;
 ok=SUCCEEDED(d->SetVertexShaderConstantF(reg,original.v,4))&&ok;for(unsigned i=0;i<4;++i)ok=SUCCEEDED(d->SetRenderState(states[i],saved[i]))&&ok;
 if(ok)++replayed;else ++failed;busy=false;
}
static bool Save(){
 const unsigned resolution=targets.resolution;
 std::string root=WosProbe::Root()+"\\cascade_captures";CreateDirectoryA(root.c_str(),nullptr);char stamp[80];SYSTEMTIME t;GetLocalTime(&t);std::snprintf(stamp,sizeof(stamp),"\\%04u%02u%02u_%02u%02u%02u_%llu",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,GetTickCount64());folder=root+stamp;if(!CreateDirectoryA(folder.c_str(),nullptr))return false;
 FILE*meta=std::fopen((folder+"\\capture.json").c_str(),"wb");if(!meta)return false;
 std::fprintf(meta,"{\"partial\":true,\"source\":\"existing far shadow draw calls; two exact vertex shaders; incomplete caster coverage; diagnostic only\",\"source_nodes\":%u,\"excluded_polymorphic_nodes\":%u,\"replayed_source_draws\":%u,\"skipped_shader_draws\":%u,\"skipped_instance_draws\":%u,\"failed_draws\":%u,\"maps\":[",sourceNodes,excludedNodes,replayed,skippedShader,skippedInstance,failed);
 bool ok=true;
 for(unsigned i=0;i<4;++i){std::vector<float> depths;bool read=WosDepthReadback::Read(device,targets.textures[i],resolution,depths,g_original_set_pixel_shader,g_original_draw_primitive_up,g_original_begin_scene,g_original_end_scene);unsigned covered=0,bad=0;
  char name[40];std::snprintf(name,sizeof(name),"\\cascade_%u",i);std::string path=folder+name;
  if(read){FILE*raw=std::fopen((path+".r32f").c_str(),"wb");if(raw){ok=(std::fwrite(depths.data(),4,depths.size(),raw)==depths.size())&&ok;ok=std::fclose(raw)==0&&ok;}else ok=false;
   FILE*bmp=std::fopen((path+".bmp").c_str(),"wb");if(bmp){BITMAPFILEHEADER fh={};BITMAPINFOHEADER ih={};fh.bfType=0x4d42;fh.bfOffBits=sizeof(fh)+sizeof(ih);fh.bfSize=fh.bfOffBits+resolution*resolution*3;ih.biSize=sizeof(ih);ih.biWidth=resolution;ih.biHeight=-resolution;ih.biPlanes=1;ih.biBitCount=24;ok=std::fwrite(&fh,1,sizeof(fh),bmp)==sizeof(fh)&&ok;ok=std::fwrite(&ih,1,sizeof(ih),bmp)==sizeof(ih)&&ok;
    std::vector<unsigned char>row(resolution*3);for(unsigned y=0;y<resolution;++y){for(unsigned x=0;x<resolution;++x){float v=depths[size_t(y)*resolution+x];bool finite=std::isfinite(v)&&v>=0&&v<=1;if(!finite)++bad;if(finite&&v<.999999f)++covered;unsigned char c=finite?static_cast<unsigned char>((1-v)*255):0;row[x*3]=row[x*3+1]=row[x*3+2]=c;}ok=std::fwrite(row.data(),1,row.size(),bmp)==row.size()&&ok;}ok=std::fclose(bmp)==0&&ok;
   }else ok=false;
  }else ok=false;
  auto&c=plan.cascades[i];std::fprintf(meta,"%s{\"index\":%u,\"width\":%u,\"height\":%u,\"readback\":%s,\"covered_pixels\":%u,\"invalid_depth\":%u,\"near\":%.9g,\"far\":%.9g}",i?",":"",i,resolution,resolution,read?"true":"false",covered,bad,c.nearDepth,c.farDepth);
 }
 std::fprintf(meta,"],\"write_ok\":%s}\n",ok?"true":"false");ok=std::fclose(meta)==0&&ok;return ok;
}
static void FrameEnd(){
 lastGpuDraws=gpuDraws;
 if(active&&ready){active=false;busy=true;bool ok=Save();busy=false;status=ok?"Four partial shadow maps saved":"Cascade capture failed";}
 else if(active){active=false;status="Cascade capture: no supported game scene";}
 if(continuous){if(!ready||failed){Reset();status="Cascades disabled: frame preparation/replay failed";}else {historyPlan=plan;historyPosition=cameraPosition;memcpy(historyDepthPlane,cameraDepthPlane,16);historyValid=true;updatePair=(updatePair+1)&3;ready=false;status="F11: four 1024 cascades ON (one update/frame, native shadows retained)";}}
 else Release();
 if(GetForegroundWindow()==g_window&&(GetAsyncKeyState(VK_F10)&1)){active=true;status="Cascade capture armed for next frame";}
 if(GetForegroundWindow()==g_window&&(GetAsyncKeyState(VK_F11)&1)){continuous=!continuous;if(!continuous)Release();status=continuous?"F11: four cascades armed":"F11: four cascades OFF";}
}
}
