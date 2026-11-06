#pragma once
// Staged current native shadow draw -> exact original material -> MRT bind plan.
// F7 is not consulted. No engine calls, replay, file output or shader/RT mutation.
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_Deferred/rsm_material_packet_staged/d3d9_packet_adapter.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_Deferred/rsm_vertex_bridge_staged/current_draw_context.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/native_material_context_compat.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/current_sun_gates.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_Deferred/rsm_material_packet_staged/sampler_state_gates.h"
namespace WosStagedCurrentPacket {
using WosNativeMaterialContext::Kind;
using WosNativeMaterialContext::DrawArgs;
using WosNativeMaterialContext::Bounds;
using WosStagedPacketD3D9::Registry;
using WosStagedPacketD3D9::Reader;
using WosStagedMaterialPacket::Scope;
struct GateReport {
    bool accepted=false,identityVerified=false,sourceVerified=false,sunVerified=false,samplerVerified=false;
    uint32_t failedVA=0;uint64_t hostFrame=0;unsigned reads=0,bytes=0;
    const char* reason="not initialized";
    WosNativeMaterialCompat::Result compatibility;
};
// Full loaded-byte gate is exposed for pure CPU fixtures. Identity-file proof is
// supplied separately only by Gates.Initialize, never inferred from code bytes.
template<class Read> bool VerifyLoaded(Read& read,const WosNativeMaterialCompat::Result& identity,GateReport& report){
    report.accepted=false;report.sourceVerified=report.sunVerified=report.samplerVerified=false;report.failedVA=0;
    if(!identity.accepted){report.reason="identity unavailable";return false;}
    unsigned char code[32]={};
    for(const auto& signature:WosEngineProbe::signatures){if(!read(0x400000+signature.rva,code,32)||std::memcmp(code,signature.bytes,32)){
        report.failedVA=0x400000+signature.rva;report.reason="engine loaded code mismatch";return false;}}
    WosNativeMaterialCompat::Result route;
    if(identity.mode==WosNativeMaterialCompat::Mode::ExWoS){
        if(!identity.moduleHashVerified || !WosNativeMaterialCompat::VerifyKnownCodeRoute(read,identity.moduleBase,route)){
            report.failedVA=route.failedVA;report.reason=identity.moduleHashVerified?route.reason:"module identity unavailable";return false;}
        route.moduleHashVerified=true;
    }else if(identity.mode==WosNativeMaterialCompat::Mode::Original){
        if(!WosNativeMaterialContext::VerifyCodeRanges(read,report.failedVA)){report.reason="native loaded route mismatch";return false;}
        route=identity;
    }else{report.reason="unsupported compatibility mode";return false;}
    report.compatibility=route;report.sourceVerified=true;
    if(!WosStagedCurrentSun::VerifyCodes(read,report.failedVA)){report.reason="current Sun loaded code mismatch";return false;}
    report.sunVerified=true;
    if(!WosStagedSamplerState::VerifyCodes(read,report.failedVA)){report.reason="sampler loaded code mismatch";return false;}
    report.samplerVerified=true;report.accepted=true;report.reason="accepted exact loaded routes";return true;
}
class Gates {
    bool identityReady_=false;HMODULE pinnedModule_=nullptr;WosNativeMaterialCompat::Result identity_;
public:
    GateReport report;
    Gates()=default;Gates(const Gates&)=delete;Gates&operator=(const Gates&)=delete;
    ~Gates(){if(pinnedModule_)FreeLibrary(pinnedModule_);}
    // Device Reset expires frames but retains process identity and the reviewed
    // module reference. No recurring EXE/mod file hashing on ordinary resets/F5.
    void Reset(){report=GateReport{};}
    bool Initialize(){
        Reset();
        if(!identityReady_){
            if(sizeof(void*)!=4 || reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))!=0x400000){report.reason="unsupported image base or architecture";return false;}
            wchar_t path[32768]={};const DWORD n=GetModuleFileNameW(nullptr,path,32768);std::string sha;
            if(!n || n>=32768 || !WosEngineProbe::ExeIdentitySha(path,sha) || sha!=WosEngineProbe::expectedSha){report.reason="running EXE file identity mismatch";return false;}
            Reader read;
            if(!WosNativeMaterialCompat::VerifyForCapture(read,identity_)){report.compatibility=identity_;report.reason=identity_.reason;report.failedVA=identity_.failedVA;return false;}
            if(identity_.mode==WosNativeMaterialCompat::Mode::ExWoS && !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(uintptr_t(identity_.moduleBase)),&pinnedModule_)){report.reason="cannot retain reviewed module";return false;}
            identityReady_=true;
        }
        report.identityVerified=true;report.compatibility=identity_;report.reason="identity ready; current frame not verified";return true;
    }
    bool BeginFrame(uint64_t hostFrame){
        if(!identityReady_ || !hostFrame){report.accepted=false;report.reason="identity or host frame unavailable";return false;}
        if(report.hostFrame==hostFrame)return report.accepted;
        report=GateReport{};report.hostFrame=hostFrame;report.identityVerified=true;Reader read;
        const bool ok=VerifyLoaded(read,identity_,report);report.reads=read.calls;report.bytes=read.bytes;return ok;
    }
    bool ValidFor(uint64_t hostFrame)const{return identityReady_ && report.identityVerified && report.accepted && hostFrame && report.hostFrame==hostFrame;}
};
// All returned D3D interfaces came from current Get* calls. Keep these references
// until the enclosing original draw completes; pointer snapshots alone are not ownership.
struct GeometryRefs {
    IDirect3DVertexBuffer9* vb=nullptr;IDirect3DIndexBuffer9* ib=nullptr;IDirect3DVertexDeclaration9* declaration=nullptr;
    GeometryRefs()=default;GeometryRefs(const GeometryRefs&)=delete;GeometryRefs&operator=(const GeometryRefs&)=delete;
    ~GeometryRefs(){Reset();}
    void Reset(){if(vb)vb->Release();if(ib)ib->Release();if(declaration)declaration->Release();vb=nullptr;ib=nullptr;declaration=nullptr;}
    bool Read(IDirect3DDevice9* device,Kind kind,WosCurrentDraw::Bindings& out){
        Reset();out={};if(!device)return false;UINT offset=0,stride=0;
        if(FAILED(device->GetStreamSource(0,&vb,&offset,&stride)) || !vb ||
            FAILED(device->GetVertexDeclaration(&declaration)) || !declaration ||
            (kind==Kind::DIP && (FAILED(device->GetIndices(&ib)) || !ib))){Reset();return false;}
        out.vb=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(vb));out.ib=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ib));
        out.declaration=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(declaration));out.offset=offset;out.stride=stride;out.complete=true;return true;
    }
};
template<class Read> bool ReadScene(Read& read,WosCurrentDraw::Scene& scene){
    scene={};uint32_t pair[2]={};unsigned char active=0;
    if(!read(0x01115c54,pair,8))return false;scene.current=pair[0];scene.valid|=1;
    if(!read(0x01036e98,pair,8))return false;scene.nearScene=pair[0];scene.farScene=pair[1];scene.valid|=2;
    if(!read(0x01073dfa,&active,1))return false;scene.shadowActive=active;scene.valid|=4;
    return read(0x011184f4,&scene.epoch,4) && read(0x01115c64,scene.arena,12);
}
struct Prepared {
    WosStagedPacketD3D9::BindPlan plan;Scope scope;WosCurrentDraw::Result context;GeometryRefs geometry;
    WosStagedMaterialPacket::Diagnostic diagnostic;
    const char* reason="inactive";unsigned readerCalls=0,readerBytes=0;
    Prepared()=default;Prepared(const Prepared&)=delete;Prepared&operator=(const Prepared&)=delete;
    void Reset(){plan.Reset();scope=Scope{};context=WosCurrentDraw::Result{};geometry.Reset();diagnostic={};reason="inactive";readerCalls=readerBytes=0;}
    bool Ready()const{return plan.ValidFor(scope) && context.valid;}
};
// Pure glue seam for fixtures; live Build below supplies exact Gates and actual
// scene/geometry callbacks. Callback-produced bindings must retain COM refs.
template<class Read,class SceneReader,class BindingReader,class Identity,class Baseline>
bool DecodeCurrent(Read& read,SceneReader& sceneReader,BindingReader& bindingReader,Identity& identity,Baseline& baseline,
    bool verified,Kind kind,uint32_t caller,uint32_t returnSlot,const DrawArgs& args,Bounds stack,
    uint64_t hostFrame,uint64_t drawSerial,WosCurrentDraw::Result& context,Scope& scope,WosStagedMaterialPacket::Packet& packet,
    WosStagedMaterialPacket::Diagnostic& diagnostic,const char*& reason){
    scope={};packet={};context={};diagnostic={};reason="inactive";
    if(!verified || !hostFrame || drawSerial>0xffffffffull){reason="gate or counter unavailable";return false;}
    context=WosCurrentDraw::Resolve(read,sceneReader,bindingReader,true,true,kind,caller,returnSlot,args,stack,hostFrame,drawSerial);
    if(!context.valid){reason=context.reason;return false;}
    scope={static_cast<uint32_t>(drawSerial),context.scene.current,context.scene.epoch,context.native.instance,context.native.descriptor,true,true,true,true};
    const auto status=WosStagedMaterialPacket::Decode(read,identity,baseline,scope,packet,diagnostic);
    if(status!=WosStagedMaterialPacket::Status::Accepted){reason="material packet rejected";return false;}
    WosCurrentDraw::Scene after;
    if(!sceneReader(after) || !WosCurrentDraw::SameScene(context.scene,after)){packet.valid=false;reason="scene changed after material packet";return false;}
    reason="accepted current draw packet";return true;
}
inline bool Build(IDirect3DDevice9* device,Registry& registry,const Gates& gates,Kind kind,uint32_t caller,uint32_t returnSlot,
    const DrawArgs& args,Bounds stack,uint64_t hostFrame,uint64_t drawSerial,Prepared& out){
    out.Reset();if(!device || device!=registry.Device() || !gates.ValidFor(hostFrame)){out.reason="device or loaded gates unavailable";return false;}
    Reader read;
    auto scenes=[&](WosCurrentDraw::Scene& scene){return ReadScene(read,scene);};
    auto bindings=[&](Kind drawKind,WosCurrentDraw::Bindings& value){return out.geometry.Read(device,drawKind,value);};
    // Scope is established by DecodeCurrent before it invokes this callback.
    auto baseline=[&](const Scope& current,unsigned stage,uint32_t (&values)[14],uint32_t& mask){
        WosStagedPacketD3D9::Baseline live{device,out.scope};return live(current,stage,values,mask);};
    WosStagedMaterialPacket::Packet packet;
    const bool ok=DecodeCurrent(read,scenes,bindings,registry,baseline,true,kind,caller,returnSlot,args,stack,hostFrame,drawSerial,
        out.context,out.scope,packet,out.diagnostic,out.reason);
    out.readerCalls=read.calls;out.readerBytes=read.bytes;
    if(!ok){out.geometry.Reset();return false;}
    if(!WosStagedPacketD3D9::Prepare(out.plan,packet,out.scope,registry)){out.reason="diffuse texture or bind plan unavailable";out.geometry.Reset();return false;}
    out.reason="accepted current draw bind plan";return true;
}
}
