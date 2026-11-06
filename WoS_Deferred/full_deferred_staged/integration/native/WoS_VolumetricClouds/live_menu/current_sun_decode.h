#pragma once
// Staged read-only decoder. Caller must verify the reviewed EXE/code and exact draw context.
// No engine or COM calls. Does not assert light visibility, cross-scene equality, or shader coverage.
#include <cstdint>
#include <cstring>
#include <cmath>
namespace WosStagedCurrentSun {
constexpr uint32_t CurrentSceneVA=0x01115c54,FrameVA=0x011184f4;
constexpr uint32_t EnvironmentOverrideIdVA=0x010f7d78,SceneEnvironmentIdVA=0x010f853c;
constexpr uint32_t TintIdVA=0x01116320,TintFallbackVA=0x00f4a9e0;
enum class Status { Accepted, UnsupportedProducer, Context, PropertyId, Read, Pointer, NonFinite, Changed };
struct Inputs {
    uint32_t scene=0,instance=0,frame=0,instanceTable=0,sceneTable=0;
    uint32_t environmentId=0,sceneEnvironmentId=0,tintId=0,environment=0,sun=0,tintPointer=0;
    uint32_t environmentFlagsAddress=0,environmentFlags=0,environmentSlotAddress=0,tintFlagsAddress=0,tintFlags=0,tintSlotAddress=0;
    bool environmentOverride=false,tintOverride=false,sunPresent=false;
    float rawDirection[4]={},rawColor[4]={},shaderDirection[4]={},shaderColor[4]={},tint[4]={};
};
inline bool Bounded(uint32_t p,uint32_t size) {
    return p>=0x10000 && (p&3)==0 && uint64_t(p)+size<=0x100000000ull;
}
template<class Reader> bool Read(Reader& read,uint32_t p,void* out,uint32_t size) {
    return Bounded(p,size) && read(p,out,size);
}
inline bool Finite(const float* f,unsigned count) {
    for(unsigned i=0;i<count;++i)if(!std::isfinite(f[i]))return false;
    return true;
}
template<class Reader> Status Decode(Reader& read,uint32_t scene,uint32_t instance,uint32_t originalMaterialFlags,Inputs& out) {
    Inputs r;r.scene=scene;r.instance=instance;
    if(!(originalMaterialFlags&0x40))return Status::UnsupportedProducer;
    if(!Bounded(scene,0x4dc) || !Bounded(instance,0x88))return Status::Context;
    uint32_t active=0;
    if(!Read(read,CurrentSceneVA,&active,4) || !Read(read,FrameVA,&r.frame,4))return Status::Read;
    if(active!=scene)return Status::Context;
    if(!Read(read,instance+0x84,&r.instanceTable,4) ||
       !Read(read,EnvironmentOverrideIdVA,&r.environmentId,4) ||
       !Read(read,TintIdVA,&r.tintId,4))return Status::Read;
    // Existing native property table has two flags DWORDs followed by slots.
    // Unsupported IDs fail closed; never extend the array from untrusted IDs.
    if(r.environmentId>=64 || r.tintId>=64)return Status::PropertyId;
    if(!Bounded(r.instanceTable,8+64*4))return Status::Pointer;
    r.environmentFlagsAddress=r.instanceTable+4*(r.environmentId>>5);
    r.tintFlagsAddress=r.instanceTable+4*(r.tintId>>5);
    if(!Read(read,r.environmentFlagsAddress,&r.environmentFlags,4) ||
       !Read(read,r.tintFlagsAddress,&r.tintFlags,4))return Status::Read;
    r.environmentOverride=(r.environmentFlags&(1u<<(r.environmentId&31)))!=0;
    r.tintOverride=(r.tintFlags&(1u<<(r.tintId&31)))!=0;
    if(r.environmentOverride) {
        r.environmentSlotAddress=r.instanceTable+8+4*r.environmentId;
    } else {
        if(!Read(read,scene+0x4d8,&r.sceneTable,4) ||
           !Read(read,SceneEnvironmentIdVA,&r.sceneEnvironmentId,4))return Status::Read;
        if(r.sceneEnvironmentId>=64)return Status::PropertyId;
        if(!Bounded(r.sceneTable,8+64*4))return Status::Pointer;
        // 007b9610 reads this scene slot unconditionally: there is no native presence-bit test.
        r.environmentSlotAddress=r.sceneTable+8+4*r.sceneEnvironmentId;
    }
    if(!Read(read,r.environmentSlotAddress,&r.environment,4))return Status::Read;
    if(!Bounded(r.environment,0x330))return Status::Pointer;
    if(!Read(read,r.environment+0x32c,&r.sun,4))return Status::Read;
    r.sunPresent=r.sun!=0;
    if(r.sunPresent) {
        if(!Bounded(r.sun,0x60))return Status::Pointer;
        if(!Read(read,r.sun+0x40,r.rawColor,16) || !Read(read,r.sun+0x50,r.rawDirection,16))return Status::Read;
        if(!Finite(r.rawColor,3) || !Finite(r.rawDirection,3))return Status::NonFinite;
        for(unsigned i=0;i<3;++i){r.shaderColor[i]=r.rawColor[i];r.shaderDirection[i]=r.rawDirection[i];}
        // FX producer types 0x53 and 0x54 explicitly zero the upload W.
    }
    if(r.tintOverride) {
        r.tintSlotAddress=r.instanceTable+8+4*r.tintId;
        if(!Read(read,r.tintSlotAddress,&r.tintPointer,4))return Status::Read;
    } else r.tintPointer=TintFallbackVA;
    if(!Read(read,r.tintPointer,r.tint,16))return Status::Read;
    if(!Finite(r.tint,4))return Status::NonFinite;
    uint32_t sceneAfter=0,frameAfter=0;
    if(!Read(read,CurrentSceneVA,&sceneAfter,4) || !Read(read,FrameVA,&frameAfter,4))return Status::Read;
    if(sceneAfter!=scene || frameAfter!=r.frame)return Status::Changed;
    out=r;return Status::Accepted;
}
}
