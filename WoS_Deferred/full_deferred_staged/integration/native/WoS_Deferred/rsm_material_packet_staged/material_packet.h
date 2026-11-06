#pragma once
// Staged CPU-only current-draw diffuse packet. See CONTRACT.md for host gates,
// native object lifetime, current baseline sampler state and expiry requirements.
#include <cstdint>
#include <cstring>
#include <cmath>
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_Deferred/rsm_material_packet_staged/sampler_state_decode.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/current_sun_decode.h"
namespace WosStagedMaterialPacket {
enum class Family { Old69, Old72, Static111 };
enum class Status { Accepted, Context, Read, Pointer, UnsupportedSubmitter, Pass, ShaderIdentity,
    Definition, Source, Binding, Override, Sampler, Baseline, CurrentSun, NoSun, NonFinite, Changed };
struct Scope {
    uint32_t draw=0,scene=0,frame=0,instance=0,descriptor=0;
    bool nativeContextVerified=false,sourceCodeVerified=false,sunCodeVerified=false,samplerCodeVerified=false;
};
struct Diagnostic {Status status=Status::Context;uint32_t failedAddress=0,failedBytes=0;};
struct Packet {
    bool valid=false;Family family=Family::Old69;Scope scope;
    uint32_t effect=0,passIndex=0,state=0,pair=0,pixelShader=0,vertexShader=0;
    uint32_t diffuseStage=0,nglTexture=0,d3dTexture=0,sourceTextureValue=0;
    bool samplerFromSource=false;uint32_t samplerFXValuePointer=0;
    WosStagedSamplerState::State nativeSampler;uint32_t baselineSampler[14]={},sampler[14]={},baselineMask=0;
    float diffuseMultiplier=1,uvScale[2]={1,1},materialColor[4]={1,1,1,1},diffuseU[4]={},diffuseV[4]={},animationTime=0;
    WosStagedCurrentSun::Inputs current;
};
inline bool SameScope(const Packet& p,const Scope& s) {
    return p.valid && s.nativeContextVerified && s.sourceCodeVerified && s.sunCodeVerified && s.samplerCodeVerified &&
        p.scope.draw==s.draw && p.scope.scene==s.scene && p.scope.frame==s.frame && p.scope.instance==s.instance && p.scope.descriptor==s.descriptor;
}
inline uint32_t Word(const void* p,unsigned offset=0){uint32_t v;std::memcpy(&v,static_cast<const unsigned char*>(p)+offset,4);return v;}
inline bool Pointer(uint32_t p,uint32_t n=4){return p>=0x10000 && !(p&3) && uint64_t(p)+n<=0x100000000ull;}
inline bool Finite(const float* p,unsigned n){for(unsigned i=0;i<n;++i)if(!std::isfinite(p[i]))return false;return true;}
constexpr const char* PS69="c044b69cc2da2d4a7ab5e7e1859564bbeb46704ca8d5686beb384ca5f3aa9b39";
constexpr const char* VS70="f4a1930ac0c3f58e3985186f59ed40bb40fa9356b5ba79f8a963038df86e5b5f";
constexpr const char* PS72="39efbb21be645360babdb5176ede2f1a7583d820e68eb7ecbd2b23649979b565";
constexpr const char* VS73="28c8b4c5601a53c17c56015450d59037d21738117c7a1714d087d2c78806e218";
constexpr const char* PS111="9801c80c514f4d156e7c1faa7bbdaaadac4eb54f9cf1f6feb49f24c7bb1050b0";
constexpr const char* VS112="ed6e2eb93618ff36c1a25178c6203a0795c9de93d2eeb40c01fc75cb23844a34";
template<class Reader> struct CheckedReader {
    Reader& read;Diagnostic& diagnostic;
    bool operator()(uint32_t p,void* out,uint32_t n) {
        if(p<0x10000 || uint64_t(p)+n>0x100000000ull || !read(p,out,n)) {
            diagnostic.failedAddress=p;diagnostic.failedBytes=n;return false;
        }return true;
    }
};
struct Table {uint32_t pointer=0,count=0;unsigned char data[128*44]={};};
struct Bindings {uint32_t pointer=0,count=0;unsigned char data[128*24]={};};
inline int UniqueHash(const Table& t,uint32_t hash){int found=-1;for(unsigned i=0;i<t.count;++i)if(Word(t.data+i*44,4)==hash){if(found!=-1)return -1;found=int(i);}return found;}
inline bool FloatBinding(const Bindings& b,unsigned fxIndex,unsigned reg,uint32_t valueBase,uint32_t value,unsigned bytesOffset=0) {
    if(uint64_t(value)+bytesOffset>0xffffffffull || value+bytesOffset<valueBase)return false;
    const uint32_t offset=value+bytesOffset-valueBase;unsigned matches=0,writers=0;
    for(unsigned i=0;i<b.count;++i){const auto* r=b.data+i*24;
        if(Word(r,8)==0 && Word(r,20)>0 && uint64_t(Word(r,4))+Word(r,20)>reg && Word(r,4)<=reg)++writers;
        if(Word(r)==fxIndex && Word(r,4)==reg && Word(r,8)==0 && Word(r,12)==1 && Word(r,16)==offset && Word(r,20)==1)++matches;
    }return matches==1 && writers==1;
}
template<class Read> Status ReadTable(Read& read,uint32_t pointer,uint32_t count,Table& t) {
    if(!count || count>128 || !Pointer(pointer,count*44))return Status::Definition;
    t.pointer=pointer;t.count=count;return read(pointer,t.data,count*44)?Status::Accepted:Status::Read;
}
template<class Read> Status ReadBindings(Read& read,const unsigned char* descriptor,Bindings& b) {
    b.count=Word(descriptor,8);b.pointer=Word(descriptor,12);
    if(!b.count || b.count>128 || !Pointer(b.pointer,b.count*24))return Status::Binding;
    return read(b.pointer,b.data,b.count*24)?Status::Accepted:Status::Read;
}
template<class Read> Status CheckOverrides(Read& read,uint32_t instance) {
    uint32_t table=0;if(!read(instance+0x84,&table,4))return Status::Read;
    if(!Pointer(table,264))return Status::Pointer;
    for(unsigned k=0;k<2;++k){uint32_t id=0,flags=0,value=0;
        if(!read(k?0x011171fc:0x011171f4,&id,4))return Status::Read;
        if(id>=64)return Status::Override;
        if(!read(table+4*(id>>5),&flags,4))return Status::Read;
        if(flags&(1u<<(id&31))){
            if(!read(table+8+4*id,&value,4))return Status::Read;
            if(k || value)return Status::Override;
        }
    }return Status::Accepted;
}
template<class Read> Status SelectPass(Read& read,const unsigned char* descriptor,const unsigned char* effect,uint32_t& index,unsigned char (&pass)[28]) {
    // 009dfbd0 selects the original render pass from signed descriptor+0x28.
    // +0x1c/+0x20 belong to a different parameter-link lookup and are ignored.
    // Variant overrides can change this selection; CheckOverrides rejects them.
    const uint32_t count=Word(effect,12),base=Word(effect,16);
    const int32_t requested=static_cast<int32_t>(Word(descriptor,40));
    if(!count || count>64 || !Pointer(base,count*28) || requested<0)return Status::Pass;
    index=static_cast<uint32_t>(requested)>=count?count-1:static_cast<uint32_t>(requested);
    return read(base+index*28,pass,28)?Status::Accepted:Status::Read;
}
template<class Read> Status Scalar(Read& read,const Table& source,const Table& fx,const Bindings* bindings,uint32_t valueBase,
    uint32_t hash,unsigned type,unsigned bytes,unsigned reg,float* out) {
    const int s=UniqueHash(source,hash),f=UniqueHash(fx,hash);if(s<0 || f<0)return Status::Source;
    const auto* a=source.data+s*44;const auto* b=fx.data+f*44;
    if(Word(a,24)!=type || Word(b,24)!=type || Word(a,32)!=bytes || Word(b,32)!=bytes || !Pointer(Word(a,28),bytes) || !Pointer(Word(b,28),bytes))return Status::Source;
    if(bindings && !FloatBinding(*bindings,f,reg,valueBase,Word(b,28)))return Status::Binding;
    if(!read(Word(a,28),out,bytes))return Status::Read;
    return Finite(out,bytes/4)?Status::Accepted:Status::NonFinite;
}
inline bool Semantic(const Table& fx,const Bindings& bindings,uint32_t valueBase,uint32_t hash,unsigned type,unsigned bytes,unsigned reg,unsigned offset=0) {
    const int i=UniqueHash(fx,hash);if(i<0)return false;const auto* e=fx.data+i*44;
    return Word(e,24)==type && Word(e,32)==bytes && Pointer(Word(e,28),bytes) && FloatBinding(bindings,i,reg,valueBase,Word(e,28),offset);
}
template<class Read,class Baseline> Status Texture(Read& read,Baseline& baseline,const Scope& scope,const Table& source,const Table& fx,
    const Bindings& ps,uint32_t valueBase,uint32_t hash,unsigned stage,Packet& packet) {
    const int s=UniqueHash(source,hash),f=UniqueHash(fx,hash);if(s<0 || f<0)return Status::Source;
    const auto* a=source.data+s*44;const auto* texture=fx.data+f*44;
    if(Word(a,24)!=11 || Word(texture,24)!=11 || Word(a,32)!=28 || Word(texture,32)!=28 || !Pointer(Word(a,28),28) || !Pointer(Word(texture,28),28))return Status::Source;
    const uint32_t value=Word(texture,28);unsigned aliases=0;
    for(unsigned i=0;i<fx.count;++i){const auto* d=fx.data+i*44;if(Word(d,24)==11 && Word(d,32)==28 && Word(d,28)==value)++aliases;}
    if(aliases!=1)return Status::Binding;
    unsigned match=0,stateIndex=0,writers=0;
    for(unsigned i=0;i<ps.count;++i){const auto* r=ps.data+i*24;
        if(Word(r,4)==stage && Word(r,8)==3)++writers;
        if(Word(r,4)==stage && Word(r,8)==3 && Word(r,12)==1 && Word(r,20)==1 && uint64_t(valueBase)+Word(r,16)==value){++match;stateIndex=Word(r);}}
    if(match!=1 || writers!=1 || stateIndex>=fx.count)return Status::Binding;
    const auto* state=fx.data+stateIndex*44;if(Word(state,24)!=14 || Word(state,32)!=40)return Status::Binding;
    unsigned char bytes[28]={},ngl[24]={};packet.sourceTextureValue=Word(a,28);
    if(!read(packet.sourceTextureValue,bytes,28))return Status::Read;packet.nglTexture=Word(bytes);
    if(!Pointer(packet.nglTexture,24))return Status::Pointer;
    if(!read(packet.nglTexture,ngl,24))return Status::Read;packet.d3dTexture=Word(ngl,20);
    if(!Pointer(packet.d3dTexture))return Status::Pointer;
    packet.samplerFXValuePointer=Word(state,28);
    // 009eaa50 can populate the sampler-state FX value from an own source.
    // Prefer that exact current source payload when its hash is present.
    int stateSource=-1;unsigned stateSources=0;
    for(unsigned i=0;i<source.count;++i)if(Word(source.data+i*44,4)==Word(state,4)){stateSource=int(i);++stateSources;}
    if(stateSources>1)return Status::Source;
    if(stateSource>=0){
        const auto* own=source.data+stateSource*44;
        if(Word(own,24)!=14 || Word(own,32)!=40 || !Pointer(Word(own,28),40))return Status::Source;
        uint32_t raw[10]={},after[10]={};unsigned char ownAfter[44]={},fxAfter[44]={};
        if(!read(Word(own,28),raw,40))return Status::Read;
        packet.nativeSampler.definition=fx.pointer+stateIndex*44;
        packet.nativeSampler.valuePointer=Word(own,28);packet.nativeSampler.nameHash=Word(state,4);
        if(!WosStagedSamplerState::Convert(raw,packet.nativeSampler))return Status::Sampler;
        if(!read(source.pointer+stateSource*44,ownAfter,44) || !read(Word(own,28),after,40) ||
           !read(fx.pointer+stateIndex*44,fxAfter,44))return Status::Read;
        if(std::memcmp(own,ownAfter,44) || std::memcmp(raw,after,40) || std::memcmp(state,fxAfter,44))return Status::Changed;
        packet.samplerFromSource=true;
    }else{
        const auto sr=WosStagedSamplerState::Decode(read,fx.pointer+stateIndex*44,packet.nativeSampler);
        if(sr!=WosStagedSamplerState::Status::Accepted)return sr==WosStagedSamplerState::Status::Read?Status::Read:Status::Sampler;
        if(packet.nativeSampler.valuePointer!=Word(state,28) || packet.nativeSampler.nameHash!=Word(state,4))return Status::Changed;
    }
    if(!baseline(scope,stage,packet.baselineSampler,packet.baselineMask) ||
        !WosStagedSamplerState::MergeCurrentBaseline(packet.nativeSampler,packet.baselineSampler,packet.baselineMask,packet.sampler))return Status::Baseline;
    unsigned char after[28]={},nglAfter[24]={};
    if(!read(packet.sourceTextureValue,after,28) || !read(packet.nglTexture,nglAfter,24))return Status::Read;
    if(std::memcmp(bytes,after,28) || std::memcmp(ngl,nglAfter,24))return Status::Changed;
    packet.diffuseStage=stage;return Status::Accepted;
}
// Identity callback verifies full bytecode SHA-256 for the supplied original object.
// Baseline callback returns all13 states from the exact current Scope and stage.
template<class Reader,class ShaderIdentity,class Baseline> Status Decode(Reader& reader,ShaderIdentity& identity,Baseline& baseline,
    const Scope& scope,Packet& output,Diagnostic& diagnostic) {
    diagnostic=Diagnostic{};output=Packet{};Packet p;p.scope=scope;CheckedReader<Reader> read{reader,diagnostic};
    auto fail=[&](Status s){diagnostic.status=s;return s;};
    if(!scope.nativeContextVerified || !scope.sourceCodeVerified || !scope.sunCodeVerified || !scope.samplerCodeVerified ||
        !Pointer(scope.scene,0x4dc) || !Pointer(scope.instance,0x88) || !Pointer(scope.descriptor,0x40))return fail(Status::Context);
    uint32_t scene=0,frame=0;if(!read(0x01115c54,&scene,4) || !read(0x011184f4,&frame,4))return fail(Status::Read);
    if(scene!=scope.scene || frame!=scope.frame)return fail(Status::Context);
    auto result=CheckOverrides(read,scope.instance);if(result!=Status::Accepted)return fail(result);
    unsigned char descriptor[64]={},effect[72]={};uint32_t object=0,vtable=0,method=0;
    if(!read(scope.descriptor,descriptor,64))return fail(Status::Read);
    object=Word(descriptor,8);if(!Pointer(object) || !read(object,&vtable,4) || !Pointer(vtable,20) || !read(vtable+16,&method,4))return fail(Status::UnsupportedSubmitter);
    if(method!=0x009dfe20)return fail(Status::UnsupportedSubmitter);
    p.effect=Word(descriptor,24);if(!Pointer(p.effect,72))return fail(Status::Pointer);
    if(!read(p.effect,effect,72))return fail(Status::Read);if(!(Word(effect)&0x40))return fail(Status::CurrentSun);
    unsigned char pass[28]={};result=SelectPass(read,descriptor,effect,p.passIndex,pass);if(result!=Status::Accepted)return fail(result);
    // Multiple source subpasses need an explicit choice; never silently select one.
    if(Word(pass,12)!=1 || !Pointer(Word(pass,16),0xec))return fail(Status::Pass);p.state=Word(pass,16);
    if(!read(p.state+0xdc,&p.pair,4))return fail(Status::Read);if(!Pointer(p.pair,48))return fail(Status::Pointer);
    // Native 009ea950 passes pair+0 to 009ea5b0 (VS), pair+0x20 to 009ea730 (PS).
    // Each descriptor stores COM shader/count/records at +4/+8/+12.
    unsigned char pair[48]={};if(!read(p.pair,pair,48))return fail(Status::Read);p.vertexShader=Word(pair,4);p.pixelShader=Word(pair,36);
    if(!Pointer(p.vertexShader) || !Pointer(p.pixelShader))return fail(Status::ShaderIdentity);
    if(identity(p.pixelShader,true,PS69) && identity(p.vertexShader,false,VS70))p.family=Family::Old69;
    else if(identity(p.pixelShader,true,PS72) && identity(p.vertexShader,false,VS73))p.family=Family::Old72;
    else if(identity(p.pixelShader,true,PS111) && identity(p.vertexShader,false,VS112))p.family=Family::Static111;
    else return fail(Status::ShaderIdentity);
    Table own,fx;Bindings ps,vs;
    result=ReadTable(read,Word(descriptor,48),Word(descriptor,44),own);if(result!=Status::Accepted)return fail(result);
    result=ReadTable(read,Word(effect,24),Word(effect,20),fx);if(result!=Status::Accepted)return fail(result);
    const uint32_t valueBase=Word(effect,32);if(!Pointer(valueBase))return fail(Status::Pointer);
    result=ReadBindings(read,pair+32,ps);if(result!=Status::Accepted)return fail(result);
    result=ReadBindings(read,pair,vs);if(result!=Status::Accepted)return fail(result);
    const bool old69=p.family==Family::Old69,static111=p.family==Family::Static111;
    if(!Semantic(fx,ps,valueBase,0x8d431650,0x53,32,old69?19:static111?25:30,16) ||
       !Semantic(fx,ps,valueBase,0xea531150,0x54,32,old69?20:static111?26:31,16) ||
       !Semantic(fx,old69?vs:ps,valueBase,0x78fe9f05,0x70,16,old69?156:static111?24:29))return fail(Status::Binding);
    result=Texture(read,baseline,scope,own,fx,ps,valueBase,old69?0x8f6691d7:0x4046ddfb,old69?3:0,p);if(result!=Status::Accepted)return fail(result);
    if(old69){
        result=Scalar(read,own,fx,&ps,valueBase,0x30babe2c,3,4,33,&p.diffuseMultiplier);if(result!=Status::Accepted)return fail(result);
        result=Scalar(read,own,fx,&ps,valueBase,0x08869be3,3,4,36,p.uvScale);if(result!=Status::Accepted)return fail(result);
        result=Scalar(read,own,fx,&ps,valueBase,0x0898b464,3,4,37,p.uvScale+1);if(result!=Status::Accepted)return fail(result);
    }else{
        result=Scalar(read,own,fx,&ps,valueBase,0x11ddea9c,4,16,static111?19:21,p.materialColor);if(result!=Status::Accepted)return fail(result);
        result=Scalar(read,own,fx,&vs,valueBase,0xfa86828c,4,16,static111?17:157,p.diffuseU);if(result!=Status::Accepted)return fail(result);
        result=Scalar(read,own,fx,&vs,valueBase,0xfa86828d,4,16,static111?18:158,p.diffuseV);if(result!=Status::Accepted)return fail(result);
        if(!Semantic(fx,vs,valueBase,0x955b4634,0x19,4,static111?16:156))return fail(Status::Binding);
        if(!read(scope.scene+0x4cc,&p.animationTime,4))return fail(Status::Read);if(!std::isfinite(p.animationTime))return fail(Status::NonFinite);
    }
    const auto sun=WosStagedCurrentSun::Decode(read,scope.scene,scope.instance,Word(effect),p.current);
    if(sun!=WosStagedCurrentSun::Status::Accepted)return fail(sun==WosStagedCurrentSun::Status::Read?Status::Read:Status::CurrentSun);
    if(!p.current.sunPresent)return fail(Status::NoSun);
    uint32_t sceneAfter=0,frameAfter=0;unsigned char descAfter[64]={},effectAfter[72]={},pairAfter[48]={};
    if(!read(0x01115c54,&sceneAfter,4) || !read(0x011184f4,&frameAfter,4) || !read(scope.descriptor,descAfter,64) ||
       !read(p.effect,effectAfter,72) || !read(p.pair,pairAfter,48))return fail(Status::Read);
    if(sceneAfter!=scope.scene || frameAfter!=scope.frame || std::memcmp(descriptor,descAfter,64) || std::memcmp(effect,effectAfter,72) || std::memcmp(pair,pairAfter,48))return fail(Status::Changed);
    p.valid=true;output=p;return fail(Status::Accepted);
}
}
