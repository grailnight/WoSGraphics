#pragma once
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/shadow_packets.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/shadow_cascades.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/shadow_targets.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/shadow_readback.h"
#include <unordered_set>
#include "../../../shadow_fast_globals.h"
#include "../../../shadow_work_cache.h"
#include "../../../hook_profile.h"
namespace WosCascadeProbe {
using WosShadowTransform::Matrix4;
static bool active=false,ready=false,busy=false,continuous=false;
// Deferred consumer seam. With a sink installed the four cascades render
// into targets the consumer owns, every cascade refreshes each frame and
// animated casters are kept, because a per-pixel consumer cannot hide a
// stale silhouette behind the native shadow the way the F11 hybrid did.
// Without a sink every path below behaves exactly as before.
struct Sink { bool(*prepare)(IDirect3DDevice9*,unsigned);bool(*begin)(IDirect3DDevice9*);bool(*select)(unsigned);bool(*end)(); };
static const Sink*sink=nullptr;
// Optional fifth target, the top-down sky height map. It rides on the sink
// session: prepare decides whether it renders this frame (and clears it),
// select binds it after the four cascade tiles. Absent, nothing changes.
struct HeightSink { bool(*prepare)(IDirect3DDevice9*,const double*,WosShadowTransform::Matrix4&);bool(*select)(IDirect3DDevice9*); };
static const HeightSink*heightSink=nullptr;
static WosShadowTransform::Matrix4 heightVP={};
static unsigned sinkResolution=1024;
static float sinkDistance=150.f;
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
// Sun orientation for the cascades. The native shadow scenes always look
// straight down (shadow_scenes_setup 0x96F950 hard-codes up = (0,1,0), eye 400
// above the player), so cascades fitted to that orientation projected every
// shadow vertically whatever the sun did. When the day cycle turns the sun,
// the host sets sunToward, and the light basis handed to WosCascades::Build is
// turned so its depth axis follows the sun; same z sense and handedness as the
// native basis. The native matrix itself is untouched (packets still decode
// with it), and casters are still exactly the native queue.
static bool(*sunToward)(double out[3])=nullptr;
static bool orientedBySun=false;
static unsigned long orientCalls=0,orientNoFn=0,orientNoSun=0,orientBadBasis=0,orientOk=0;
static double orientNativeZ[3]={},orientZ[3]={};
static Matrix4 OrientToSun(const Matrix4&light){
 orientedBySun=false;++orientCalls;
 double t[3];if(!sunToward){++orientNoFn;return light;}if(!sunToward(t)){++orientNoSun;return light;}
 using WosCascades::V3;
 V3 x={light.v[0],light.v[4],light.v[8]},y={light.v[1],light.v[5],light.v[9]},z={light.v[2],light.v[6],light.v[10]};
 V3 s={t[0],t[1],t[2]};
 if(!WosCascades::Unit(x)||!WosCascades::Unit(y)||!WosCascades::Unit(z)||!WosCascades::Unit(s)){++orientBadBasis;return light;}
 orientNativeZ[0]=z.x;orientNativeZ[1]=z.y;orientNativeZ[2]=z.z;
 const double handed=WosCascades::Dot({x.y*y.z-x.z*y.y,x.z*y.x-x.x*y.z,x.x*y.y-x.y*y.x},z);
 // The native z is vertical; keep its sense (down for a camera looking down).
 V3 nz=s*(z.y>=0?1.0:-1.0);
 V3 nx=x-nz*WosCascades::Dot(x,nz);
 if(!WosCascades::Unit(nx)){nx=y-nz*WosCascades::Dot(y,nz);if(!WosCascades::Unit(nx)){++orientBadBasis;return light;}}
 V3 ny={nz.y*nx.z-nz.z*nx.y,nz.z*nx.x-nz.x*nx.z,nz.x*nx.y-nz.y*nx.x};
 if(handed<0)ny=ny*-1.0;
 Matrix4 out=light;
 out.v[0]=float(nx.x);out.v[4]=float(nx.y);out.v[8]=float(nx.z);
 out.v[1]=float(ny.x);out.v[5]=float(ny.y);out.v[9]=float(ny.z);
 out.v[2]=float(nz.x);out.v[6]=float(nz.y);out.v[10]=float(nz.z);
 orientedBySun=true;++orientOk;orientZ[0]=nz.x;orientZ[1]=nz.y;orientZ[2]=nz.z;
 return out;
}
static bool CameraPosition(const Matrix4&vp,WosCascades::V3&out){
 double inv[16];WosCascades::V3 a,b;
 if(!WosCascades::Inverse(vp,inv)||!WosCascades::Unproject(inv,0,0,0,a)||!WosCascades::Unproject(inv,0,0,1,b))return false;
 auto w=[&](WosCascades::V3 p){return p.x*vp.v[3]+p.y*vp.v[7]+p.z*vp.v[11]+vp.v[15];};
 double wa=w(a),wb=w(b);if(!std::isfinite(wb-wa)||std::abs(wb-wa)<1e-8)return false;
 out=a-(b-a)*(wa/(wb-wa));return std::isfinite(out.x)&&std::isfinite(out.y)&&std::isfinite(out.z);
}
static unsigned gpuDraws=0,lastGpuDraws=0,prepareCalls=0,prepareEntries=0;
static double prepareCpuMs=0,replayCpuMs=0;
struct CpuTimer {
 double&total;LARGE_INTEGER start={};
 explicit CpuTimer(double&value):total(value){QueryPerformanceCounter(&start);}
 ~CpuTimer(){LARGE_INTEGER end,freq;QueryPerformanceCounter(&end);QueryPerformanceFrequency(&freq);if(freq.QuadPart>0)total+=1000.0*double(end.QuadPart-start.QuadPart)/freq.QuadPart;}
};

static WosShadowPackets::Pass source,passes[5],nearSource,nearPasses[5];
static std::vector<Matrix4> projectedMatrices[5],nearProjectedMatrices[5];
static const Matrix4&TargetVP(unsigned i){return i<4?plan.cascades[i].vp:heightVP;}
static bool compactProjection=false;
static bool ProjectPass(const WosShadowPackets::Pass&src,const Matrix4&vp,WosShadowPackets::Pass&legacy,std::vector<Matrix4>&compact){return compactProjection?WosShadowWork::Project(src,vp,compact):WosShadowPackets::Reproject(src,vp,legacy);}
static WosShadowWork::MatrixIndex nearInstanceIndex;
static uint32_t nearScene=0;
static bool nearReady=false;
static WosShadowWork::MatrixIndex instanceIndex;
// Recognised caster paths, in kind order:
//   0 vs_0002 BoneArrayWorld, viewProj at c144, transposed
//   1 vs_0003 worldViewProj at c0, per instance, reprojected from the packet
//   2 vs_0005 SpeedTree branches, g_mModelViewProj at c44
//   3 vs_0007 SpeedTree leaf cards, g_mModelViewProj at c36
// The SpeedTree pair are Z-up while the engine is Y-up, so their matrix is
// the same light matrix with the y and z rows exchanged.
// The SpeedTree pair place their geometry from g_vTreePos and wind constants,
// so their matrix is the light's view-projection and needs no per-instance work.
static std::vector<unsigned char> vsCodes[4];
static const UINT kindRegister[4]={144,0,44,36};
static std::unordered_map<IDirect3DVertexShader9*,int> shaderKinds;
static std::string folder;
static uint32_t farScene=0,mainScene=0;
static float cameraDepthPlane[4]={};
// Deferred stagger. The resolve picks the tightest cascade that contains a
// receiver, so a held cascade that no longer covers part of the view hands
// those pixels to the next, coarser one instead of shading them wrongly.
//   0 every cascade every frame
//   1 cascades 0 and 1 every frame, 2 and 3 alternate
//   2 cascade 0 every frame, 1 every other frame, 2 and 3 every fourth
// A held cascade is refreshed early once the camera has turned or moved too
// far from where it was rendered, and every one is refreshed on a new scene.
// Animated casters in a held cascade lag by at most the hold.
static int sinkStagger=1;
static bool saCasters=true;  // [Cascades] SaCasters, see DrawSaCasters
static unsigned staggerFrame=0,staggerForced=0,staggerUpdated=4;
static WosCascades::V3 cascadeEye[4]={};
static float cascadeDir[4][3]={};
static uint32_t historyScene=0;
static unsigned StaggerSchedule(unsigned f){
 if(sinkStagger>=2)return 1u|((f&1)==0?2u:((f&2)?8u:4u));
 return 3u|((f&1)?8u:4u);
}
static void SelectUpdates(){
 updateMask=15;refreshReason=1;
 if(sink){
  staggerUpdated=4;
  if(sinkStagger<=0||!historyValid||mainScene!=historyScene)return;
  refreshReason=0;
  unsigned mask=StaggerSchedule(staggerFrame++);
  for(unsigned i=0;i<4;++i){
   if(mask&(1u<<i))continue;
   double alignment=0;for(unsigned k=0;k<3;++k)alignment+=double(cameraDepthPlane[k])*cascadeDir[i][k];
   auto movement=cameraPosition-cascadeEye[i];
   double fresh=plan.cascades[i].farDepth,held=historyPlan.cascades[i].farDepth;
   if(alignment<.995||WosCascades::Dot(movement,movement)>16||std::abs(fresh-held)>1e-3*(std::max)(1.0,std::abs(fresh))){mask|=1u<<i;++staggerForced;}
  }
  updateMask=mask;staggerUpdated=0;
  for(unsigned i=0;i<4;++i){if(updateMask&(1u<<i))++staggerUpdated;else plan.cascades[i]=historyPlan.cascades[i];}
  return;
 }
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
namespace SaCasters{static void Release();static bool drawn=false;}
static void Release(){SaCasters::Release();for(auto&v:projectedMatrices)v.clear();for(auto&v:nearProjectedMatrices)v.clear();nearReady=false;nearScene=0;nearSource={};for(auto&p:nearPasses)p={};nearInstanceIndex.Clear();historyValid=false;updateMask=15;updatePair=0;verified=false;targets.Reset();source={};for(auto&p:passes)p={};instanceIndex.Clear();for(auto&e:shaderKinds)e.first->Release();shaderKinds.clear();for(auto&v:vsCodes)v.clear();if(device)device->Release();device=nullptr;ready=false;}
static void Reset(){active=continuous=false;Release();}
static std::vector<unsigned char> ReadFile(const std::string &path){FILE*f=std::fopen(path.c_str(),"rb");if(!f)return{};std::fseek(f,0,SEEK_END);long n=std::ftell(f);std::rewind(f);std::vector<unsigned char>b;if(n>0&&n<1048576){b.resize(n);if(std::fread(b.data(),1,n,f)!=size_t(n))b.clear();}std::fclose(f);return b;}
static int ShaderKind(IDirect3DVertexShader9*vs){
 auto it=shaderKinds.find(vs);if(it!=shaderKinds.end())return it->second;
 int kind=-1;UINT n=0;if(vs&&SUCCEEDED(vs->GetFunction(nullptr,&n))&&n<1048576){std::vector<unsigned char>b(n);if(n&&SUCCEEDED(vs->GetFunction(b.data(),&n)))for(int i=0;i<4;++i)if(!vsCodes[i].empty()&&b==vsCodes[i])kind=i;}
 if(vs){vs->AddRef();shaderKinds.emplace(vs,kind);}return kind;
}
static void Prepare(IDirect3DDevice9*d,uintptr_t caller){
 if(caller==0x009dcf3a)++prepareEntries;
 if((!active&&!continuous&&!sink)||ready||busy||caller!=0x009dcf3a)return;
 ++prepareCalls;
 prepareCpuMs=replayCpuMs=0;CpuTimer preparationTimer(prepareCpuMs);
 compactProjection=WosShadowWork::enabled;
 gpuDraws=skippedAnimated=0;replayed=skippedShader=skippedInstance=failed=sourceNodes=excludedNodes=0;SaCasters::drawn=false;
 source={};for(auto&p:passes)p={};instanceIndex.Clear();
 nearReady=false;nearScene=0;nearSource={};for(auto&p:nearPasses)p={};nearInstanceIndex.Clear();
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
 WosCascades::Config cfg;cfg.resolution=sink?sinkResolution:(continuous?1024:2048);
 if(sink&&std::isfinite(sinkDistance)&&sinkDistance>=20&&sinkDistance<=400)cfg.distance=sinkDistance;
 if(!best||!WosCascades::Build(camera,OrientToSun(light),cfg,plan)){active=false;status="Cascade probe: camera fitting failed";Release();return;}
 double planeLength=std::sqrt(double(camera.v[3])*camera.v[3]+double(camera.v[7])*camera.v[7]+double(camera.v[11])*camera.v[11]);
 if(!std::isfinite(planeLength)||planeLength<1e-8){Reset();status="Cascade camera depth invalid";return;}
 for(unsigned i=0;i<4;++i)cameraDepthPlane[i]=static_cast<float>(camera.v[i*4+3]/planeLength);
 if(!CameraPosition(camera,cameraPosition)){Reset();status="Cascade camera position invalid";return;}
 SelectUpdates();
 for(unsigned i=0;i<4;++i)if((updateMask&(1u<<i))&&!ProjectPass(source,plan.cascades[i].vp,passes[i],projectedMatrices[i])){active=false;status="Cascade probe: reproject failed";Release();return;}
 // Capture both current-frame queues into the same freshly cleared atlas.
 // Each queue has a different native WVP, so its instance lookup must be separate.
 if(sink&&g.nearScene&&g.nearScene!=g.farScene){
  uint32_t nearHead=0;WosShadowPackets::Error nearError;
  if(read(g.nearScene+0x414,&nearHead,4)&&WosShadowPackets::Build(read,nearHead,light,nearSource,nearError)){
   nearReady=true;
   for(unsigned i=0;i<4;++i)if((updateMask&(1u<<i))&&!ProjectPass(nearSource,plan.cascades[i].vp,nearPasses[i],nearProjectedMatrices[i]))nearReady=false;
   if(nearReady){nearScene=g.nearScene;for(size_t i=0;i<nearSource.instances.size();++i)nearInstanceIndex.Emplace(&nearSource.instances[i].shaderWVP,i);}
  }
 }
 static const char*const kindFiles[4]={"\\cascade_probe\\vs_0002.cso","\\cascade_probe\\vs_0003.cso","\\cascade_probe\\vs_0005.cso","\\cascade_probe\\vs_0007.cso"};
 for(unsigned i=0;i<4;++i)if(vsCodes[i].empty())vsCodes[i]=ReadFile(WosProbe::Root()+kindFiles[i]);
 bool targetsReady=!vsCodes[0].empty()&&!vsCodes[1].empty();
 if(targetsReady)targetsReady=sink?sink->prepare(d,cfg.resolution):((device==d&&targets.resolution==cfg.resolution)||targets.Create(d,cfg.resolution));
 if(!targetsReady){active=false;status="Cascade probe: shader references or GPU targets unavailable";Release();return;}
 if(sink&&heightSink){
  const double eye[3]={cameraPosition.x,cameraPosition.y,cameraPosition.z};
  if(heightSink->prepare(d,eye,heightVP)){
   bool projected=ProjectPass(source,heightVP,passes[4],projectedMatrices[4]);
   if(nearReady)projected=ProjectPass(nearSource,heightVP,nearPasses[4],nearProjectedMatrices[4])&&projected;
   if(projected)updateMask|=16u;
  }
 }
 if(!sink)for(unsigned i=0;i<4;++i){if(!(updateMask&(1u<<i)))continue;WosShadowTargets::Binding binding;if(!binding.Begin(d,targets,i)||FAILED(d->Clear(0,nullptr,D3DCLEAR_ZBUFFER,0,1,0))||FAILED(binding.End())){active=false;status="Cascade probe: target initialization failed";Release();return;}}
 for(size_t i=0;i<source.instances.size();++i)instanceIndex.Emplace(&source.instances[i].shaderWVP,i);
 farScene=g.farScene;sourceNodes=static_cast<unsigned>(source.nodes.size());if(device!=d){if(device)device->Release();device=d;d->AddRef();}ready=true;status="Capturing experimental cascades...";
}
// SA district casters. The GTA SA district (WoS_Arena collision loader) is
// drawn on the ocean carrier, which never enters the game's shadow scenes, so
// nothing above can replay it. Its DLL records each opaque slot's main-pass
// draw and exports the list; it is drawn here once per frame into every
// updated cascade and the height map, with the rigid caster shader (kind 1,
// vs_0003), which decodes the same SHORT4N position and reads the normal.
namespace SaCasters {
struct Caster {  // layout owned by SA_CollisionLoader (sa_runtime.h), checked by size
 IDirect3DVertexBuffer9*vb;UINT offset,stride;
 IDirect3DIndexBuffer9*ib;IDirect3DVertexDeclaration9*decl;
 INT base;UINT min,vertices,start,primitives;
 float scale[4],bias[4];
 float world[16];   // row-vector: p_world = p * world
 float lo[3],hi[3]; // object-space box of the slot, all zero when unknown
 UINT type;         // D3DPRIMITIVETYPE: strips for carrier slots, lists for tiles
};
using ListFn=unsigned(__cdecl*)(const void**);
using SizeFn=unsigned(__cdecl*)();
using EnumFn=BOOL(WINAPI*)(HANDLE,HMODULE*,DWORD,LPDWORD);
static ListFn list=nullptr;
static bool mismatch=false,shadersFailed=false;  // drawn: declared with Release, above
static ULONGLONG nextScan=0;
static IDirect3DDevice9*owner=nullptr;
static IDirect3DVertexShader9*vs=nullptr;
static IDirect3DPixelShader9*ps=nullptr;
static std::vector<WosShadowWork::CasterBox> boxes;
// ps_3_0: def c0, 0,0,0,0 / mov oC0, c0. Colour writes are off in the atlas;
// the point is that nothing the native caster left bound can discard.
static const DWORD depthOnlyPS[]={0xffff0300,0x05000051,0xa00f0000,0,0,0,0,0x02000001,0x800f0800,0xa0e40000,0x0000ffff};
// The loader DLL is injected under a stamped name, so look for the export.
static void Find(){
 if(list||mismatch)return;
 const ULONGLONG now=GetTickCount64();if(now<nextScan)return;nextScan=now+2000;
 static EnumFn enumModules=reinterpret_cast<EnumFn>(GetProcAddress(GetModuleHandleA("kernel32.dll"),"K32EnumProcessModules"));
 if(!enumModules)return;
 HMODULE modules[512];DWORD bytes=0;
 if(!enumModules(GetCurrentProcess(),modules,sizeof(modules),&bytes))return;
 const DWORD n=(std::min)(DWORD(512),bytes/DWORD(sizeof(HMODULE)));
 for(DWORD i=0;i<n;++i){
  auto l=reinterpret_cast<ListFn>(GetProcAddress(modules[i],"SA_ShadowCasters"));
  auto z=reinterpret_cast<SizeFn>(GetProcAddress(modules[i],"SA_ShadowCasterSize"));
  if(!l||!z)continue;
  if(z()==sizeof(Caster))list=l;else {mismatch=true;status="SA district shadow casters: loader layout mismatch";}
  return;
 }
}
static void Release(){if(vs)vs->Release();if(ps)ps->Release();vs=nullptr;ps=nullptr;owner=nullptr;shadersFailed=false;}
static bool Shaders(IDirect3DDevice9*d){
 if(owner==d&&vs&&ps)return true;
 if(shadersFailed&&owner==d)return false;
 Release();owner=d;
 const auto&code=vsCodes[1];
 if(code.empty()||FAILED(d->CreateVertexShader(reinterpret_cast<const DWORD*>(code.data()),&vs))||FAILED(d->CreatePixelShader(depthOnlyPS,&ps))){
  if(vs)vs->Release();if(ps)ps->Release();vs=nullptr;ps=nullptr;shadersFailed=true;status="SA district shadow casters: shader creation failed";return false;}
 return true;
}
// Positions are stored with w = 0, so the decoded point is (xyz, bias.w).
static void Box(const Caster&c,WosShadowWork::CasterBox&box){
 box.valid=false;
 if(!WosShadowWork::cullEnabled||(c.lo[0]==0&&c.lo[1]==0&&c.lo[2]==0&&c.hi[0]==0&&c.hi[1]==0&&c.hi[2]==0))return;
 const float margin=.5f;  // SHORT4N rounding against the builder's box
 for(unsigned k=0;k<8;++k){
  for(unsigned a=0;a<3;++a)box.corner[k][a]=((k>>a)&1u)?c.hi[a]+margin:c.lo[a]-margin;
  box.corner[k][3]=c.bias[3];
 }
 box.valid=std::isfinite(c.bias[3]);
}
// Shader rows for kind 1: transpose(world * VP), both row-vector.
static void Rows(const float*world,const Matrix4&vp,Matrix4&m){
 for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c){
  double v=0;for(unsigned k=0;k<4;++k)v+=double(world[r*4+k])*vp.v[k*4+c];
  m.v[c*4+r]=static_cast<float>(v);
 }
}
}
static void DrawSaCasters(IDirect3DDevice9*d){
 using namespace SaCasters;
 Find();if(!list)return;
 const void*raw=nullptr;const unsigned count=list(&raw);
 WosShadowWork::cpuCurrent.saAvailable=count;
 if(!count||!raw||!Shaders(d))return;
 const Caster*c=static_cast<const Caster*>(raw);
 boxes.resize(count);
 bool sameWorld=true;WosShadowWork::CasterBox all;all.valid=WosShadowWork::cullEnabled;
 float lo[3]={1e30f,1e30f,1e30f},hi[3]={-1e30f,-1e30f,-1e30f};
 for(unsigned k=0;k<count;++k){
  Box(c[k],boxes[k]);if(!boxes[k].valid){all.valid=false;continue;}
  if(memcmp(c[k].world,c[0].world,sizeof(c[0].world))||memcmp(c[k].bias+3,c[0].bias+3,4))sameWorld=false;
  for(unsigned a=0;a<3;++a){lo[a]=(std::min)(lo[a],boxes[k].corner[0][a]);hi[a]=(std::max)(hi[a],boxes[k].corner[7][a]);}
 }
 // One box around the whole district rejects a target in eight corner tests.
 if(all.valid&&sameWorld)for(unsigned k=0;k<8;++k){for(unsigned a=0;a<3;++a)all.corner[k][a]=((k>>a)&1u)?hi[a]:lo[a];all.corner[k][3]=c[0].bias[3];}
 else all.valid=false;

 IDirect3DVertexShader9*oldVS=nullptr;IDirect3DPixelShader9*oldPS=nullptr;IDirect3DVertexDeclaration9*oldDecl=nullptr;
 IDirect3DVertexBuffer9*oldVB=nullptr;UINT oldOffset=0,oldStride=0,oldFreq=1;IDirect3DIndexBuffer9*oldIB=nullptr;
 float oldConstants[9*4]={};
 const D3DRENDERSTATETYPE states[]={D3DRS_SCISSORTESTENABLE,D3DRS_STENCILENABLE,D3DRS_COLORWRITEENABLE,D3DRS_CLIPPLANEENABLE,D3DRS_ALPHATESTENABLE};DWORD saved[5]={};
 DWORD cull=D3DCULL_CCW;
 bool ok=SUCCEEDED(d->GetVertexShader(&oldVS))&&SUCCEEDED(d->GetPixelShader(&oldPS))&&SUCCEEDED(d->GetVertexDeclaration(&oldDecl))&&
  SUCCEEDED(d->GetStreamSource(0,&oldVB,&oldOffset,&oldStride))&&SUCCEEDED(d->GetStreamSourceFreq(0,&oldFreq))&&SUCCEEDED(d->GetIndices(&oldIB))&&
  SUCCEEDED(d->GetVertexShaderConstantF(0,oldConstants,9))&&SUCCEEDED(d->GetRenderState(D3DRS_CULLMODE,&cull));
 for(unsigned i=0;i<5&&ok;++i)ok=SUCCEEDED(d->GetRenderState(states[i],&saved[i]));
 if(!ok){if(oldVS)oldVS->Release();if(oldPS)oldPS->Release();if(oldDecl)oldDecl->Release();if(oldVB)oldVB->Release();if(oldIB)oldIB->Release();return;}

 busy=true;
 for(auto st:states)ok=SUCCEEDED(d->SetRenderState(st,0))&&ok;
 ok=SUCCEEDED(g_original_set_vertex_shader(d,vs))&&SUCCEEDED(g_original_set_pixel_shader(d,ps))&&ok;
 if(oldFreq!=1)ok=SUCCEEDED(d->SetStreamSourceFreq(0,1))&&ok;
 bool sinkOpen=false;if(ok){sinkOpen=sink->begin(d);ok=sinkOpen;}
 const bool height=(updateMask&16u)&&heightSink;
 unsigned draws=0,culled=0;bool drawFailed=false;
 for(unsigned i=0;i<5&&ok&&!drawFailed;++i){
  if(!(updateMask&(1u<<i)))continue;
  if(i==4&&!height)break;
  Matrix4 m;Rows(c[0].world,TargetVP(i),m);
  if(WosShadowWork::OutsideClip(all,m.v)){culled+=count;continue;}
  bool selected=false;const float*world=nullptr;const float*scale=nullptr;const float*bias=nullptr;
  IDirect3DVertexBuffer9*vb=nullptr;UINT offset=~0u,stride=0;IDirect3DIndexBuffer9*ib=nullptr;IDirect3DVertexDeclaration9*decl=nullptr;
  for(unsigned k=0;k<count;++k){
   const Caster&cc=c[k];
   if(!world||memcmp(world,cc.world,sizeof(cc.world))){world=cc.world;Rows(world,TargetVP(i),m);if(selected&&FAILED(d->SetVertexShaderConstantF(0,m.v,4))){drawFailed=true;break;}}
   if(WosShadowWork::OutsideClip(boxes[k],m.v)){++culled;continue;}
   if(!selected){
    if(i==4){if(!heightSink->select(d)||FAILED(d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE))){drawFailed=true;break;}}
    else if(!sink->select(i)){ok=false;break;}
    if(FAILED(d->SetVertexShaderConstantF(0,m.v,4))){drawFailed=true;break;}
    selected=true;
   }
   if(!scale||memcmp(scale,cc.scale,16)){scale=cc.scale;if(FAILED(d->SetVertexShaderConstantF(7,scale,1))){drawFailed=true;break;}}
   if(!bias||memcmp(bias,cc.bias,16)){bias=cc.bias;if(FAILED(d->SetVertexShaderConstantF(8,bias,1))){drawFailed=true;break;}}
   if(cc.vb!=vb||cc.offset!=offset||cc.stride!=stride){vb=cc.vb;offset=cc.offset;stride=cc.stride;if(FAILED(d->SetStreamSource(0,vb,offset,stride))){drawFailed=true;break;}}
   if(cc.ib!=ib){ib=cc.ib;if(FAILED(d->SetIndices(ib))){drawFailed=true;break;}}
   if(cc.decl!=decl){decl=cc.decl;if(FAILED(d->SetVertexDeclaration(decl))){drawFailed=true;break;}}
   if(FAILED(g_original_draw_indexed_primitive(d,static_cast<D3DPRIMITIVETYPE>(cc.type),cc.base,cc.min,cc.vertices,cc.start,cc.primitives))){drawFailed=true;break;}
   ++draws;++gpuDraws;
  }
 }
 if(height)ok=SUCCEEDED(d->SetRenderState(D3DRS_CULLMODE,cull))&&ok;
 if(sinkOpen)ok=sink->end()&&ok;
 ok=SUCCEEDED(d->SetVertexDeclaration(oldDecl))&&ok;
 ok=SUCCEEDED(d->SetStreamSource(0,oldVB,oldOffset,oldStride))&&ok;
 if(oldFreq!=1)ok=SUCCEEDED(d->SetStreamSourceFreq(0,oldFreq))&&ok;
 ok=SUCCEEDED(d->SetIndices(oldIB))&&ok;
 ok=SUCCEEDED(d->SetVertexShaderConstantF(0,oldConstants,9))&&ok;
 ok=SUCCEEDED(g_original_set_vertex_shader(d,oldVS))&&SUCCEEDED(g_original_set_pixel_shader(d,oldPS))&&ok;
 for(unsigned i=0;i<5;++i)ok=SUCCEEDED(d->SetRenderState(states[i],saved[i]))&&ok;
 if(oldVS)oldVS->Release();if(oldPS)oldPS->Release();if(oldDecl)oldDecl->Release();if(oldVB)oldVB->Release();if(oldIB)oldIB->Release();
 busy=false;
 WosShadowWork::cpuCurrent.saDraws+=draws;WosShadowWork::cpuCurrent.saCulled+=culled;
 // A broken session or restore must not let the frame's maps be trusted (the
 // same rule as a native replay); a failed SA draw alone only drops SA.
 if(!ok)++failed;
 if(drawFailed)status="SA district shadow casters: a draw failed";
}
// What this replay learned about the current caster draw: the local-shadow
// replay runs next in the same hook and needs exactly the same reads and
// lookups, so it takes them from here. Cleared at the start of every replay,
// so it never outlives the draw it describes.
struct CasterInfo {bool valid=false,fromNear=false;int kind=-1;UINT reg=0;Matrix4 original={};size_t instance=0;WosShadowWork::CasterBox box;};
static CasterInfo casterInfo;
template<class Draw>static void Replay(IDirect3DDevice9*d,Draw draw){
 casterInfo.valid=false;
 if((!active&&!continuous&&!sink)||!ready||busy||WosProbe::suppress)return;
 auto g=WosShadowFastGlobals::Read(WosEngineProbe::imageBase,verified&&ready&&WosEngineProbe::imageBase==0x400000,[](){return WosEngineProbe::ReadGlobals();});
 const bool fromNear=sink&&nearReady&&g.current==nearScene;
 if(g.valid!=7||(!fromNear&&g.current!=farScene)||g.shadowActive!=1)return;
 CpuTimer replayTimer(replayCpuMs);
 DWORD zwrite=0,blend=0;if(FAILED(d->GetRenderState(D3DRS_ZWRITEENABLE,&zwrite))||!zwrite||FAILED(d->GetRenderState(D3DRS_ALPHABLENDENABLE,&blend))||blend)return;
 if(sink&&saCasters&&!SaCasters::drawn){SaCasters::drawn=true;DrawSaCasters(d);}
 IDirect3DVertexShader9*vs=nullptr;if(FAILED(d->GetVertexShader(&vs)))return;int kind=ShaderKind(vs);if(vs)vs->Release();if(kind<0){++skippedShader;return;}
 // BoneArrayWorld casters cannot be retained across frames without stale silhouettes.
 // Their original game draw still runs; exclude them only from the live cached maps.
 if(continuous&&!sink&&kind==0){++skippedAnimated;return;}

 UINT reg=kindRegister[kind];Matrix4 original; if(FAILED(d->GetVertexShaderConstantF(reg,original.v,4))){++failed;return;}
 auto& indices=fromNear?nearInstanceIndex:instanceIndex;
 auto* projected=fromNear?nearPasses:passes;
 size_t instance=0;if(kind==1){const size_t*found=indices.Find(&original);if(!found){++skippedInstance;return;}instance=*found;}
 WosHookProfile::Sub(WosHookProfile::CascadePre);
 WosShadowWork::CasterBox box;if(kind==1)WosShadowWork::BuildCasterBox(d,box);
 casterInfo.valid=true;casterInfo.fromNear=fromNear;casterInfo.kind=kind;casterInfo.reg=reg;casterInfo.original=original;casterInfo.instance=instance;casterInfo.box=box;
 const bool height=(updateMask&16u)&&sink&&heightSink;
 // Target matrices and the targets the caster's box reaches, decided before
 // any device state changes: a caster no cascade can see costs no switch.
 Matrix4 targetMatrix[5];unsigned visible=0;
 for(unsigned i=0;i<5;++i){if(!(updateMask&(1u<<i)))continue;Matrix4&m=targetMatrix[i];if(kind==0){for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)m.v[r*4+c]=TargetVP(i).v[c*4+r];}else if(kind>=2){const auto&vp=TargetVP(i);for(unsigned c=0;c<4;++c){m.v[c]=vp.v[c];m.v[4+c]=vp.v[8+c];m.v[8+c]=vp.v[4+c];m.v[12+c]=vp.v[12+c];}}else m=compactProjection?(fromNear?nearProjectedMatrices[i][instance]:projectedMatrices[i][instance]):projected[i].instances[instance].shaderWVP;
  // A cascade or the height map the caster's box misses would clip it away.
  if(WosShadowWork::OutsideClip(box,m.v)){++WosShadowWork::cpuCurrent.cascadeCulled;continue;}
  // Skinned characters stay out of the sky height map: it refreshes every few
  // frames, so a running hero would leave dark columns where he stood, and a
  // body is too thin to matter for sky visibility anyway.
  if(sink&&i==4&&(kind==0||!height))break;
  visible|=1u<<i;
 }
 WosHookProfile::Sub(WosHookProfile::CascadeCull);
 if(!visible){++replayed;++WosShadowWork::cpuCurrent.cascadeCasters;return;}
 const D3DRENDERSTATETYPE states[]={D3DRS_SCISSORTESTENABLE,D3DRS_STENCILENABLE,D3DRS_COLORWRITEENABLE,D3DRS_CLIPPLANEENABLE};DWORD saved[4]={};for(unsigned i=0;i<4;++i)if(FAILED(d->GetRenderState(states[i],&saved[i]))){++failed;return;}
 busy=true;bool ok=true;for(auto s:states)ok=SUCCEEDED(d->SetRenderState(s,0))&&ok;
 WosShadowTargets::Binding binding;bool sinkOpen=false;
 if(ok&&sink){sinkOpen=sink->begin(d);ok=sinkOpen;}
 // The height map is seen from above with its own handedness, and what it
 // needs is the highest surface whichever way a face points: no culling.
 DWORD cull=D3DCULL_CCW;if(height&&(visible&16u)&&FAILED(d->GetRenderState(D3DRS_CULLMODE,&cull)))ok=false;
 WosHookProfile::Sub(WosHookProfile::CascadeBind);
 for(unsigned i=0;i<5&&ok;++i){if(!(visible&(1u<<i)))continue;const Matrix4&m=targetMatrix[i];
  if(sink&&i==4){if(!heightSink->select(d)||FAILED(d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE)))break;}
  else if(sink){if(!sink->select(i)){ok=false;break;}}
  else if(!(binding.active?binding.Select(targets,i):binding.Begin(d,targets,i))){ok=false;break;}
 ok=SUCCEEDED(d->SetVertexShaderConstantF(reg,m.v,4))&&SUCCEEDED(draw());++gpuDraws;++WosShadowWork::cpuCurrent.cascadeDraws;
 }
 WosHookProfile::Sub(WosHookProfile::CascadeDraw);
 if(height&&(visible&16u))ok=SUCCEEDED(d->SetRenderState(D3DRS_CULLMODE,cull))&&ok;
 if(sinkOpen)ok=sink->end()&&ok;
 ok=SUCCEEDED(binding.End())&&ok;
 ok=SUCCEEDED(d->SetVertexShaderConstantF(reg,original.v,4))&&ok;for(unsigned i=0;i<4;++i)ok=SUCCEEDED(d->SetRenderState(states[i],saved[i]))&&ok;
 if(ok)++replayed;else ++failed;busy=false;++WosShadowWork::cpuCurrent.cascadeCasters;
 WosHookProfile::Sub(WosHookProfile::CascadeEnd);
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
 if(sink){if(!ready||failed){bool wasReady=ready;Release();ready=false;if(wasReady)status="Cascades: replay failed, native shadows retained";}else {historyPlan=plan;historyPosition=cameraPosition;memcpy(historyDepthPlane,cameraDepthPlane,16);historyValid=true;historyScene=mainScene;
  for(unsigned i=0;i<4;++i)if(updateMask&(1u<<i)){cascadeEye[i]=cameraPosition;memcpy(cascadeDir[i],cameraDepthPlane,12);}
  ready=false;status=staggerUpdated<4?"Dynamic cascades rendered, far ones staggered":"Four dynamic cascades rendered";}}
 else if(continuous){if(!ready||failed){Reset();status="Cascades disabled: frame preparation/replay failed";}else {historyPlan=plan;historyPosition=cameraPosition;memcpy(historyDepthPlane,cameraDepthPlane,16);historyValid=true;updatePair=(updatePair+1)&3;ready=false;status="F11: four 1024 cascades ON (one update/frame, native shadows retained)";}}
 else Release();
 if(!sink&&GetForegroundWindow()==g_window&&(GetAsyncKeyState(VK_F10)&1)){active=true;status="Cascade capture armed for next frame";}
 if(!sink&&GetForegroundWindow()==g_window&&(GetAsyncKeyState(VK_F11)&1)){continuous=!continuous;if(!continuous)Release();status=continuous?"F11: four cascades armed":"F11: four cascades OFF";}
}
}
