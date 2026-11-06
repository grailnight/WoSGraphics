#pragma once
// Reviewed native point contract: 008A2BA0 cases 55/56/57 upload count,
// position.xyz + 1/nativeRange, and RGB. ps_0013 computes
// attenuation = 1-saturate(length(lightPosition-worldPosition)*inverseRange).
// CTAB describes register locations only; callers still identify supported
// material bytecode and the current scene independently.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <string>
#include <vector>
#ifdef _WIN32
#include <d3d9.h>
#endif

namespace WosDeferredLights {
constexpr unsigned ConstantCount=224, MaxPoints=8, RowTexels=32, RowFloats=RowTexels*4, MaxSets=1024;
struct Register { unsigned index=0,count=0; };
struct Binding {
    Register direction,color,pointPosition,pointColor,pointCount;
    bool hasDirectional=false,hasPoints=false,bakedDirectional=false,valid=false;
};
struct LightSet { std::array<float,RowFloats> texels{}; };
namespace Detail {
inline uint16_t U16(const unsigned char* p) { uint16_t v=0;std::memcpy(&v,p,2);return v; }
inline uint32_t U32(const unsigned char* p) { uint32_t v=0;std::memcpy(&v,p,4);return v; }
inline bool Range(size_t total,size_t offset,size_t length) { return offset<=total && length<=total-offset; }
inline bool String(const unsigned char* bytes,size_t total,uint32_t offset,std::string& out) {
    if(offset>=total)return false;
    const auto* end=static_cast<const unsigned char*>(std::memchr(bytes+offset,0,total-offset));
    if(!end || size_t(end-(bytes+offset))>4096)return false;
    out.assign(reinterpret_cast<const char*>(bytes+offset),size_t(end-(bytes+offset)));return true;
}
inline bool Table(const unsigned char* b,size_t bytes,uint32_t version,Binding& result,bool allowBaked) {
    if(bytes<28 || U32(b)!=28 || U32(b+8)!=version)return false;
    const uint32_t entries=U32(b+12),start=U32(b+16);
    if(entries>1024 || !Range(bytes,start,size_t(entries)*20))return false;
    std::string ignored;
    if(!String(b,bytes,U32(b+4),ignored) || !String(b,bytes,U32(b+24),ignored))return false;
    bool found[5]={};Binding binding;
    Register* destinations[]={&binding.direction,&binding.color,&binding.pointPosition,&binding.pointColor,&binding.pointCount};
    const char* names[]={"gDirLightsDir","gDirLightsColor","gPointLightsPosRange","gPointLightsColor","gNumPointLights"};
    for(uint32_t i=0;i<entries;++i) {
        const auto* e=b+start+size_t(i)*20;std::string name;
        if(!String(b,bytes,U32(e),name))return false;
        const uint32_t type=U32(e+12),value=U32(e+16);
        if(!Range(bytes,type,16))return false;
        const auto* t=b+type;
        const unsigned set=U16(e+4),index=U16(e+6),count=U16(e+8);
        // Default-value offsets are data, not host pointers.
        if(value && !Range(bytes,value,size_t(count)*16))return false;
        int known=-1;for(unsigned n=0;n<5;++n)if(name==names[n])known=int(n);
        if(known<0)continue;
        if(found[known] || set!=2 || !count || index>=ConstantCount || count>ConstantCount-index)return false;
        const unsigned cls=U16(t),kind=U16(t+2),rows=U16(t+4),cols=U16(t+6),elements=U16(t+8),members=U16(t+10);
        if(kind!=3 || rows!=1 || members || !elements)return false; // D3DXPT_FLOAT
        if(known==4) { if(cls!=0 || cols!=1 || count!=1 || elements!=1)return false; }
        else if(cls!=1 || cols!=4 || (known<2 && (count!=1 || elements!=1)) || count>elements)return false;
        found[known]=true;*destinations[known]={index,count};
    }
        const bool baked=allowBaked&&!found[0]&&found[1]&&!found[2]&&!found[3]&&!found[4];
    if((found[0]!=found[1]&&!baked) || found[2]!=found[3] || found[2]!=found[4])return false;
    binding.hasDirectional=found[0]&&found[1];binding.hasPoints=found[2];binding.bakedDirectional=baked;
    binding.valid=true;
    // Two distinct named light inputs cannot alias the same constant registers.
    for(unsigned a=0;a<5;++a)for(unsigned c=a+1;c<5;++c)if(found[a]&&found[c]) {
        const auto& x=*destinations[a];const auto& y=*destinations[c];
        if(x.index<y.index+y.count && y.index<x.index+x.count)return false;
    }
    result=binding;return true;
}
inline bool RegisterValid(Register r) { return r.count && r.index<ConstantCount && r.count<=ConstantCount-r.index; }
}
// Uses instruction/comment boundaries instead of searching arbitrary operands
// for the CTAB fourcc. The returned binding is cleared on every failure.
inline bool Parse(const void* shaderBytes,size_t byteCount,Binding& out,bool allowBaked=false) {
    out={};if(!shaderBytes || byteCount<8 || byteCount%4 || byteCount>1024*1024)return false;
    const auto* b=static_cast<const unsigned char*>(shaderBytes);const size_t words=byteCount/4;
    const uint32_t version=Detail::U32(b);
    if(version!=0xffff0200u && version!=0xffff0300u)return false;
    bool haveTable=false;Binding binding;
    for(size_t i=1;i<words;) {
        const uint32_t token=Detail::U32(b+i*4),opcode=token&0xffffu;
        if(opcode==0xffffu) { if(token!=0xffffu || i+1!=words || !haveTable)return false;out=binding;return true; }
        if(opcode==0xfffeu) {
            const size_t count=(token>>16)&0x7fffu;
            if(!count || count>words-i-1)return false;
            if(Detail::U32(b+(i+1)*4)==0x42415443u) {
                if(haveTable || count<8 || !Detail::Table(b+(i+2)*4,(count-1)*4,version,binding,allowBaked))return false;
                haveTable=true;
            }
            i+=count+1;
        } else {
            if(token&0x80000000u)return false;
            const size_t count=(token>>24)&0xfu;
            if(count>words-i-1)return false;
            i+=count+1;
        }
    }
    return false;
}
inline bool Valid(const Binding& b) {
    if(!b.valid)return false;
    if(b.bakedDirectional&&(b.hasDirectional||b.hasPoints||!Detail::RegisterValid(b.color)||b.color.count!=1))return false;
    if(b.hasDirectional && (!Detail::RegisterValid(b.direction)||b.direction.count!=1||!Detail::RegisterValid(b.color)||b.color.count!=1))return false;
    if(b.hasPoints && (!Detail::RegisterValid(b.pointPosition)||!Detail::RegisterValid(b.pointColor)||!Detail::RegisterValid(b.pointCount)||b.pointCount.count!=1))return false;
    return true;
}
// Exact row layout: [0] towardSun.xyz,pointCount; [1] sunRGB,0;
// [2+2*i] camera-relative pointPosition.xyz,inverseRange; [3+2*i] RGB,0;
// [18..31] all zero. Exact opt-in baked mode uses sunColor.w=1, zero direction.
// This flags existing vertex-baked illumination; no direction/cosine is invented.
inline bool Pack(const Binding& b,const float bank[ConstantCount][4],const double origin[3],LightSet& out) {
    out={};if(!bank||!origin||!Valid(b))return false;
    for(unsigned i=0;i<3;++i)if(!std::isfinite(origin[i]))return false;
    LightSet result;
    if(b.hasDirectional)for(unsigned i=0;i<3;++i) {
        const float direction=bank[b.direction.index][i],color=bank[b.color.index][i];
        if(!std::isfinite(direction)||!std::isfinite(color))return false;
        result.texels[i]=direction;result.texels[4+i]=color;
    }
    if(b.bakedDirectional) {
        for(unsigned i=0;i<3;++i){const float color=bank[b.color.index][i];if(!std::isfinite(color))return false;result.texels[4+i]=color;}
        result.texels[7]=1.0f;
    }
    if(b.hasPoints) {
        const float nativeCount=bank[b.pointCount.index][0];
        if(!std::isfinite(nativeCount)||nativeCount<0||nativeCount>MaxPoints||std::floor(nativeCount)!=nativeCount)return false;
        unsigned count=unsigned(nativeCount);if(count>b.pointPosition.count)count=b.pointPosition.count;if(count>b.pointColor.count)count=b.pointColor.count;
        result.texels[3]=float(count);
        for(unsigned i=0;i<count;++i) {
            const float* position=bank[b.pointPosition.index+i];const float* color=bank[b.pointColor.index+i];
            if(!std::isfinite(position[3])||position[3]<=0)return false;
            for(unsigned j=0;j<3;++j) {
                const double relative=double(position[j])-origin[j];
                if(!std::isfinite(relative)||std::abs(relative)>(std::numeric_limits<float>::max)()||!std::isfinite(color[j]))return false;
                result.texels[(2+2*i)*4+j]=float(relative);result.texels[(3+2*i)*4+j]=color[j];
            }
            result.texels[(2+2*i)*4+3]=position[3];
        }
    }
    out=result;return true;
}
#ifdef _WIN32
inline bool Read(IDirect3DDevice9* device,const Binding& binding,const double origin[3],LightSet& out) {
    out={};if(!device||!Valid(binding))return false;
    // Not zeroed: Pack reads only registers a successful read below filled,
    // and clearing 224 registers per draw was measurable frame time.
    float bank[ConstantCount][4];
    auto read=[&](Register r,unsigned cap) { const unsigned count=r.count<cap?r.count:cap;return SUCCEEDED(device->GetPixelShaderConstantF(r.index,bank[r.index],count)); };
    if(binding.hasDirectional && (!read(binding.direction,1)||!read(binding.color,1)))return false;
    if(binding.bakedDirectional && !read(binding.color,1))return false;
    if(binding.hasPoints) {
        if(!read(binding.pointCount,1))return false;
        const float nativeCount=bank[binding.pointCount.index][0];
        if(!std::isfinite(nativeCount)||nativeCount<0||nativeCount>MaxPoints||std::floor(nativeCount)!=nativeCount)return false;
        unsigned count=unsigned(nativeCount);
        if(count>binding.pointPosition.count)count=binding.pointPosition.count;
        if(count>binding.pointColor.count)count=binding.pointColor.count;
        // Pack ignores inactive slots. Avoid querying stale arrays at count zero
        // and avoid fetching unused tail registers when only a prefix is active.
        if(count&&(!read(binding.pointPosition,count)||!read(binding.pointColor,count)))return false;
    }
    return Pack(binding,bank,origin,out);
}
#endif
// Intern is called once per primary draw, thousands of times per frame, so
// its cost is CPU frame time. Keys compare bitwise: -0.0 and 0.0 are distinct
// sets, and a NaN pattern can never match because none is ever stored. The
// finite check therefore runs only for a set that is not already known.
class Registry {
    using Key=std::array<uint32_t,RowFloats>;
    struct KeyHash {
        size_t operator()(const Key& key) const {
            uint64_t h=0x9e3779b97f4a7c15ull;
            for(size_t i=0;i<RowFloats;i+=2){
                uint64_t word=uint64_t(key[i])|(uint64_t(key[i+1])<<32);
                h^=word;h*=0xff51afd7ed558ccdull;h^=h>>33;
            }
            return size_t(h);
        }
    };
    std::unordered_map<Key,uint16_t,KeyHash> lookup;
    std::vector<float> rows;
    // Consecutive draws usually share one light set; remember the last hit.
    Key lastKey{};uint16_t lastId=0;bool hasLast=false;
public:
    Registry() : rows(RowFloats,0) { lookup.reserve(MaxSets); }
    // Row zero is permanently reserved for uncovered/invalid G-buffer pixels.
    void Clear() { lookup.clear();rows.resize(RowFloats);std::fill(rows.begin(),rows.end(),0.0f);hasLast=false;lastId=0; }
    unsigned Size() const { return unsigned(rows.size()/RowFloats); }
    const std::vector<float>& Rows() const { return rows; }
    bool Intern(const LightSet& set,uint16_t& id) {
        id=0;Key key{};std::memcpy(key.data(),set.texels.data(),sizeof(float)*RowFloats);
        if(hasLast&&key==lastKey){id=lastId;return true;}
        const auto existing=lookup.find(key);
        if(existing!=lookup.end()){lastKey=key;lastId=existing->second;hasLast=true;id=lastId;return true;}
        for(float value:set.texels)if(!std::isfinite(value))return false;
        if(Size()>=MaxSets)return false;
        const uint16_t next=uint16_t(Size());
        // Allocate the map entry first; roll it back if vector allocation fails.
        try {
            auto inserted=lookup.emplace(key,next);
            try { rows.insert(rows.end(),set.texels.begin(),set.texels.end()); }
            catch(...) { lookup.erase(inserted.first);return false; }
        } catch(...) { return false; }
        lastKey=key;lastId=next;hasLast=true;id=next;return true;
    }
};
}
