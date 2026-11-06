#pragma once
// CPU-only staged state machine. Does not install hooks, call D3D, own COM,
// read game memory or make a flux/GI claim. See INTEGRATION.md for the caller
// obligations: every ID is canonical and pinned for the whole capture epoch.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
namespace WosRsmClear {
constexpr unsigned MaxRects=64,MaxDimension=16384;
struct Rect {int32_t left=0,top=0,right=0,bottom=0;};
struct Area {
 unsigned width=0,height=0;Rect viewport{};bool viewportKnown=false;
 bool scissorKnown=false,scissorEnabled=false;Rect scissor{};
 unsigned count=0;bool pointerWasNull=true,rectsCopied=true;
 std::array<Rect,MaxRects> rects{};
};
enum class Coverage {Unknown,Empty,Partial,Full};
inline Rect Intersect(Rect a,Rect b){return {std::max(a.left,b.left),std::max(a.top,b.top),std::min(a.right,b.right),std::min(a.bottom,b.bottom)};}
inline bool Empty(Rect r){return r.right<=r.left||r.bottom<=r.top;}
inline bool Ordered(Rect r){return r.right>=r.left&&r.bottom>=r.top;}
// D3D9 Clear rects are clipped to viewport and enabled scissor. Compute the
// exact bounded union (integer pixel rectangle coordinates, half-open bounds).
inline Coverage Classify(const Area& a){
 if(!a.width||!a.height||a.width>MaxDimension||a.height>MaxDimension||!a.viewportKnown||!a.scissorKnown||!Ordered(a.viewport))return Coverage::Unknown;
 Rect surface{0,0,int32_t(a.width),int32_t(a.height)};
 if(a.viewport.left<0||a.viewport.top<0||a.viewport.right>surface.right||a.viewport.bottom>surface.bottom)return Coverage::Unknown;
 if((a.count==0)!=a.pointerWasNull||a.count>MaxRects||!a.rectsCopied)return Coverage::Unknown;
 Rect clip=Intersect(surface,a.viewport);
 if(a.scissorEnabled){if(!Ordered(a.scissor)||a.scissor.left<0||a.scissor.top<0||a.scissor.right>surface.right||a.scissor.bottom>surface.bottom)return Coverage::Unknown;clip=Intersect(clip,a.scissor);}
 std::array<Rect,MaxRects> rects{};unsigned count=0;
 if(a.count==0){if(!Empty(clip))rects[count++]=clip;}
 else for(unsigned i=0;i<a.count;++i){if(!Ordered(a.rects[i]))return Coverage::Unknown;Rect r=Intersect(a.rects[i],clip);if(!Empty(r))rects[count++]=r;}
 if(!count)return Coverage::Empty;
 std::array<int32_t,MaxRects*2> xs{};unsigned nx=0;
 for(unsigned i=0;i<count;++i){xs[nx++]=rects[i].left;xs[nx++]=rects[i].right;}
 std::sort(xs.begin(),xs.begin()+nx);nx=unsigned(std::unique(xs.begin(),xs.begin()+nx)-xs.begin());uint64_t pixels=0;
 for(unsigned x=1;x<nx;++x){std::array<Rect,MaxRects> spans{};unsigned n=0;
  for(unsigned i=0;i<count;++i)if(rects[i].left<=xs[x-1]&&rects[i].right>=xs[x])spans[n++]=rects[i];
  std::sort(spans.begin(),spans.begin()+n,[](Rect a,Rect b){return a.top<b.top;});
  int32_t bottom=0;uint64_t height=0;
  for(unsigned i=0;i<n;++i){if(spans[i].bottom>bottom){height+=uint32_t(spans[i].bottom-std::max(bottom,spans[i].top));bottom=spans[i].bottom;}}
  pixels+=uint64_t(xs[x]-xs[x-1])*height;
 }
 return pixels==uint64_t(a.width)*a.height?Coverage::Full:Coverage::Partial;
}
struct Key {uint64_t device=0,depth=0;};
inline bool operator==(Key a,Key b){return a.device==b.device&&a.depth==b.depth;}
struct Context {
 Key key{};uint64_t color=0;uint32_t scene=0;unsigned slot=0,width=0,height=0;
 float vp[16]={};bool verifiedNativeShadow=false,identitiesPinned=false;
};
inline bool Valid(const Context& c){
 if(!c.verifiedNativeShadow||!c.identitiesPinned||!c.key.device||!c.key.depth||!c.color||!c.scene||c.slot>1||!c.width||!c.height||c.width>MaxDimension||c.height>MaxDimension)return false;
 double magnitude=0;for(float v:c.vp){if(!std::isfinite(v)||std::abs(v)>1e8f)return false;magnitude+=std::abs(v);}return magnitude>1e-8;
}
inline bool Same(const Context&a,const Context&b){return a.key==b.key&&a.color==b.color&&a.scene==b.scene&&a.slot==b.slot&&a.width==b.width&&a.height==b.height&&!std::memcmp(a.vp,b.vp,sizeof(a.vp));}
enum class Reason {None,MissingFullClear,PartialClear,UnknownClearArea,UnverifiedClear,ClearFailed,ContextChanged,IdentityAlias,AuxiliaryAlias,AuxiliaryMissing,AuxiliaryFailed,UnknownWriter,DrawFailed,RestoreFailed,StaleWriter,StaleAuxiliary,ObservationFailure};
inline const char* Name(Reason r){switch(r){case Reason::None:return "ready";case Reason::MissingFullClear:return "full native clear not observed";case Reason::PartialClear:return "partial native depth clear";case Reason::UnknownClearArea:return "native clear area unavailable";case Reason::UnverifiedClear:return "unverified depth clear origin";case Reason::ClearFailed:return "native clear failed";case Reason::ContextChanged:return "depth scene color or VP changed";case Reason::IdentityAlias:return "near/far depth identity alias";case Reason::AuxiliaryAlias:return "near/far normal identity alias";case Reason::AuxiliaryMissing:return "normal zero is not in this depth generation";case Reason::AuxiliaryFailed:return "normal zero or MRT write failed";case Reason::UnknownWriter:return "unknown depth writer";case Reason::DrawFailed:return "native draw failed";case Reason::RestoreFailed:return "draw state restore failed";case Reason::StaleWriter:return "writer completion crossed a generation";case Reason::StaleAuxiliary:return "normal clear completion crossed a generation";default:return "observation failed";}}
struct Stamp {uint64_t epoch=0,generation=0;unsigned slot=0;Key key{};};
struct WriteToken {Stamp stamp{};uint64_t sequence=0,normal=0;bool geometric=false;};
struct Snapshot {Stamp stamp{};uint64_t revision=0,normal=0,frame=0;};
struct ClearEvent {
 Area area{};uint32_t flags=0;float depthValue=1;bool nativeSucceeded=false;
 // True only for a verified native call/context. A same-key clear from any
 // other origin must still be reported, but cannot bootstrap trusted data.
 bool verifiedNativeOrigin=false;
};
struct MapState {
 bool occupied=false,invalid=true,fullClear=false,pending=false,alias=false,normalAlias=false;
 Context context{};uint64_t generation=0,normalGeneration=0,normal=0,revision=0,writerSequence=0;
 unsigned fullClears=0,partialClears=0,emptyClears=0,normalDraws=0,guardDraws=0;
 Reason reason=Reason::MissingFullClear;
};
class Tracker {
 std::array<MapState,2> maps_{};uint64_t epoch_=0,serial_=0,frame_=0;bool active_=false;
 uint64_t Next(){if(serial_==std::numeric_limits<uint64_t>::max()){active_=false;return 0;}return ++serial_;}
 void Fail(MapState&m,Reason r){if(!m.invalid||m.reason==Reason::None||m.reason==Reason::MissingFullClear)m.reason=r;m.invalid=true;m.pending=false;m.normalGeneration=0;m.revision=Next();}
 // A late completion can still touch reused auxiliary storage after the old
 // depth key has been replaced. Invalidate every currently affected pair.
 void FailCompletion(Key key,uint64_t normal,Reason r){if(!active_)return;for(auto&m:maps_)if(m.occupied&&(m.context.key==key||(normal&&m.normal==normal)))Fail(m,r);}
 MapState* Existing(Key key){for(auto&m:maps_)if(m.occupied&&m.context.key==key)return &m;return nullptr;}
 MapState* Resolve(const Context& c){
  if(!active_)return nullptr;
  if(!Valid(c)){if(auto*m=Existing(c.key))Fail(*m,Reason::ContextChanged);return nullptr;}
  auto& m=maps_[c.slot];auto& other=maps_[1-c.slot];
  if(other.occupied&&other.context.key==c.key){other.alias=true;Fail(other,Reason::IdentityAlias);if(!m.occupied){m.context=c;m.occupied=true;}m.alias=true;Fail(m,Reason::IdentityAlias);return nullptr;}
  if(!m.occupied||!Same(m.context,c)){m=MapState{};m.occupied=true;m.context=c;m.generation=Next();m.revision=Next();}
  return &m;
 }
 bool Match(const Stamp&s,MapState*&m){if(!active_||s.epoch!=epoch_||s.slot>1)return false;m=&maps_[s.slot];return m->occupied&&m->context.key==s.key&&m->generation==s.generation;}
 Stamp MakeStamp(const MapState&m)const{return {epoch_,m.generation,m.context.slot,m.context.key};}
public:
 void Reset(){active_=false;maps_={};frame_=0;epoch_=Next();}
 void BeginCapture(uint64_t frame,bool executableVerified){Reset();frame_=frame;active_=executableVerified&&frame!=0&&epoch_!=0;}
 void EndCapture(){Reset();}
 bool Active()const{return active_;}
 const MapState& State(unsigned slot)const{return maps_.at(slot);}
 // Call only after a complete actual context query at Clear/draw time. Raw
 // SetRT/SetDS calls are transient bindings and must NOT create generations.
 bool ObserveContext(const Context& c){return Resolve(c)!=nullptr;}
 bool OnNativeClear(const Context& c,const ClearEvent&e,Stamp&out){
  out={};if(!active_||!(e.flags&2u))return false; // Color/stencil-only clears do not modify depth/normal.
  auto*m=Resolve(c);if(!m)return false;
  if(!e.nativeSucceeded){Fail(*m,Reason::ClearFailed);return false;}
  if(e.area.width!=c.width||e.area.height!=c.height){Fail(*m,Reason::UnknownClearArea);return false;}
  Coverage coverage=Classify(e.area);
  if(coverage==Coverage::Unknown){Fail(*m,Reason::UnknownClearArea);return false;}
  if(coverage==Coverage::Empty){++m->emptyClears;return false;}
  m->generation=Next();m->revision=Next();m->normalGeneration=0;m->normal=0;m->pending=false;m->normalDraws=m->guardDraws=0;m->fullClear=false;m->invalid=true;
  if(coverage!=Coverage::Full){++m->partialClears;m->reason=Reason::PartialClear;return false;}
  ++m->fullClears;
  if(!e.verifiedNativeOrigin||!std::isfinite(e.depthValue)||e.depthValue!=1.f){m->reason=Reason::UnverifiedClear;return false;}
  if(m->alias||m->normalAlias){m->reason=m->alias?Reason::IdentityAlias:Reason::AuxiliaryAlias;return false;}
  m->fullClear=true;m->invalid=false;m->reason=Reason::None;out=MakeStamp(*m);return true;
 }
 bool ConfirmNormalZero(const Context&c,Stamp stamp,uint64_t normal,unsigned width,unsigned height,bool allTextureZeroSucceeded){
  MapState*m=nullptr;
  if(!Match(stamp,m)){FailCompletion(stamp.key,normal,Reason::StaleAuxiliary);return false;}
  if(!Valid(c)||!Same(m->context,c)){FailCompletion(stamp.key,normal,Reason::ContextChanged);return false;}
  if(m->invalid||!m->fullClear||m->pending||m->normalDraws||m->guardDraws||m->normalGeneration){FailCompletion(stamp.key,normal,Reason::AuxiliaryMissing);return false;}
  if(!allTextureZeroSucceeded||!normal||width!=c.width||height!=c.height){FailCompletion(stamp.key,normal,Reason::AuxiliaryFailed);return false;}
  for(auto&other:maps_)if(&other!=m&&other.occupied&&other.normal==normal){m->normalAlias=other.normalAlias=true;Fail(*m,Reason::AuxiliaryAlias);Fail(other,Reason::AuxiliaryAlias);return false;}
  m->normal=normal;m->normalGeneration=m->generation;m->revision=Next();return true;
 }
 bool BeginWriter(const Context&c,uint64_t normal,bool geometric,WriteToken&out){
  out={};auto*m=Resolve(c);if(!m)return false;
  if(m->invalid||!m->fullClear||m->normalGeneration!=m->generation||m->normal!=normal||!normal||m->pending){Fail(*m,Reason::AuxiliaryMissing);return false;}
  m->pending=true;m->writerSequence=Next();m->revision=Next();out={MakeStamp(*m),m->writerSequence,normal,geometric};return true;
 }
 bool FinishWriter(const WriteToken&t,bool nativeSucceeded,bool auxiliarySucceeded,bool restoreSucceeded){
  MapState*m=nullptr;
  if(!Match(t.stamp,m)){FailCompletion(t.stamp.key,t.normal,Reason::StaleWriter);return false;}
  if(!m->pending||m->writerSequence!=t.sequence||m->normal!=t.normal||m->invalid){FailCompletion(t.stamp.key,t.normal,Reason::StaleWriter);return false;}
  m->pending=false;m->revision=Next();
  if(!nativeSucceeded){Fail(*m,Reason::DrawFailed);return false;}
  if(!auxiliarySucceeded){Fail(*m,Reason::AuxiliaryFailed);return false;}
  if(!restoreSucceeded){Fail(*m,Reason::RestoreFailed);return false;}
  if(t.geometric)++m->normalDraws;else ++m->guardDraws;return true;
 }
 void UncoveredWriter(Key key){if(active_)if(auto*m=Existing(key))Fail(*m,Reason::UnknownWriter);}
 void ObservationFailed(Key key={}){if(!active_)return;if(key.device&&key.depth){if(auto*m=Existing(key))Fail(*m,Reason::ObservationFailure);}else for(auto&m:maps_)if(m.occupied)Fail(m,Reason::ObservationFailure);}
 bool Get(const Context&c,uint64_t normal,Snapshot&out)const{
  out={};if(!active_||!Valid(c))return false;const auto&m=maps_[c.slot];
  if(!m.occupied||!Same(m.context,c)||m.invalid||m.alias||m.normalAlias||m.pending||!m.fullClear||!m.normalDraws||m.normalGeneration!=m.generation||m.normal!=normal)return false;
  out={MakeStamp(m),m.revision,normal,frame_};return true;
 }
 bool StillCurrent(const Snapshot&t)const{
  if(!active_||t.stamp.epoch!=epoch_||t.stamp.slot>1||t.frame!=frame_)return false;const auto&m=maps_[t.stamp.slot];
  return m.occupied&&!m.invalid&&!m.alias&&!m.normalAlias&&!m.pending&&m.fullClear&&m.normalDraws&&m.context.key==t.stamp.key&&m.generation==t.stamp.generation&&m.normalGeneration==m.generation&&m.normal==t.normal&&m.revision==t.revision;
 }
};
}
