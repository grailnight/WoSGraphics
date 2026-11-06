#pragma once
// Staged exact native009ea730 ->009e24b0/0072c140 sampler-state subset.
// Caller supplies an independently proved original FX definition, verifies code
// gates once, and brackets the packet's scene/frame. No engine or COM calls.
#include <cstdint>
#include <cstring>
namespace WosStagedSamplerState {
enum class Status { Accepted, DefinitionPointer, Read, TypeOrSize, ValuePointer, UnsupportedEnum, Changed };
struct State {
    uint32_t definition=0,valuePointer=0,nameHash=0;
    uint32_t raw[10]={};
    // Index is D3DSAMPLERSTATETYPE (1..13); zero is unused.
    // Only masks1/2/3/5/6/7/10 are native-written by this binding path.
    uint32_t values[14]={};
    uint32_t writtenMask=0;
};
inline bool Bounded(uint32_t p,uint32_t bytes) {
    return p>=0x10000 && (p&3)==0 && uint64_t(p)+bytes<=0x100000000ull;
}
inline bool Convert(const uint32_t (&raw)[10],State& result) {
    // Native passes valid address/filter enum values directly except anisotropic
    // MAG/MIP, which0072c140 converts from3 to2. Reject unimplemented enums;
    // never silently force an unsupported material to linear filtering.
    if(raw[1]<1 || raw[1]>5 || raw[2]<1 || raw[2]>5 || raw[3]<1 || raw[3]>5 ||
       raw[5]<1 || raw[5]>3 || raw[6]<1 || raw[6]>3 || raw[4]>3)return false;
    State r=result;std::memcpy(r.raw,raw,sizeof(raw));
    r.values[1]=raw[1];r.values[2]=raw[2];r.values[3]=raw[3];
    r.values[6]=raw[5];r.values[5]=raw[6]==3?2:raw[6];r.values[7]=raw[4]==3?2:raw[4];
    r.values[10]=1;
    r.writtenMask=(1u<<1)|(1u<<2)|(1u<<3)|(1u<<5)|(1u<<6)|(1u<<7)|(1u<<10);
    result=r;return true;
}
template<class Reader> Status Decode(Reader& read,uint32_t definition,State& out) {
    if(!Bounded(definition,44))return Status::DefinitionPointer;
    uint32_t words[11]={};
    if(!read(definition,words,sizeof(words)))return Status::Read;
    if(words[6]!=14 || words[8]!=40)return Status::TypeOrSize;
    if(!Bounded(words[7],40))return Status::ValuePointer;
    State r;r.definition=definition;r.valuePointer=words[7];r.nameHash=words[1];
    uint32_t raw[10]={};
    if(!read(r.valuePointer,raw,sizeof(raw)))return Status::Read;
    if(!Convert(raw,r))return Status::UnsupportedEnum;
    // Fresh values are copied, not retained as engine pointers. Recheck both
    // typed definition and payload so a changing read fails closed.
    uint32_t after[11]={},rawAfter[10]={};
    if(!read(definition,after,sizeof(after)) || !read(r.valuePointer,rawAfter,sizeof(rawAfter)))return Status::Read;
    if(std::memcmp(words,after,sizeof(words)) || std::memcmp(raw,rawAfter,sizeof(raw)))return Status::Changed;
    out=r;return Status::Accepted;
}
inline bool MergeCurrentBaseline(const State& native,const uint32_t (&baseline)[14],uint32_t availableMask,uint32_t (&out)[14]) {
    constexpr uint32_t all=(1u<<14)-2;
    if((availableMask&all)!=all || native.writtenMask!=((1u<<1)|(1u<<2)|(1u<<3)|(1u<<5)|(1u<<6)|(1u<<7)|(1u<<10)))return false;
    uint32_t result[14];std::memcpy(result,baseline,sizeof(result));
    for(unsigned i=1;i<=13;++i)if(native.writtenMask&(1u<<i))result[i]=native.values[i];
    std::memcpy(out,result,sizeof(result));return true;
}
}
