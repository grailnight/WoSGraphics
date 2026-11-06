#pragma once
// Include after renderer_probe.h. Diagnostic only: no engine calls, hooks or D3D state writes.
#include <d3d9.h>
#include <array>
#include <unordered_map>
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/native_material_context_decode.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/native_material_context_signatures.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/native_material_context_compat.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/current_sun_decode.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/current_sun_gates.h"
namespace WosNativeMaterialContext {
static FILE *contextLog=nullptr,*contextRaw=nullptr;
static std::string contextFolder;
static bool contextEnabled=false,contextIoError=false,contextCurrentInputGate=false;
static unsigned contextRows=0,contextSnapshots=0,contextRawBytes=0;
static unsigned contextGlobalRejects=0,contextChangedRejects=0,contextSnapshotFailures=0;
static unsigned contextSnapshotLimit=0,contextBindingFailures=0,contextBindingMismatches=0;
static std::array<unsigned,static_cast<unsigned>(Reason::Count)> contextReasons={};
static std::unordered_map<std::string,unsigned> contextObjects;
constexpr unsigned ContextDrawLimit=10000,ContextSnapshotLimit=32768,ContextByteLimit=8u*1024u*1024u;
constexpr unsigned ContextSourceEntryLimit=2048,ContextSourceGroupLimit=64,ContextSourceGroupEntryLimit=32,ContextSourceDescriptorLimit=128,ContextSourceEffectDescriptorLimit=2;
struct LocalReader {
    bool operator()(uint32_t address,void* out,size_t bytes) const {
        if(uint64_t(address)+bytes>0x100000000ull){std::memset(out,0,bytes);return false;}
        return WosEngineProbe::Read(address,out,bytes);
    }
};
static void ClearParameterGroups();
static void ClearMaterialSources();
static void Finish(const char *reason="FRAME_END") {
    if(contextRaw) { if(std::ferror(contextRaw))contextIoError=true;if(std::fclose(contextRaw))contextIoError=true;contextRaw=nullptr; }
    if(contextLog) {
        contextIoError=contextIoError || std::ferror(contextLog)!=0;
        std::fprintf(contextLog,"{\"event\":\"end\",\"reason\":\"%s\",\"rows\":%u,\"snapshots\":%u,\"raw_bytes\":%u,\"io_error\":%u,\"global_rejects\":%u,\"changed_rejects\":%u,\"snapshot_failures\":%u,\"snapshot_limit\":%u,\"binding_failures\":%u,\"binding_mismatches\":%u,\"reasons\":{",
            reason,contextRows,contextSnapshots,contextRawBytes,contextIoError?1u:0u,contextGlobalRejects,
            contextChangedRejects,contextSnapshotFailures,contextSnapshotLimit,contextBindingFailures,contextBindingMismatches);
        for(unsigned i=0;i<contextReasons.size();++i)std::fprintf(contextLog,"%s\"%s\":%u",i?",":"",ReasonName(static_cast<Reason>(i)),contextReasons[i]);
        std::fprintf(contextLog,"}}\n");std::fclose(contextLog);contextLog=nullptr;
    }
    contextEnabled=false;contextCurrentInputGate=false;contextObjects.clear();ClearParameterGroups();ClearMaterialSources();
}
// Lazily starts on the first native draw of an existing F7 capture. Engine probe already hashes the EXE once per F7.
static void Begin() {
    Finish("RESTART");contextFolder=WosProbe::folder;contextRows=contextSnapshots=contextRawBytes=0;
    contextGlobalRejects=contextChangedRejects=contextSnapshotFailures=contextSnapshotLimit=0;
    contextBindingFailures=contextBindingMismatches=0;contextReasons.fill(0);contextIoError=false;
    contextLog=std::fopen((contextFolder+"\\native_material_context.jsonl").c_str(),"wb");
    if(!contextLog){contextIoError=true;return;}
    LocalReader read;WosNativeMaterialCompat::Result gate;
    const bool identity=sizeof(void*)==4 && WosEngineProbe::enabled && WosEngineProbe::imageBase==0x00400000;
    contextEnabled=identity && WosNativeMaterialCompat::VerifyForCapture(read,gate);
    const uint32_t failedVA=gate.failedVA;
    uint32_t currentInputsFailedVA=0;
    contextCurrentInputGate=contextEnabled && WosStagedCurrentSun::VerifyCodes(read,currentInputsFailedVA);
    std::fprintf(contextLog,"{\"event\":\"begin\",\"version\":5,\"verified_engine\":%u,\"verified_routes\":%u,\"failed_va\":\"%08x\",\"draw_limit\":%u,\"snapshot_limit\":%u,\"raw_limit\":%u,\"source_phase_entry_limit\":%u,\"source_phase_group_limit\":%u,\"source_group_entry_limit\":%u,\"source_descriptor_count_limit\":%u,\"source_effect_descriptor_limit\":%u,\"current_inputs_version\":1,\"current_inputs_gate\":%u,\"current_inputs_failed_va\":\"%08x\",\"note\":\"Read-only inferred context. Draw ids join frame.tsv. Arena bounds and exact node type are checked; queue-list membership and original material semantics remain unproven.\"}\n",
        identity?1u:0u,contextEnabled?1u:0u,failedVA,ContextDrawLimit,ContextSnapshotLimit,ContextByteLimit,ContextSourceEntryLimit,ContextSourceGroupLimit,ContextSourceGroupEntryLimit,ContextSourceDescriptorLimit,ContextSourceEffectDescriptorLimit,contextCurrentInputGate?1u:0u,currentInputsFailedVA);
    std::fprintf(contextLog,"{\"event\":\"code_compatibility\",\"mode\":\"%s\",\"accepted\":%u,\"module_base\":\"%08x\",\"trampoline\":\"%08x\",\"module_sha256_verified\":%u,\"failed_va\":\"%08x\",\"reason\":\"%s\",\"note\":\"Known ex_WoS route may override pixel samplers 1/8, VS constants and render state after original binding. Actual-bound texture equality remains required for source use.\"}\n",
        WosNativeMaterialCompat::ModeName(gate.mode),gate.accepted?1u:0u,gate.moduleBase,gate.trampoline,gate.moduleHashVerified?1u:0u,gate.failedVA,gate.reason);
    if(contextEnabled) {
        contextRaw=std::fopen((contextFolder+"\\native_material_context.bin").c_str(),"wb");
        if(!contextRaw){contextEnabled=false;contextIoError=true;}
    }
}
// Dedup uses address, semantic kind and all bytes, so a state cache changed within the frame gets a new snapshot.
static unsigned SaveSnapshot(const char* kind,uint32_t address,const void* data,unsigned bytes) {
    if(!contextLog || !contextRaw || contextIoError || !bytes || bytes>128*0x2c)return 0;
    std::string key(kind);key.push_back('\0');key.append(reinterpret_cast<const char*>(&address),4);
    key.append(static_cast<const char*>(data),bytes);
    const auto it=contextObjects.find(key);if(it!=contextObjects.end())return it->second;
    if(contextSnapshots>=ContextSnapshotLimit || bytes>ContextByteLimit-contextRawBytes) {++contextSnapshotLimit;return 0;}
    const unsigned id=contextSnapshots+1;
    if(std::fwrite(data,1,bytes,contextRaw)!=bytes){contextIoError=true;return 0;}
    std::fprintf(contextLog,"{\"event\":\"snapshot\",\"id\":%u,\"kind\":\"%s\",\"address\":\"%08x\",\"offset\":%u,\"bytes\":%u}\n",
        id,kind,address,contextRawBytes,bytes);
    contextObjects.emplace(std::move(key),id);++contextSnapshots;contextRawBytes+=bytes;return id;
}
static unsigned SnapshotRead(const char* kind,uint32_t address,unsigned bytes,unsigned char* out=nullptr,bool byteAddress=false) {
    unsigned char data[128*0x2c]={};LocalReader read;
    if(bytes>sizeof(data) || !(byteAddress?address>=0x10000:Pointer(address)) || !read(address,data,bytes)) {++contextSnapshotFailures;return 0;}
    if(out)std::memcpy(out,data,bytes);return SaveSnapshot(kind,address,data,bytes);
}
struct DescriptorInfo {uint32_t address=0,object=0,vtable=0,method=0,baseMaterial=0,requestedPass=0,parameterCount=0,parameters=0,passTag=0,passName=0;unsigned snapshot=0,objectSnapshot=0,vtableSnapshot=0,codeSnapshot=0;};
static DescriptorInfo Descriptor(uint32_t address) {
    DescriptorInfo d;d.address=address;unsigned char bytes[0x40]={};
    d.snapshot=SnapshotRead("state",address,0x40,bytes);if(!d.snapshot)return d;
    d.passTag=Word(bytes,0x20);d.passName=Word(bytes,0x1c);d.parameterCount=Word(bytes,0x2c);d.parameters=Word(bytes,0x30);d.baseMaterial=Word(bytes,0x18);d.requestedPass=Word(bytes,0x28);d.object=Word(bytes,8);unsigned char object[4]={};
    d.objectSnapshot=SnapshotRead("submitter_object",d.object,4,object);if(!d.objectSnapshot)return d;
    d.vtable=Word(object);unsigned char vt[0x14]={};
    d.vtableSnapshot=SnapshotRead("submitter_vtable",d.vtable,0x14,vt);if(!d.vtableSnapshot)return d;
    d.method=Word(vt,0x10);
    // Bytes are evidence, never executable calls; readability alone does not prove a method's semantics.
    d.codeSnapshot=SnapshotRead("submitter_code",d.method,32,nullptr,true);return d;
}
struct ParameterEntryInfo {
    uint32_t address=0,hash=0,linked=0,value=0,declaredBytes=0,capturedBytes=0;
    unsigned snapshot=0,linkedSnapshot=0,valueSnapshot=0;
};
struct ParameterGroupInfo {
    uint32_t descriptor=0,pointer=0,count=0;unsigned firstDraw=0,captured=0,descriptorSnapshot=0;
    const char* status="not_attempted";ParameterEntryInfo entries[64]={};
};
static std::unordered_map<uint64_t,ParameterGroupInfo> contextParameterGroups;
static unsigned contextParameterEntries=0,contextShadowParameterGroups=0,contextOtherParameterGroups=0;
static void ClearParameterGroups() {
    contextParameterGroups.clear();contextParameterEntries=contextShadowParameterGroups=contextOtherParameterGroups=0;
}
static ParameterGroupInfo ReadParameters(const DescriptorInfo &d,bool shadow) {
    const uint64_t key=uint64_t(d.address)|(uint64_t(shadow?1:0)<<32);
    const auto old=contextParameterGroups.find(key);if(old!=contextParameterGroups.end())return old->second;
    ParameterGroupInfo g;g.descriptor=d.address;g.pointer=d.parameters;g.count=d.parameterCount;g.firstDraw=WosProbe::draws;g.descriptorSnapshot=d.snapshot;
    auto &groups=shadow?contextShadowParameterGroups:contextOtherParameterGroups;
    if(groups>=(shadow?64u:16u)){g.status="group_limit";return g;}
    if(d.method!=0x009dfe20){g.status="unsupported_submitter";return g;}
    ++groups;
    if(!g.count){g.status="empty";contextParameterGroups.emplace(key,g);return g;}
    if(g.count>64 || !Pointer(g.pointer) || uint64_t(g.pointer)+uint64_t(g.count)*0x2c>0x100000000ull) {
        g.status="invalid_count_or_pointer";contextParameterGroups.emplace(key,g);return g;
    }
    g.status="captured";
    for(unsigned i=0;i<g.count;++i) {
        if(contextParameterEntries>=512){g.status="entry_budget";break;}
        ++contextParameterEntries;auto &e=g.entries[g.captured++];e.address=g.pointer+i*0x2c;
        unsigned char entry[0x2c]={},linked[0x2c]={};
        e.snapshot=SnapshotRead("descriptor_parameter",e.address,sizeof(entry),entry);if(!e.snapshot)continue;
        e.hash=Word(entry,4);e.linked=Word(entry,0x14);e.value=Word(entry,0x1c);
        e.linkedSnapshot=SnapshotRead("linked_parameter",e.linked,sizeof(linked),linked);if(!e.linkedSnapshot)continue;
        e.declaredBytes=Word(linked,0x20);
        // Copy loop rounds linked destination size to 16. Capture only a bounded source prefix, never infer a texture.
        if(e.declaredBytes>0 && e.declaredBytes<=65536) {
            const unsigned rounded=(e.declaredBytes+15u)&~15u;e.capturedBytes=rounded<64?rounded:64;
            e.valueSnapshot=SnapshotRead("parameter_value_prefix",e.value,e.capturedBytes);
        }
    }
    contextParameterGroups.emplace(key,g);return g;
}
static void WriteParameters(const ParameterGroupInfo &g) {
    std::fprintf(contextLog,"\"descriptor_parameters\":{\"descriptor\":\"%08x\",\"pointer\":\"%08x\",\"count\":%u,\"first_draw\":%u,\"descriptor_snapshot\":%u,\"status\":\"%s\",\"captured_entries\":%u,\"entries\":[",
        g.descriptor,g.pointer,g.count,g.firstDraw,g.descriptorSnapshot,g.status,g.captured);
    for(unsigned i=0;i<g.captured;++i) {
        const auto &e=g.entries[i];
        std::fprintf(contextLog,"%s{\"address\":\"%08x\",\"snapshot\":%u,\"name_hash\":\"%08x\",\"linked\":\"%08x\",\"linked_snapshot\":%u,\"value_pointer\":\"%08x\",\"declared_bytes\":%u,\"captured_bytes\":%u,\"value_snapshot\":%u}",
            i?",":"",e.address,e.snapshot,e.hash,e.linked,e.linkedSnapshot,e.value,e.declaredBytes,e.capturedBytes,e.valueSnapshot);
    }
    std::fprintf(contextLog,"]}");
}
struct BaseMaterialInfo {
    uint32_t pointer=0,count=0,passes=0,requestedPass=0,parameterCount=0,parameters=0,valueBase=0;unsigned snapshot=0,captured=0;
    bool passBoundsValid=false;unsigned passSnapshots[64]={};uint32_t passHashes[64]={},subpassCounts[64]={},states[64]={};
};
static BaseMaterialInfo ReadBaseMaterial(const DescriptorInfo &descriptor) {
    BaseMaterialInfo b;b.pointer=descriptor.baseMaterial;b.requestedPass=descriptor.requestedPass;
    if(!b.pointer)return b;unsigned char bytes[0x48]={};
    b.snapshot=SnapshotRead("base_material",b.pointer,sizeof(bytes),bytes);if(!b.snapshot)return b;
    b.count=Word(bytes,0xc);b.passes=Word(bytes,0x10);b.parameterCount=Word(bytes,0x14);b.parameters=Word(bytes,0x18);b.valueBase=Word(bytes,0x20);
    b.passBoundsValid=b.count>0 && b.count<=64 && Pointer(b.passes) &&
        uint64_t(b.passes)+uint64_t(b.count)*0x1c<=0x100000000ull;
    if(b.passBoundsValid)for(unsigned i=0;i<b.count;++i) {
        unsigned char pass[0x1c]={};b.passSnapshots[i]=SnapshotRead("base_pass",b.passes+i*0x1c,0x1c,pass);
        if(b.passSnapshots[i]){b.passHashes[i]=Word(pass,8);b.subpassCounts[i]=Word(pass,0xc);b.states[i]=Word(pass,0x10);}
        if(b.passSnapshots[i])++b.captured;
    }return b;
}
struct OverrideSlot {
    uint32_t id=0,table=0,flagsAddress=0,flags=0,valueAddress=0,value=0;
    bool idRead=false,inRange=false,flagsRead=false,present=false,valueRead=false;
};
template<class Reader> static OverrideSlot ReadOverrideSlot(Reader &read,uint32_t idGlobal,uint32_t table) {
    OverrideSlot o;o.table=table;o.idRead=read(idGlobal,&o.id,4);
    o.inRange=o.idRead && o.id<64;
    if(!o.inRange || !Pointer(table) || uint64_t(table)+8+uint64_t(o.id)*4+4>0x100000000ull)return o;
    o.flagsAddress=table+(o.id>>5)*4;o.flagsRead=read(o.flagsAddress,&o.flags,4);
    if(!o.flagsRead)return o;
    o.present=(o.flags&(1u<<(o.id&31)))!=0;
    if(o.present){o.valueAddress=table+8+o.id*4;o.valueRead=read(o.valueAddress,&o.value,4);}
    return o;
}
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/native_material_sources.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/native_current_inputs.h"
static void WriteOverride(const char* key,const OverrideSlot &o) {
    std::fprintf(contextLog,"\"%s\":{\"id\":%u,\"id_read\":%s,\"id_in_range\":%s,\"table\":\"%08x\",\"flags_address\":\"%08x\",\"flags\":\"%08x\",\"flags_read\":%s,\"present\":%s,\"value_address\":\"%08x\",\"value\":\"%08x\",\"value_read\":%s}",
        key,o.id,o.idRead?"true":"false",o.inRange?"true":"false",o.table,o.flagsAddress,o.flags,
        o.flagsRead?"true":"false",o.present?"true":"false",o.valueAddress,o.value,o.valueRead?"true":"false");
}
static void WriteBaseMaterial(const BaseMaterialInfo &b,const Context &c) {
    std::fprintf(contextLog,"\"base_material\":{\"pointer\":\"%08x\",\"snapshot\":%u,\"base_pass_count\":%u,\"base_passes_pointer\":\"%08x\",\"requested_pass\":%u,\"pass_bounds_valid\":%s,\"captured_passes\":%u,\"equals_node_source\":%s,\"equals_active_material\":%s,\"base_passes\":[",
        b.pointer,b.snapshot,b.count,b.passes,b.requestedPass,b.passBoundsValid?"true":"false",b.captured,
        b.pointer==c.sourceMaterial?"true":"false",b.pointer==c.activeMaterial?"true":"false");
    if(b.passBoundsValid)for(unsigned i=0;i<b.count;++i)std::fprintf(contextLog,"%s{\"index\":%u,\"address\":\"%08x\",\"snapshot\":%u}",i?",":"",i,b.passes+i*0x1c,b.passSnapshots[i]);
    std::fprintf(contextLog,"]}");
}
static void WriteDescriptor(const char* name,const DescriptorInfo &d) {
    std::fprintf(contextLog,"\"%s\":{\"address\":\"%08x\",\"snapshot\":%u,\"object\":\"%08x\",\"object_snapshot\":%u,\"vtable\":\"%08x\",\"vtable_snapshot\":%u,\"method\":\"%08x\",\"code_snapshot\":%u}",
        name,d.address,d.snapshot,d.object,d.objectSnapshot,d.vtable,d.vtableSnapshot,d.method,d.codeSnapshot);
}
static uint32_t Ptr(const void* p) {return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));}
struct VertexDescriptorInfo {uint32_t address=0,stride=0,declaration=0;unsigned snapshot=0;};
static VertexDescriptorInfo ReadVertexDescriptor(const Context &c) {
    VertexDescriptorInfo v;v.address=Word(c.geometryBytes,0x4c);unsigned char bytes[12]={};
    // 0072c260 reads +0 for SetStreamSource stride and +8 for SetVertexDeclaration.
    v.snapshot=SnapshotRead("vertex_descriptor",v.address,sizeof(bytes),bytes);
    if(v.snapshot){v.stride=Word(bytes);v.declaration=Word(bytes,8);}return v;
}
static bool GeometryBindingsMatch(Kind kind,const Context &c,const VertexDescriptorInfo &v,unsigned valid,
    uint32_t vb,uint32_t ib,uint32_t offset,uint32_t stride,uint32_t declaration) {
    const unsigned required=kind==Kind::DIP?19u:17u;
    const uint32_t expectedOffset=kind==Kind::DIP || c.route==Route::PrimitiveQueue?Word(c.geometryBytes,0x30):0;
    return v.snapshot!=0 && (valid&required)==required && vb==Word(c.geometryBytes,0x2c) &&
        offset==expectedOffset && stride==v.stride && declaration==v.declaration &&
        (kind!=Kind::DIP || ib==Word(c.geometryBytes,0x3c));
}
static void WriteBindings(IDirect3DDevice9 *device,Kind kind,const Context &c,const VertexDescriptorInfo &vertex) {
    if(!device){++contextBindingFailures;std::fprintf(contextLog,"\"bindings\":{\"valid\":false}");return;}
    unsigned valid=0;IDirect3DVertexBuffer9 *vb=nullptr;IDirect3DIndexBuffer9 *ib=nullptr;
    UINT offset=0,stride=0;const HRESULT vh=device->GetStreamSource(0,&vb,&offset,&stride);
    const HRESULT ih=device->GetIndices(&ib);if(SUCCEEDED(vh))valid|=1;if(SUCCEEDED(ih))valid|=2;
    const unsigned vid=WosProbe::Id(vb),iid=WosProbe::Id(ib);const uint32_t vp=Ptr(vb),ip=Ptr(ib);
    IDirect3DPixelShader9 *ps=nullptr;IDirect3DVertexShader9 *vs=nullptr;
    if(SUCCEEDED(device->GetPixelShader(&ps)))valid|=4;
    if(SUCCEEDED(device->GetVertexShader(&vs)))valid|=8;
    const unsigned psid=WosProbe::Shader(ps,"ps"),vsid=WosProbe::Shader(vs,"vs");
    IDirect3DVertexDeclaration9 *decl=nullptr;
    if(SUCCEEDED(device->GetVertexDeclaration(&decl)))valid|=16;
    const unsigned did=WosProbe::Id(decl);
    const bool geometryMatch=GeometryBindingsMatch(kind,c,vertex,valid,vp,ip,offset,stride,Ptr(decl));
    if(valid!=31)++contextBindingFailures;if(!geometryMatch)++contextBindingMismatches;
    std::fprintf(contextLog,"\"bindings\":{\"valid_mask\":%u,\"geometry_match\":%s,\"vb\":%u,\"vb_pointer\":\"%08x\",\"offset\":%u,\"stride\":%u,\"ib\":%u,\"ib_pointer\":\"%08x\",\"declaration\":%u,\"declaration_pointer\":\"%08x\",\"vertex_descriptor\":{\"address\":\"%08x\",\"snapshot\":%u,\"expected_stride\":%u,\"expected_declaration\":\"%08x\"},\"native_ps\":%u,\"native_ps_pointer\":\"%08x\",\"native_vs\":%u,\"native_vs_pointer\":\"%08x\",\"textures\":[",
        valid,geometryMatch?"true":"false",vid,vp,offset,stride,iid,ip,did,Ptr(decl),vertex.address,vertex.snapshot,vertex.stride,vertex.declaration,psid,Ptr(ps),vsid,Ptr(vs));
    for(unsigned stage=0;stage<16;++stage) {
        IDirect3DBaseTexture9 *tex=nullptr;const HRESULT hr=device->GetTexture(stage,&tex);
        if(FAILED(hr))++contextBindingFailures;
        std::fprintf(contextLog,"%s{\"stage\":%u,\"id\":%u,\"pointer\":\"%08x\",\"hr\":\"%08lx\"}",
            stage?",":"",stage,WosProbe::Id(tex),Ptr(tex),static_cast<unsigned long>(hr));
        if(tex)tex->Release();
    }
    std::fprintf(contextLog,"]}");
    if(vb)vb->Release();if(ib)ib->Release();if(ps)ps->Release();if(vs)vs->Release();if(decl)decl->Release();
}
// Capture caller and returnSlot in the actual D3D hook with the MSVC intrinsics, before any shader DrawScope.
static void Observe(IDirect3DDevice9 *device,Kind kind,uintptr_t caller,uintptr_t returnSlot,const DrawArgs &args) {
    if(!WosProbe::trace || WosProbe::suppress)return;
    if(contextFolder!=WosProbe::folder)Begin();
    if(!contextLog || !contextEnabled || contextIoError)return;
    if(contextRows>=ContextDrawLimit || WosProbe::draws>=ContextDrawLimit){Finish("DRAW_LIMIT");return;}
    LocalReader read;const auto globals=WosEngineProbe::ReadGlobals();uint32_t arenaWords[3]={};Bounds arena;
    if(globals.valid!=7 || !read(0x01115c64,arenaWords,sizeof(arenaWords)) || !MakeArena(arenaWords,arena)) {
        ++contextGlobalRejects;++contextRows;
        std::fprintf(contextLog,"{\"event\":\"reject\",\"draw\":%u,\"reason\":\"globals_or_arena\"}\n",WosProbe::draws);return;
    }
    const auto tib=reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    Bounds stack={reinterpret_cast<uintptr_t>(tib->StackLimit),reinterpret_cast<uintptr_t>(tib->StackBase)};
    Context c;const Reason result=Decode(read,kind,static_cast<uint32_t>(caller),static_cast<uint32_t>(returnSlot),args,stack,arena,c);
    ++contextRows;++contextReasons[static_cast<unsigned>(result)];
    if(result!=Reason::Accepted) {
        std::fprintf(contextLog,"{\"event\":\"reject\",\"draw\":%u,\"kind\":\"%s\",\"reason\":\"%s\",\"caller\":\"%08x\",\"parent\":\"%08x\",\"grandparent\":\"%08x\",\"route\":\"%s\",\"node\":\"%08x\"}\n",
            WosProbe::draws,kind==Kind::DIP?"DIP":"DP",ReasonName(result),c.caller,c.parent,c.grandparent,RouteName(c.route),c.node);return;
    }
    const auto after=WosEngineProbe::ReadGlobals();uint32_t arenaAfter[3]={};
    if(!read(0x01115c64,arenaAfter,sizeof(arenaAfter)) || (globals.current!=after.current || globals.root!=after.root || globals.nearScene!=after.nearScene || globals.farScene!=after.farScene || globals.shadowActive!=after.shadowActive || globals.valid!=after.valid) || std::memcmp(arenaWords,arenaAfter,sizeof(arenaWords))) {
        ++contextChangedRejects;--contextReasons[0];
        std::fprintf(contextLog,"{\"event\":\"reject\",\"draw\":%u,\"reason\":\"metadata_changed\"}\n",WosProbe::draws);return;
    }
    const unsigned node=SaveSnapshot("node",c.node,c.nodeBytes,sizeof(c.nodeBytes));
    const unsigned instance=SaveSnapshot("instance",c.instance,c.instanceBytes,sizeof(c.instanceBytes));
    const unsigned geometry=SaveSnapshot("geometry",c.geometry,c.geometryBytes,sizeof(c.geometryBytes));
    const auto descriptor=Descriptor(c.descriptor);
    const auto state=c.state==c.descriptor?descriptor:Descriptor(c.state);
    const auto vertex=ReadVertexDescriptor(c);
    const auto baseMaterial=ReadBaseMaterial(descriptor);
    const uint32_t propertyTable=Word(c.instanceBytes,0x84);
    const auto materialOverride=ReadOverrideSlot(read,0x011171f4,propertyTable);
    const auto variantOverride=ReadOverrideSlot(read,0x011171fc,propertyTable);
    const auto parameters=ReadMaterialSources(descriptor,baseMaterial,globals.shadowActive!=0,materialOverride,variantOverride);
    auto currentInputs=ReadCurrentInputs(read,contextCurrentInputGate,WosProbe::draws,globals.current,c.instance,baseMaterial.pointer);
    SaveCurrentInputs(currentInputs);
    const unsigned active=SnapshotRead("active_material",c.activeMaterial,0x48);
    const unsigned source=c.sourceMaterial==c.activeMaterial?active:SnapshotRead("source_material",c.sourceMaterial,0x48);
    std::fprintf(contextLog,"{\"event\":\"draw\",\"draw\":%u,\"kind\":\"%s\",\"route\":\"%s\",\"caller\":\"%08x\",\"parent\":\"%08x\",\"grandparent\":\"%08x\",\"scene\":\"%08x\",\"root\":\"%08x\",\"near\":\"%08x\",\"far\":\"%08x\",\"shadow\":%u,\"arena\":{\"base\":\"%08x\",\"cursor\":\"%08x\"},\"node\":\"%08x\",\"node_snapshot\":%u,\"instance\":\"%08x\",\"instance_snapshot\":%u,\"geometry\":\"%08x\",\"geometry_snapshot\":%u,\"dispatch_record\":\"%08x\",\"active_material\":\"%08x\",\"active_material_snapshot\":%u,\"source_material\":\"%08x\",\"source_material_snapshot\":%u,\"pass\":%u,\"descriptor_equals_node_state\":%s,\"wrapper_matched\":%s,\"draw_args_matched\":%s,\"args\":{\"type\":%u,\"base\":%d,\"min_vertex\":%u,\"vertices\":%u,\"start\":%u,\"primitives\":%u},",
        WosProbe::draws,kind==Kind::DIP?"DIP":"DP",RouteName(c.route),c.caller,c.parent,c.grandparent,
        globals.current,globals.root,globals.nearScene,globals.farScene,globals.shadowActive,arenaWords[0],arenaWords[2],
        c.node,node,c.instance,instance,c.geometry,geometry,c.record,c.activeMaterial,active,c.sourceMaterial,source,c.pass,
        c.descriptor==c.state?"true":"false",c.wrapperMatched?"true":"false",c.drawArgsMatched?"true":"false",
        args.type,args.base,args.minVertex,args.vertices,args.start,args.primitives);
    WriteDescriptor("descriptor",descriptor);std::fprintf(contextLog,",");WriteDescriptor("node_state",state);std::fprintf(contextLog,",");
    WriteBaseMaterial(baseMaterial,c);std::fprintf(contextLog,",");
    WriteMaterialSources(parameters);std::fprintf(contextLog,",");
    WriteCurrentInputs(currentInputs);std::fprintf(contextLog,",");
    std::fprintf(contextLog,"\"override_evidence\":{\"base_vs_effective\":\"observed_pointers_only\",");
    WriteOverride("material",materialOverride);std::fprintf(contextLog,",");WriteOverride("variant",variantOverride);std::fprintf(contextLog,"},");
    WriteBindings(device,kind,c,vertex);std::fprintf(contextLog,"}\n");
    if(std::ferror(contextLog)){contextIoError=true;contextEnabled=false;}
}
}
