#pragma once
// Pure x86 decoder for a read-only F7 diagnostic. It never scans a stack or calls engine code.
#include <cstdint>
#include <cstddef>
#include <cstring>
namespace WosNativeMaterialContext {
enum class Kind : uint32_t { DP, DIP, Unsupported };
struct DrawArgs {
    uint32_t type=0, start=0, primitives=0;
    int32_t base=0;
    uint32_t minVertex=0, vertices=0;
    static DrawArgs Indexed(uint32_t t,int32_t b,uint32_t m,uint32_t v,uint32_t s,uint32_t p) {
        DrawArgs a; a.type=t;a.base=b;a.minVertex=m;a.vertices=v;a.start=s;a.primitives=p;return a;
    }
    static DrawArgs Primitive(uint32_t t,uint32_t s,uint32_t p) {
        DrawArgs a;a.type=t;a.start=s;a.primitives=p;return a;
    }
};
struct Bounds { uint64_t begin=0,end=0; };
enum class Reason : unsigned {
    Accepted, UnsupportedKind, StackBounds, StackRead, ReturnMismatch, UnknownCaller,
    UnknownParent, ArenaBounds, NodeBounds, NodeRead, NodeType, InstanceBounds,
    InstanceRead, GeometryPointer, GeometryRead, RecordPointer, RecordRead,
    MaterialPointer, SavedGeometryMismatch, WrapperGeometryMismatch,
    PrimitiveCount, DrawArgsMismatch, Count
};
static const char* ReasonName(Reason r) {
    static const char* names[]={"accepted","unsupported_kind","stack_bounds","stack_read",
        "return_mismatch","unknown_caller","unknown_parent","arena_bounds","node_bounds",
        "node_read","node_type","instance_bounds","instance_read","geometry_pointer",
        "geometry_read","record_pointer","record_read","material_pointer",
        "saved_geometry_mismatch","wrapper_geometry_mismatch","primitive_count","draw_args_mismatch"};
    const unsigned i=static_cast<unsigned>(r);
    return i<static_cast<unsigned>(Reason::Count)?names[i]:"invalid_reason";
}
enum class Route : unsigned {
    None, IndexedQueue, IndexedQueueMulti, IndexedVirtual, IndexedVirtualMulti,
    PrimitiveQueue, PrimitiveQueueRanges, PrimitiveQueueMulti, PrimitiveVirtualMulti
};
static const char* RouteName(Route r) {
    static const char* names[]={"none","dip_queue","dip_queue_multi","dip_virtual",
        "dip_virtual_multi","dp_queue","dp_queue_ranges","dp_queue_multi","dp_virtual_multi"};
    const unsigned i=static_cast<unsigned>(r);return i<9?names[i]:"invalid_route";
}
static bool Pointer(uint32_t p) { return p>=0x10000 && (p&3)==0; }
static bool Contains(Bounds b,uint64_t p,size_t bytes) {
    return b.begin>=0x10000 && b.end>b.begin && b.end<=0x100000000ull &&
        p>=b.begin && p<=b.end && bytes<=b.end-p;
}
static uint32_t Word(const unsigned char* p,size_t off=0) {
    uint32_t x=0;std::memcpy(&x,p+off,4);return x;
}
static bool MakeArena(const uint32_t (&a)[3],Bounds &b) {
    b={};const uint64_t capacityEnd=uint64_t(a[0])+a[1];
    if(!Pointer(a[0]) || a[1]==0 || a[1]>64u*1024u*1024u ||
       capacityEnd>0x100000000ull || a[2]<=a[0] || a[2]>capacityEnd)return false;
    b={a[0],a[2]};return true;
}
static bool PrimitiveCount(uint32_t type,uint32_t elements,uint32_t &count) {
    switch(type) {
    case 1:count=elements;return true;
    case 2:count=elements/2;return true;
    case 3:if(elements<1)return false;count=elements-1;return true;
    case 4:count=elements/3;return true;
    case 5:case 6:if(elements<2)return false;count=elements-2;return true;
    default:return false;
    }
}
struct Context {
    Route route=Route::None;
    uint32_t caller=0,parent=0,grandparent=0,node=0,record=0,activeMaterial=0;
    uint32_t instance=0,geometry=0,state=0,sourceMaterial=0,pass=0,descriptor=0;
    unsigned char nodeBytes[0x20]={},instanceBytes[0xb0]={},geometryBytes[0x54]={};
    bool wrapperMatched=false,drawArgsMatched=false;
};
// read(address, destination, bytes) must fail instead of dereferencing inaccessible memory.
// Every stack read is inside the supplied current-thread bounds. No unknown route is guessed.
template<class Reader> static Reason Decode(Reader &read,Kind kind,uint32_t caller,uint32_t slot,
    const DrawArgs &args,Bounds stack,Bounds arena,Context &out) {
    out={};out.caller=caller;
    if(kind!=Kind::DIP && kind!=Kind::DP)return Reason::UnsupportedKind;
    unsigned char s[0x80]={};
    if(!Pointer(slot) || !Contains(stack,slot,sizeof(s)))return Reason::StackBounds;
    if(!read(slot,s,sizeof(s)))return Reason::StackRead;
    if(Word(s)!=caller)return Reason::ReturnMismatch;
    if(!Contains(arena,arena.begin,1) || arena.end-arena.begin>64u*1024u*1024u)return Reason::ArenaBounds;
    if(kind==Kind::DIP) {
        if(caller!=0x007ad1a6)return Reason::UnknownCaller;
        out.parent=Word(s,0x2c);
        switch(out.parent) {
        case 0x009e0de3:out.route=Route::IndexedQueue;out.node=Word(s,0x68);out.record=Word(s,0x64);break;
        case 0x009e0690:out.route=Route::IndexedVirtual;out.node=Word(s,0x24);break;
        case 0x009e5edb:
            out.grandparent=Word(s,0x58);
            if(out.grandparent==0x009e0d55) {
                out.route=Route::IndexedQueueMulti;out.node=Word(s,0x7c);out.record=Word(s,0x78);
            } else if(out.grandparent==0x009e0710) {
                out.route=Route::IndexedVirtualMulti;out.node=Word(s,0x54);
            } else return Reason::UnknownParent;
            break;
        default:return Reason::UnknownParent;
        }
    } else {
        if(caller==0x009e0ef1 || caller==0x009e0e87) {
            out.route=caller==0x009e0ef1?Route::PrimitiveQueue:Route::PrimitiveQueueRanges;
            out.node=Word(s,0x28);out.record=Word(s,0x24);
        } else if(caller==0x009e5f19) {
            out.parent=Word(s,0x24);
            if(out.parent==0x009e0d55) {
                out.route=Route::PrimitiveQueueMulti;out.node=Word(s,0x48);out.record=Word(s,0x44);
            } else if(out.parent==0x009e0710) {
                out.route=Route::PrimitiveVirtualMulti;out.node=Word(s,0x20);
            } else return Reason::UnknownParent;
        } else return Reason::UnknownCaller;
    }
    if((out.node&15) || !Contains(arena,out.node,sizeof(out.nodeBytes)))return Reason::NodeBounds;
    if(!read(out.node,out.nodeBytes,sizeof(out.nodeBytes)))return Reason::NodeRead;
    if(Word(out.nodeBytes)!=0x00db7b84)return Reason::NodeType;
    out.instance=Word(out.nodeBytes,0xc);out.geometry=Word(out.nodeBytes,0x10);
    out.state=Word(out.nodeBytes,0x14);out.sourceMaterial=Word(out.nodeBytes,0x18);
    out.pass=Word(out.nodeBytes,0x1c);
    if((out.instance&15) || !Contains(arena,out.instance,sizeof(out.instanceBytes)))return Reason::InstanceBounds;
    if(!read(out.instance,out.instanceBytes,sizeof(out.instanceBytes)))return Reason::InstanceRead;
    if(!Pointer(out.geometry) || uint64_t(out.geometry)+sizeof(out.geometryBytes)>0x100000000ull)return Reason::GeometryPointer;
    if(!read(out.geometry,out.geometryBytes,sizeof(out.geometryBytes)))return Reason::GeometryRead;
    out.descriptor=Word(out.geometryBytes,0x20);
    if(out.record) {
        if(!Pointer(out.record))return Reason::RecordPointer;
        if(!read(out.record,&out.activeMaterial,4))return Reason::RecordRead;
    } else if(out.route==Route::IndexedVirtual || out.route==Route::IndexedVirtualMulti ||
              out.route==Route::PrimitiveVirtualMulti)out.activeMaterial=out.sourceMaterial;
    else return Reason::RecordPointer;
    if(!Pointer(out.activeMaterial))return Reason::MaterialPointer;
    if(kind==Kind::DIP) {
        if(out.route==Route::IndexedQueue && Word(s,0x20)!=out.geometry)return Reason::SavedGeometryMismatch;
        // All four indexed routes pass the same nine immutable geometry-derived wrapper arguments.
        const uint32_t expected[]={Word(out.geometryBytes,0x50),Word(out.geometryBytes,0x44),
            Word(out.geometryBytes,0x40),out.geometry+0x3c,Word(out.geometryBytes,0x48)==2?101u:102u,
            Word(out.geometryBytes,0x34),Word(out.geometryBytes,0x30),out.geometry+0x2c,Word(out.geometryBytes,0x4c)};
        for(unsigned i=0;i<9;++i)if(Word(s,0x30+4*i)!=expected[i])return Reason::WrapperGeometryMismatch;
        out.wrapperMatched=true;
        uint32_t primitives=0;
        if(!PrimitiveCount(expected[0],expected[1],primitives))return Reason::PrimitiveCount;
        if(args.type!=expected[0] || args.base!=0 || args.minVertex!=0 || args.vertices!=expected[5] ||
            args.start!=expected[2]/(expected[4]==101?2:4) || args.primitives!=primitives)return Reason::DrawArgsMismatch;
    } else {
        if(args.type!=Word(out.geometryBytes,0x50) || Word(out.geometryBytes,0x44)!=0)return Reason::DrawArgsMismatch;
        if(out.route!=Route::PrimitiveQueueRanges) {
            uint32_t count=0;if(!PrimitiveCount(args.type,Word(out.geometryBytes,0x34),count))return Reason::PrimitiveCount;
            if(args.start!=0 || args.primitives!=count)return Reason::DrawArgsMismatch;
        }
        // The ranges route takes start/count from a separate per-instance list. Do not invent full-mesh equality.
    }
    out.drawArgsMatched=true;return Reason::Accepted;
}
}
