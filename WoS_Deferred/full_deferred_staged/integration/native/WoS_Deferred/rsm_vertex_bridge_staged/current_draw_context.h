#pragma once
// Portable current-draw observation adapter; no COM calls, globals, F7 IDs, I/O or engine calls.
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/native_material_context_decode.h"
namespace WosCurrentDraw {
using namespace WosNativeMaterialContext;
struct Scene {uint32_t current=0,nearScene=0,farScene=0,shadowActive=0,epoch=0,valid=0,arena[3]={};};
struct Bindings {bool complete=false;uint32_t vb=0,ib=0,offset=0,stride=0,declaration=0;};
struct Result {
 bool valid=false;const char* reason="inactive";Reason nativeReason=Reason::UnsupportedKind;
 uint64_t frame=0,draw=0;Scene scene{};Context native{};Bindings bindings{};uint32_t vertexDescriptor=0;
};
inline bool SameScene(const Scene&a,const Scene&b){return a.current==b.current&&a.nearScene==b.nearScene&&a.farScene==b.farScene&&a.shadowActive==b.shadowActive&&a.epoch==b.epoch&&a.valid==b.valid&&!std::memcmp(a.arena,b.arena,sizeof(a.arena));}
inline bool Current(const Result&r,uint64_t frame,uint64_t draw,uint32_t scene,uint32_t epoch){return r.valid&&r.frame==frame&&r.draw==draw&&r.scene.current==scene&&r.scene.epoch==epoch;}
// sceneReader must read current native values; bindingReader must independently query actual stream0/IB/declaration.
// Callers retain returned COM references through their original draw; pointers here are comparisons, not ownership.
template<class Reader,class SceneReader,class BindingReader>
Result Resolve(Reader&read,SceneReader&sceneReader,BindingReader&bindingReader,bool enabled,bool verified,
 Kind kind,uint32_t caller,uint32_t returnSlot,const DrawArgs&args,Bounds stack,uint64_t frame,uint64_t draw){
 Result out;out.frame=frame;out.draw=draw;
 if(!enabled||!verified||!frame||kind==Kind::Unsupported)return out;
 out.reason="scene";
 if(!sceneReader(out.scene)||out.scene.valid!=7||out.scene.shadowActive!=1||!out.scene.nearScene||!out.scene.farScene||out.scene.nearScene==out.scene.farScene||
   (out.scene.current!=out.scene.nearScene&&out.scene.current!=out.scene.farScene))return out;
 Bounds arena;out.reason="arena";if(!MakeArena(out.scene.arena,arena))return out;
 out.reason="native_context";out.nativeReason=Decode(read,kind,caller,returnSlot,args,stack,arena,out.native);
 if(out.nativeReason!=Reason::Accepted)return out;
 out.reason="descriptor_state";if(out.native.descriptor!=out.native.state)return out;
 out.vertexDescriptor=Word(out.native.geometryBytes,0x4c);unsigned char vertex[12]={};out.reason="vertex_descriptor";
 if(!Pointer(out.vertexDescriptor)||uint64_t(out.vertexDescriptor)+sizeof(vertex)>0x100000000ull||!read(out.vertexDescriptor,vertex,sizeof(vertex)))return out;
 out.reason="geometry_binding";if(!bindingReader(kind,out.bindings)||!out.bindings.complete)return out;
 const uint32_t expectedOffset=kind==Kind::DIP||out.native.route==Route::PrimitiveQueue?Word(out.native.geometryBytes,0x30):0;
 if(out.bindings.vb!=Word(out.native.geometryBytes,0x2c)||out.bindings.offset!=expectedOffset||out.bindings.stride!=Word(vertex)||
   out.bindings.declaration!=Word(vertex,8)||(kind==Kind::DIP&&out.bindings.ib!=Word(out.native.geometryBytes,0x3c)))return out;
 Scene after;out.reason="changed";if(!sceneReader(after)||!SameScene(out.scene,after))return out;
 out.valid=true;out.reason="accepted";return out;
}
}
