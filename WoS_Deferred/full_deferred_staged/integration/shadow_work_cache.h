#pragma once
#include "native/WoS_VolumetricClouds/live_menu/shadow_packets.h"
#include "hook_profile.h"
#include <array>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <unordered_map>
namespace WosShadowWork {
using Matrix=WosShadowTransform::Matrix4;
static bool enabled=true;
static unsigned long long projectionInstances=0,avoidedNodeCopies=0;
// Unlike Pass::Reproject, the replay-only table owns no copied nodes/worlds.
// Commit only complete validated results, preserving the old failure contract.
inline bool Project(const WosShadowPackets::Pass&source,const Matrix&vp,std::vector<Matrix>&out){
 std::vector<Matrix> staging;staging.resize(source.instances.size());
 for(size_t i=0;i<source.instances.size();++i)if(!WosShadowTransform::Build(source.instances[i].world,vp,staging[i]))return false;
 out.swap(staging);projectionInstances+=source.instances.size();avoidedNodeCopies+=source.nodes.size();return true;
}
struct LocalMatrices {
 struct Entry {unsigned long long generation=0;Matrix world={};std::array<Matrix,12> value={};};
 std::vector<Entry> nearEntries,farEntries;
 std::array<Matrix,12> light,worldSpace,zUp;
 unsigned count=0;unsigned long long generation=1,hits=0,misses=0,fallbacks=0;
 static constexpr size_t limit=4096;
 void Invalidate(){if(++generation==0){nearEntries.clear();farEntries.clear();generation=1;}}
 void Reset(){nearEntries.clear();farEntries.clear();count=0;Invalidate();}
 void Begin(const Matrix*values,unsigned n){
  Invalidate();count=n<=12?n:0;
  for(unsigned face=0;face<count;++face){
   light[face]=values[face];Matrix a={},b={};const Matrix&vp=values[face];
   for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)a.v[r*4+c]=vp.v[c*4+r];
   for(unsigned c=0;c<4;++c){b.v[c]=vp.v[c];b.v[4+c]=vp.v[8+c];b.v[8+c]=vp.v[4+c];b.v[12+c]=vp.v[12+c];}
   worldSpace[face]=a;zUp[face]=b;
  }
 }
 const Matrix* Get(unsigned kind,bool isNear,size_t index,const Matrix&world){
  if(!count)return nullptr;
  if(kind==0){++hits;return worldSpace.data();}
  if(kind==2||kind==3){++hits;return zUp.data();}
  if(kind!=1||index>=limit){++fallbacks;return nullptr;}
  auto&entries=isNear?nearEntries:farEntries;
  try {if(entries.size()<=index)entries.resize(index+1);}catch(...){++fallbacks;return nullptr;}
  Entry&e=entries[index];
  if(e.generation==generation&&!std::memcmp(&world,&e.world,sizeof(world))){++hits;return e.value.data();}
  std::array<Matrix,12> prepared={};
  for(unsigned f=0;f<count;++f)if(!WosShadowTransform::Build(world,light[f],prepared[f])){++fallbacks;return nullptr;}
  e.value=prepared;e.world=world;e.generation=generation;++misses;return e.value.data();
 }
};
static LocalMatrices local;
// Instance lookup by the 64 raw bytes of a native shader WVP. Same contract as
// the unordered_map<std::string,size_t> it replaces: byte equality, and the
// first insertion of a key wins. Open addressing over storage that survives
// Clear(), so neither building the index nor a per-draw lookup allocates.
// Lookup by raw key bytes, Words 32-bit words long. For the shader WVP index
// the contract matches the unordered_map<std::string,size_t> it replaced:
// byte equality, and the first insertion of a key wins. Open addressing over
// storage that survives Clear(), so neither building nor a lookup allocates.
template<unsigned Words> class WordIndex {
 struct Slot {uint32_t stamp=0;uint32_t key[Words]={};size_t value=0;};
 std::vector<Slot> slots;size_t used=0;uint32_t stamp=1;
 static uint32_t Hash(const uint32_t*k){
  uint32_t h=2166136261u;for(unsigned i=0;i<Words;++i){h^=k[i];h*=16777619u;h^=h>>15;}
  h^=h>>16;h*=0x7feb352du;h^=h>>15;return h;
 }
 bool Grow(){
  std::vector<Slot> old;old.swap(slots);
  try {slots.resize(old.empty()?1024:old.size()*2);}catch(...){slots.swap(old);return false;}
  const uint32_t live=stamp;stamp=1;used=0;
  for(const Slot&s:old)if(s.stamp==live)Insert(s.key,s.value);
  return true;
 }
 void Insert(const uint32_t*k,size_t value){
  const size_t mask=slots.size()-1;
  for(size_t i=Hash(k)&mask;;i=(i+1)&mask){
   Slot&s=slots[i];
   if(s.stamp!=stamp){s.stamp=stamp;std::memcpy(s.key,k,sizeof(s.key));s.value=value;++used;return;}
   if(!std::memcmp(s.key,k,sizeof(s.key)))return;
  }
 }
 const Slot* Locate(const void*bytes)const{
  if(slots.empty())return nullptr;
  uint32_t k[Words];std::memcpy(k,bytes,sizeof(k));const size_t mask=slots.size()-1;
  for(size_t i=Hash(k)&mask;;i=(i+1)&mask){
   const Slot&s=slots[i];
   if(s.stamp!=stamp)return nullptr;
   if(!std::memcmp(s.key,k,sizeof(k)))return &s;
  }
 }
public:
 void Clear(){used=0;if(++stamp==0){for(Slot&s:slots)s.stamp=0;stamp=1;}}
 size_t Size()const{return used;}
 // Returns false only when the table could not grow; callers treat the key
 // as absent, which for every user here falls back to the uncached path.
 bool Emplace(const void*bytes,size_t value){
  if((used+1)*2>slots.size()&&!Grow())return false;
  uint32_t k[Words];std::memcpy(k,bytes,sizeof(k));Insert(k,value);return true;
 }
 const size_t* Find(const void*bytes)const{const Slot*s=Locate(bytes);return s?&s->value:nullptr;}
};
using MatrixIndex=WordIndex<16>;
// The rigid caster shader (vs_0003) decodes SHORT4N positions as v0*c7+c8, so
// every vertex lies in the box c8 +- |c7|. A caster whose box is wholly outside
// one clip plane of a target would be clipped to nothing there, so skipping
// that draw leaves the target exactly as it was. Anything else stays unbounded.
static bool cullEnabled=true;  // [Cache] CasterCull
static unsigned long long cullUnbounded=0;
struct CasterBox {float corner[8][4];bool valid=false;};
static std::unordered_map<IDirect3DVertexDeclaration9*,bool> declNormalized;
static bool PositionNormalized(IDirect3DDevice9*d){
 IDirect3DVertexDeclaration9*decl=nullptr;if(FAILED(d->GetVertexDeclaration(&decl))||!decl)return false;
 auto it=declNormalized.find(decl);if(it!=declNormalized.end()){decl->Release();return it->second;}
 D3DVERTEXELEMENT9 e[MAXD3DDECLLENGTH+1]={};UINT n=0;bool ok=false,found=false;
 if(SUCCEEDED(decl->GetDeclaration(nullptr,&n))&&n<=MAXD3DDECLLENGTH+1&&SUCCEEDED(decl->GetDeclaration(e,&n)))
  for(UINT i=0;i<n&&e[i].Stream!=0xff;++i)if(e[i].Usage==D3DDECLUSAGE_POSITION&&e[i].UsageIndex==0){
   ok=!found&&e[i].Stream==0&&e[i].Type==D3DDECLTYPE_SHORT4N;found=true;}
 // The entry keeps its reference, so the address cannot be reused by another declaration.
 try {declNormalized.emplace(decl,ok);}catch(...){decl->Release();return false;}
 return ok;
}
static void BuildCasterBox(IDirect3DDevice9*d,CasterBox&box){
 box.valid=false;if(!cullEnabled)return;
 float c[8]={};
 if(!PositionNormalized(d)||FAILED(d->GetVertexShaderConstantF(7,c,2))||c[3]!=0){++cullUnbounded;return;}
 for(float v:c)if(!std::isfinite(v)){++cullUnbounded;return;}
 for(unsigned k=0;k<8;++k){
  for(unsigned a=0;a<3;++a){const float s=std::fabs(c[a]);box.corner[k][a]=c[4+a]+((k>>a)&1u?s:-s);}
  box.corner[k][3]=c[7];
 }
 box.valid=true;
}
// m holds the four clip rows exactly as uploaded to the shader.
static bool OutsideClip(const CasterBox&box,const float*m){
 if(!box.valid)return false;
 unsigned all=63;
 for(unsigned k=0;k<8&&all;++k){
  float p[4];for(unsigned r=0;r<4;++r)p[r]=m[r*4]*box.corner[k][0]+m[r*4+1]*box.corner[k][1]+m[r*4+2]*box.corner[k][2]+m[r*4+3]*box.corner[k][3];
  // A margin keeps rounding differences with the GPU on the drawn side.
  const float e=1e-4f*(std::fabs(p[3])+1.f);unsigned out=0;
  if(p[0]+p[3]<-e)out|=1;if(p[3]-p[0]<-e)out|=2;if(p[1]+p[3]<-e)out|=4;
  if(p[3]-p[1]<-e)out|=8;if(p[2]<-e)out|=16;if(p[3]-p[2]<-e)out|=32;
  all&=out;
 }
 return all!=0;
}
// Submission wall time of the shadow replays, last completed frame. Like the
// other CPU timers this is not GPU duration.
struct CpuStats {
 double cascadePrepareMs=0,cascadeReplayMs=0,localReplayMs=0;
 unsigned cascadeCasters=0,cascadeDraws=0,localCasters=0,localDraws=0;
 unsigned cascadeCulled=0,localCulled=0;
 unsigned saAvailable=0,saDraws=0,saCulled=0;  // SA district casters (cascade_probe.h)
};
static CpuStats cpuCurrent,cpuLast;
// Whole draw-hook wall time (our scopes, the driver and the native call),
// split by what the hook did to the draw. Diagnostic only.
struct HookStats {double ms[4]={};unsigned calls[4]={};};
enum HookKind {HookOther=0,HookDeferred=1,HookGiTargets=2,HookShadowReplay=3};
static HookStats hookCurrent,hookLast;
struct HookTimer {
 // Two QPC calls per draw; only while [Profiler] Hooks=1.
 LARGE_INTEGER start={};unsigned shadowDraws=0;int kind=HookOther;bool on=WosHookProfile::enabled;
 HookTimer(){if(!on)return;QueryPerformanceCounter(&start);shadowDraws=cpuCurrent.localDraws+cpuCurrent.cascadeDraws;}
 ~HookTimer(){
  if(!on)return;
  LARGE_INTEGER end,freq;QueryPerformanceCounter(&end);QueryPerformanceFrequency(&freq);
  if(cpuCurrent.localDraws+cpuCurrent.cascadeDraws!=shadowDraws)kind=HookShadowReplay;
  if(freq.QuadPart>0)hookCurrent.ms[kind]+=1000.0*double(end.QuadPart-start.QuadPart)/double(freq.QuadPart);
  ++hookCurrent.calls[kind];
 }
};
// Colour slots a shadow binding saves and restores. Device caps are fixed for
// the device's lifetime, so the per-caster bindings ask once, not per draw.
static IDirect3DDevice9*slotsDevice=nullptr;static unsigned slotsCount=0;
static unsigned RenderTargetSlots(IDirect3DDevice9*d){
 if(d!=slotsDevice){D3DCAPS9 caps={};if(!d||FAILED(d->GetDeviceCaps(&caps)))return 0;
  slotsDevice=d;slotsCount=caps.NumSimultaneousRTs<4?unsigned(caps.NumSimultaneousRTs):4u;}
 return slotsCount;
}
// The cascade timers belong to the probe, which resets them only when it
// prepares, so a frame without cascade casters reports zero instead of a
// stale total.
static void EndFrame(double cascadePrepareMs,double cascadeReplayMs){
 if(cpuCurrent.cascadeCasters){cpuCurrent.cascadePrepareMs=cascadePrepareMs;cpuCurrent.cascadeReplayMs=cascadeReplayMs;}
 cpuLast=cpuCurrent;cpuCurrent=CpuStats();hookLast=hookCurrent;hookCurrent=HookStats();
}
static void WriteJson(FILE*f){
 std::fprintf(f,"\"shadow_work_cache\":{\"enabled\":%s,\"local_hits\":%llu,\"local_misses\":%llu,\"local_fallbacks\":%llu,\"projected_instances\":%llu,\"avoided_node_copies\":%llu},",enabled?"true":"false",local.hits,local.misses,local.fallbacks,projectionInstances,avoidedNodeCopies);
 std::fprintf(f,"\"shadow_cpu\":{\"cascade_prepare_ms\":%.4f,\"cascade_replay_ms\":%.4f,\"local_replay_ms\":%.4f,\"cascade_casters\":%u,\"cascade_draws\":%u,\"local_casters\":%u,\"local_draws\":%u,\"cull\":{\"enabled\":%s,\"cascade_culled\":%u,\"local_culled\":%u,\"unbounded_total\":%llu},\"sa_casters\":{\"available\":%u,\"draws\":%u,\"culled\":%u},\"scope\":\"last_frame_submission_wall_time_not_gpu_duration\"},",
  cpuLast.cascadePrepareMs,cpuLast.cascadeReplayMs,cpuLast.localReplayMs,cpuLast.cascadeCasters,cpuLast.cascadeDraws,cpuLast.localCasters,cpuLast.localDraws,
  cullEnabled?"true":"false",cpuLast.cascadeCulled,cpuLast.localCulled,cullUnbounded,cpuLast.saAvailable,cpuLast.saDraws,cpuLast.saCulled);
 std::fprintf(f,"\"draw_hooks\":{\"other_ms\":%.4f,\"other_calls\":%u,\"deferred_ms\":%.4f,\"deferred_calls\":%u,\"gi_targets_ms\":%.4f,\"gi_targets_calls\":%u,\"shadow_replay_ms\":%.4f,\"shadow_replay_calls\":%u,\"scope\":\"whole_hook_wall_time_last_frame\"},",
  hookLast.ms[0],hookLast.calls[0],hookLast.ms[1],hookLast.calls[1],hookLast.ms[2],hookLast.calls[2],hookLast.ms[3],hookLast.calls[3]);
}
}
