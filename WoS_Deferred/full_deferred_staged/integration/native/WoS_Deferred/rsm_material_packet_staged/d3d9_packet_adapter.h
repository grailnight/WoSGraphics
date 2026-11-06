#pragma once
// Staged render-thread-only adapter. Trusted registration is mandatory: only
// successful D3D Create/Get outputs or typed Set inputs may enter Registry.
// No engine call, draw, shader binding, render-target binding or native VS0..150 write.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d9.h>
#include <bcrypt.h>
#include <vector>
#include <array>
#include <algorithm>
#include <utility>
#include <limits>
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_Deferred/rsm_material_packet_staged/material_packet.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_Deferred/rsm_device_ownership_staged/device_ownership.h"
#pragma comment(lib,"bcrypt.lib")
namespace WosStagedPacketD3D9 {
using Packet=WosStagedMaterialPacket::Packet;
using Scope=WosStagedMaterialPacket::Scope;
using Family=WosStagedMaterialPacket::Family;
inline bool EqualScope(const Scope& a,const Scope& b){
    return a.nativeContextVerified && a.sourceCodeVerified && a.sunCodeVerified && a.samplerCodeVerified &&
        b.nativeContextVerified && b.sourceCodeVerified && b.sunCodeVerified && b.samplerCodeVerified &&
        a.draw==b.draw && a.scene==b.scene && a.frame==b.frame && a.instance==b.instance && a.descriptor==b.descriptor;
}
// New Reader per Decode. A bad or over-budget read returns zeroed output.
struct Reader {
    unsigned calls=0;uint32_t bytes=0;unsigned failures=0;
    bool operator()(uint32_t address,void* output,uint32_t count){
        if(!output || !count || count>32768)return false;
        std::memset(output,0,count);
        if(calls>=128 || bytes>32768-count || address<0x10000 || uint64_t(address)+count>0x100000000ull){++failures;return false;}
        ++calls;bytes+=count;SIZE_T actual=0;
        if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(uintptr_t(address)),output,count,&actual) || actual!=count){
            std::memset(output,0,count);++failures;return false;}
        return true;
    }
};
inline bool Digest(const void* bytes,ULONG count,char (&out)[65]){
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    DWORD objectBytes=0,returned=0;unsigned char digest[32]={};bool ok=false;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return false;
    if(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectBytes),sizeof(objectBytes),&returned,0)>=0 && objectBytes<=65536){
        try{std::vector<unsigned char> object(objectBytes);
            if(BCryptCreateHash(algorithm,&hash,object.data(),objectBytes,nullptr,0,0)>=0){
                ok=BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<void*>(bytes)),count,0)>=0 && BCryptFinishHash(hash,digest,32,0)>=0;
                BCryptDestroyHash(hash);hash=nullptr;
            }
        }catch(...){ok=false;}
    }
    BCryptCloseAlgorithmProvider(algorithm,0);
    if(ok){constexpr char digits[]="0123456789abcdef";for(unsigned i=0;i<32;++i){out[2*i]=digits[digest[i]>>4];out[2*i+1]=digits[digest[i]&15];}out[64]=0;}
    return ok;
}
class Registry {
    struct Shader {IDirect3DPixelShader9* ps=nullptr;IDirect3DVertexShader9* vs=nullptr;uint64_t use=0;bool attempted=false,hashed=false;char digest[65]={};};
    struct Texture {IDirect3DTexture9* object=nullptr;IDirect3DBaseTexture9* originalInterface=nullptr;uintptr_t registeredAddress=0;uint64_t bytes=0,use=0;};
    IDirect3DDevice9* owner_=nullptr;std::vector<Shader> shaders_;std::vector<Texture> textures_;uint64_t serial_=0,textureBytes_=0;
    WosRsmDeviceOwnership::Proof ownership_;
    static void Release(Shader& s){if(s.ps)s.ps->Release();if(s.vs)s.vs->Release();s=Shader{};}
    void EvictTexture(size_t index){textureBytes_-=textures_[index].bytes;textures_[index].object->Release();if(textures_[index].originalInterface)textures_[index].originalInterface->Release();textures_.erase(textures_.begin()+index);++stats.textureEvictions;}
    bool ShaderOwner(IDirect3DDevice9* device){return device && device==owner_ && ownership_.Valid();}
    template<class T> bool ProveDevice(T* object){return ownership_.MatchesResourceOwner(object);}
    void DropResources(){for(auto& s:shaders_)Release(s);shaders_.clear();for(auto& t:textures_){t.object->Release();if(t.originalInterface)t.originalInterface->Release();}textures_.clear();textureBytes_=0;serial_=0;if(owner_)owner_->Release();owner_=nullptr;}
    template<class T> bool RegisterShader(IDirect3DDevice9* d,T* object,bool pixel){
        if(!ShaderOwner(d) || !object)return false;
        for(auto& s:shaders_)if((pixel && reinterpret_cast<void*>(s.ps)==object)||(!pixel && reinterpret_cast<void*>(s.vs)==object)){s.use=++serial_;return true;}
        if(!ProveDevice(object)){++stats.deviceMismatch;return false;}
        try{if(shaders_.size()==ShaderCap){auto i=std::min_element(shaders_.begin(),shaders_.end(),[](const Shader& a,const Shader& b){return a.use<b.use;});Release(*i);shaders_.erase(i);++stats.shaderEvictions;}
            Shader s;s.use=++serial_;object->AddRef();if(pixel)s.ps=reinterpret_cast<IDirect3DPixelShader9*>(object);else s.vs=reinterpret_cast<IDirect3DVertexShader9*>(object);
            try{shaders_.push_back(s);}catch(...){Release(s);throw;}return true;
        }catch(...){++stats.registrationFailure;return false;}
    }
    static uint64_t LevelBytes(const D3DSURFACE_DESC& d){
        uint64_t unit=0;bool block=false;
        switch(d.Format){
        case D3DFMT_DXT1:unit=8;block=true;break;
        case D3DFMT_DXT2:case D3DFMT_DXT3:case D3DFMT_DXT4:case D3DFMT_DXT5:unit=16;block=true;break;
        case D3DFMT_A8:case D3DFMT_L8:case D3DFMT_P8:unit=1;break;
        case D3DFMT_R5G6B5:case D3DFMT_X1R5G5B5:case D3DFMT_A1R5G5B5:case D3DFMT_A4R4G4B4:case D3DFMT_A8L8:case D3DFMT_V8U8:case D3DFMT_L16:case D3DFMT_R16F:unit=2;break;
        case D3DFMT_R8G8B8:unit=3;break;
        case D3DFMT_A8R8G8B8:case D3DFMT_X8R8G8B8:case D3DFMT_A8B8G8R8:case D3DFMT_X8B8G8R8:case D3DFMT_G16R16:case D3DFMT_G16R16F:case D3DFMT_R32F:case D3DFMT_A2R10G10B10:case D3DFMT_A2B10G10R10:unit=4;break;
        case D3DFMT_A16B16G16R16:case D3DFMT_A16B16G16R16F:case D3DFMT_G32R32F:unit=8;break;
        case D3DFMT_A32B32G32R32F:unit=16;break;
        default:return 0;
        }
        if(!d.Width || !d.Height || d.Width>16384 || d.Height>16384)return 0;
        return block?uint64_t((d.Width+3)/4)*((d.Height+3)/4)*unit:uint64_t(d.Width)*d.Height*unit;
    }
public:
    // F7 20260909_091838: >300 ordinary textures cross main/shadow phases.
    // Keep the independent64MiB strong-reference payload cap unchanged.
    static constexpr size_t ShaderCap=256,TextureCap=1024;
    static constexpr uint64_t TextureByteCap=64ull*1024*1024;
    struct Statistics {uint64_t shaderEvictions=0,textureEvictions=0,shaderMisses=0,textureMisses=0,hashAttempts=0,hashFailures=0,deviceMismatch=0,registrationFailure=0,unsupportedTexture=0;}stats;
    explicit Registry(IDirect3DDevice9* trustedDevice=nullptr){Initialize(trustedDevice);}
    Registry(const Registry&)=delete;Registry& operator=(const Registry&)=delete;
    ~Registry(){Reset();}
    void Reset(){DropResources();ownership_.Reset();}
    // Initialization / post-Present only. Ordinary binding changes keep pinned
    // ownership; resource registration never performs full binding validation.
    // Failed revalidation drops every cached resource before a later Establish.
    void Initialize(IDirect3DDevice9* trustedDevice){
        if(trustedDevice && trustedDevice==owner_ && ownership_.Valid()){
            if(!ownership_.ValidateBindings())DropResources();return;
        }
        if(trustedDevice)trustedDevice->AddRef();
        DropResources();
        if(ownership_.Establish(trustedDevice)){owner_=trustedDevice;trustedDevice=nullptr;}
        if(trustedDevice)trustedDevice->Release();
    }
    IDirect3DDevice9* Device()const{return owner_;}
    uint64_t FacadeKey()const{return ownership_.FacadeKey();}
    uint64_t ResourceOwnerKey()const{return ownership_.ResourceOwnerKey();}
    int OwnershipStage()const{return static_cast<int>(ownership_.evidence.stage);}
    size_t ShaderCount()const{return shaders_.size();}size_t TextureCount()const{return textures_.size();}uint64_t TextureBytes()const{return textureBytes_;}
    bool RegisterPixelShader(IDirect3DDevice9* d,IDirect3DPixelShader9* trusted){return RegisterShader(d,trusted,true);}
    bool RegisterVertexShader(IDirect3DDevice9* d,IDirect3DVertexShader9* trusted){return RegisterShader(d,trusted,false);}
    bool RegisterTexture(IDirect3DDevice9* d,IDirect3DBaseTexture9* trusted){
        if(!ShaderOwner(d) || !trusted)return false;
        for(auto& t:textures_)if(t.registeredAddress==reinterpret_cast<uintptr_t>(trusted)||t.object==trusted){t.use=++serial_;return true;}
        if(!ProveDevice(trusted)){++stats.deviceMismatch;return false;}
        if(trusted->GetType()!=D3DRTYPE_TEXTURE){++stats.unsupportedTexture;return false;}
        IDirect3DTexture9* typed=nullptr;
        if(FAILED(trusted->QueryInterface(__uuidof(IDirect3DTexture9),reinterpret_cast<void**>(&typed))) || !typed){++stats.registrationFailure;return false;}
        uint64_t bytes=0;const UINT levels=typed->GetLevelCount();bool valid=levels>0 && levels<=16;
        for(UINT i=0;valid && i<levels;++i){D3DSURFACE_DESC desc{};valid=SUCCEEDED(typed->GetLevelDesc(i,&desc));valid=valid && !(desc.Usage&(D3DUSAGE_RENDERTARGET|D3DUSAGE_DEPTHSTENCIL));const uint64_t part=valid?LevelBytes(desc):0;valid=part!=0 && part<=TextureByteCap && bytes<=TextureByteCap-part;if(valid)bytes+=part;}
        if(!valid || bytes>TextureByteCap){typed->Release();++stats.unsupportedTexture;return false;}
        IDirect3DBaseTexture9* originalInterface=typed==trusted?nullptr:trusted;
        if(originalInterface)originalInterface->AddRef();
        try{while(!textures_.empty() && (textures_.size()>=TextureCap || textureBytes_+bytes>TextureByteCap)){
                const auto i=std::min_element(textures_.begin(),textures_.end(),[](const Texture& a,const Texture& b){return a.use<b.use;});EvictTexture(size_t(i-textures_.begin()));}
            textures_.push_back({typed,originalInterface,reinterpret_cast<uintptr_t>(trusted),bytes,++serial_});textureBytes_+=bytes;return true;
        }catch(...){typed->Release();if(originalInterface)originalInterface->Release();++stats.registrationFailure;return false;}
    }
    // Raw-address lookup performs no COM call unless registration pinned it.
    bool operator()(uint32_t address,bool pixel,const char* expected){
        for(auto& s:shaders_)if((pixel?reinterpret_cast<uintptr_t>(s.ps):reinterpret_cast<uintptr_t>(s.vs))==address && address){
            s.use=++serial_;if(!s.attempted){s.attempted=true;++stats.hashAttempts;UINT n=0;
                HRESULT hr=pixel?s.ps->GetFunction(nullptr,&n):s.vs->GetFunction(nullptr,&n);
                if(SUCCEEDED(hr) && n>=8 && n<=1024*1024 && (n&3)==0){try{std::vector<unsigned char> raw(n);UINT actual=n;
                    hr=pixel?s.ps->GetFunction(raw.data(),&actual):s.vs->GetFunction(raw.data(),&actual);
                    if(SUCCEEDED(hr) && actual==n)s.hashed=Digest(raw.data(),n,s.digest);
                }catch(...){s.hashed=false;}}
                if(!s.hashed)++stats.hashFailures;
            }return s.hashed && expected && std::strcmp(s.digest,expected)==0;
        }++stats.shaderMisses;return false;
    }
    // Returned reference belongs to caller, never to an inferred unregistered pointer.
    IDirect3DTexture9* AcquireTexture(uint32_t address){for(auto& t:textures_)if(address && (t.registeredAddress==address || reinterpret_cast<uintptr_t>(t.object)==address)){
            t.use=++serial_;t.object->AddRef();return t.object;}++stats.textureMisses;return nullptr;}
};
// GetSamplerState is independent of GetTexture; an unbound shadow stage still
// supplies the inherited border/LOD/sRGB/element/DMAP state for the source draw.
struct Baseline {
    IDirect3DDevice9* device;Scope scope;
    bool operator()(const Scope& current,unsigned stage,uint32_t (&values)[14],uint32_t& mask){
        std::memset(values,0,sizeof(values));mask=0;if(!device || stage>=16 || !EqualScope(scope,current))return false;
        for(unsigned i=1;i<=13;++i){DWORD value=0;if(FAILED(device->GetSamplerState(stage,static_cast<D3DSAMPLERSTATETYPE>(i),&value)))return false;values[i]=value;mask|=1u<<i;}return true;
    }
};
struct ConstantRange {bool pixel=false;UINT first=0,count=0;float values[12]={};};
class BindPlan {
    friend bool Prepare(BindPlan&,const Packet&,const Scope&,Registry&);
    template<class>friend class BasicBindScope;
    Scope scope_;IDirect3DDevice9* device_=nullptr;IDirect3DTexture9* texture_=nullptr;
    bool valid_=false;unsigned stage_=0,rangeCount_=0;ConstantRange ranges_[4];uint32_t sampler_[14]={},baseline_[14]={};float direction_[4]={};
    void Move(BindPlan& other){scope_=other.scope_;device_=other.device_;texture_=other.texture_;valid_=other.valid_;stage_=other.stage_;rangeCount_=other.rangeCount_;
        std::memcpy(ranges_,other.ranges_,sizeof(ranges_));std::memcpy(sampler_,other.sampler_,sizeof(sampler_));std::memcpy(baseline_,other.baseline_,sizeof(baseline_));std::memcpy(direction_,other.direction_,sizeof(direction_));family=other.family;
        other.device_=nullptr;other.texture_=nullptr;other.valid_=false;}
public:
    Family family=Family::Old69;
    BindPlan()=default;~BindPlan(){Reset();}BindPlan(const BindPlan&)=delete;BindPlan& operator=(const BindPlan&)=delete;
    BindPlan(BindPlan&& other)noexcept{Move(other);}BindPlan& operator=(BindPlan&& other)noexcept{if(this!=&other){Reset();Move(other);}return *this;}
    void Reset(){if(texture_)texture_->Release();if(device_)device_->Release();texture_=nullptr;device_=nullptr;valid_=false;}
    bool ValidFor(const Scope& s)const{return valid_ && texture_ && device_ && EqualScope(scope_,s);}
    const float* SunDirection()const{return direction_;}
};
inline bool Prepare(BindPlan& plan,const Packet& packet,const Scope& scope,Registry& registry){
    plan.Reset();if(!WosStagedMaterialPacket::SameScope(packet,scope) || !registry.Device())return false;
    const bool old69=packet.family==Family::Old69;
    if(packet.diffuseStage!=(old69?3u:0u) || packet.baselineMask!=0x3ffe || !packet.current.sunPresent ||
        !WosStagedMaterialPacket::Finite(packet.current.shaderDirection,4) || !WosStagedMaterialPacket::Finite(packet.current.shaderColor,4) ||
        !WosStagedMaterialPacket::Finite(packet.current.tint,4) || packet.current.shaderDirection[3]!=0 || packet.current.shaderColor[3]!=0)return false;
    BindPlan p;p.family=packet.family;p.scope_=scope;p.stage_=packet.diffuseStage;
    auto add=[&](bool ps,unsigned first,unsigned count,const float* values){auto& r=p.ranges_[p.rangeCount_++];r.pixel=ps;r.first=first;r.count=count;std::memcpy(r.values,values,count*16);};
    if(old69){
        if(!std::isfinite(packet.diffuseMultiplier) || !WosStagedMaterialPacket::Finite(packet.uvScale,2))return false;
        float sun[8];std::memcpy(sun,packet.current.shaderDirection,16);std::memcpy(sun+4,packet.current.shaderColor,16);
        const float multiplier[4]={packet.diffuseMultiplier,0,0,0},uv[8]={packet.uvScale[0],0,0,0,packet.uvScale[1],0,0,0};
        add(false,156,1,packet.current.tint);add(true,19,2,sun);add(true,33,1,multiplier);add(true,36,2,uv);
    }else{
        if((packet.family!=Family::Old72 && packet.family!=Family::Static111) || !std::isfinite(packet.animationTime) || !WosStagedMaterialPacket::Finite(packet.diffuseU,4) ||
            !WosStagedMaterialPacket::Finite(packet.diffuseV,4) || !WosStagedMaterialPacket::Finite(packet.materialColor,4))return false;
        float uv[12]={packet.animationTime,0,0,0};std::memcpy(uv+4,packet.diffuseU,16);std::memcpy(uv+8,packet.diffuseV,16);
        float sun[12];std::memcpy(sun,packet.current.tint,16);std::memcpy(sun+4,packet.current.shaderDirection,16);std::memcpy(sun+8,packet.current.shaderColor,16);
        add(false,156,3,uv);add(true,21,1,packet.materialColor);add(true,29,3,sun);
    }
    p.texture_=registry.AcquireTexture(packet.d3dTexture);if(!p.texture_)return false;
    p.device_=registry.Device();p.device_->AddRef();std::memcpy(p.sampler_,packet.sampler,sizeof(p.sampler_));std::memcpy(p.baseline_,packet.baselineSampler,sizeof(p.baseline_));std::memcpy(p.direction_,packet.current.shaderDirection,16);p.valid_=true;plan=std::move(p);return true;
}
// Backend template is also exercised by a CPU failure-injection fixture. Device
// supplies only Get/Set constants, texture and sampler calls plus Identity().
struct DeviceBackend {
    IDirect3DDevice9* d;
    IDirect3DDevice9* Identity()const{return d;}
    HRESULT GetConstants(bool ps,UINT first,float* values,UINT count){return ps?d->GetPixelShaderConstantF(first,values,count):d->GetVertexShaderConstantF(first,values,count);}
    HRESULT SetConstants(bool ps,UINT first,const float* values,UINT count){return ps?d->SetPixelShaderConstantF(first,values,count):d->SetVertexShaderConstantF(first,values,count);}
    HRESULT GetTexture(UINT stage,IDirect3DBaseTexture9** t){return d->GetTexture(stage,t);}HRESULT SetTexture(UINT stage,IDirect3DBaseTexture9* t){return d->SetTexture(stage,t);}
    HRESULT GetSampler(UINT stage,unsigned state,DWORD* v){return d->GetSamplerState(stage,static_cast<D3DSAMPLERSTATETYPE>(state),v);}
    HRESULT SetSampler(UINT stage,unsigned state,DWORD v){return d->SetSamplerState(stage,static_cast<D3DSAMPLERSTATETYPE>(state),v);}
};
template<class Backend>class BasicBindScope {
    Backend backend_;const BindPlan& plan_;bool ready_=false,modified_=false,restored_=false,restoreOK_=true;
    float saved_[4][12]={};DWORD sampler_[14]={};IDirect3DBaseTexture9* texture_=nullptr;
public:
    BasicBindScope(Backend backend,const BindPlan& p,const Scope& scope):backend_(backend),plan_(p){
        if(!p.ValidFor(scope) || backend_.Identity()!=p.device_)return;
        for(unsigned i=0;i<p.rangeCount_;++i){const auto& r=p.ranges_[i];if(FAILED(backend_.GetConstants(r.pixel,r.first,saved_[i],r.count)))return;}
        for(unsigned i=1;i<=13;++i)if(FAILED(backend_.GetSampler(p.stage_,i,sampler_+i)))return;
        for(unsigned i=1;i<=13;++i)if(sampler_[i]!=p.baseline_[i])return;
        if(FAILED(backend_.GetTexture(p.stage_,&texture_)))return;
        // A setter may alter state even on failure. All state was saved first;
        // restore every owned range/state for any partial binding failure.
        modified_=true;
        for(unsigned i=0;i<p.rangeCount_;++i){const auto& r=p.ranges_[i];if(FAILED(backend_.SetConstants(r.pixel,r.first,r.values,r.count))){Restore();return;}}
        for(unsigned i=1;i<=13;++i)if(FAILED(backend_.SetSampler(p.stage_,i,p.sampler_[i]))){Restore();return;}
        if(FAILED(backend_.SetTexture(p.stage_,p.texture_))){Restore();return;}ready_=true;
    }
    BasicBindScope(const BasicBindScope&)=delete;BasicBindScope& operator=(const BasicBindScope&)=delete;
    ~BasicBindScope(){Restore();if(texture_)texture_->Release();}
    bool Ready()const{return ready_ && !restored_;}
    bool Restore(){if(restored_)return restoreOK_;restored_=true;ready_=false;
        if(modified_){const auto& p=plan_;if(FAILED(backend_.SetTexture(p.stage_,texture_)))restoreOK_=false;
            for(unsigned i=1;i<=13;++i)if(FAILED(backend_.SetSampler(p.stage_,i,sampler_[i])))restoreOK_=false;
            for(unsigned i=0;i<p.rangeCount_;++i){const auto& r=p.ranges_[i];if(FAILED(backend_.SetConstants(r.pixel,r.first,saved_[i],r.count)))restoreOK_=false;}}
        return restoreOK_;
    }
    bool RestoreSucceeded()const{return restoreOK_;}
};
class BindScope:public BasicBindScope<DeviceBackend>{public:BindScope(IDirect3DDevice9* device,const BindPlan& plan,const Scope& scope):BasicBindScope(DeviceBackend{device},plan,scope){}};
}
