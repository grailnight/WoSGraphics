#pragma once
// Include after deferred_runtime.h and native_indirect.h. Keep one cumulative
// receiver map across primary batches; the legacy material map has holes where
// its DrawScope is skipped and must never be used for a primary-owned scene.
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/primary/gi_adapter.h"
namespace WosPrimaryGIProvider {
static WosPrimaryGI::Adapter adapter;
static IDirect3DPixelShader9* conversion=nullptr;
static IDirect3DDevice9* deviceOwner=nullptr;
static uint64_t ownershipFrame=0;
static bool allScenesOwned=false;
struct Ownership { uint32_t scene=0;IDirect3DTexture9*depth=nullptr;IUnknown*identity=nullptr; };
static std::vector<Ownership> ownership;
static unsigned mergeFailures=0,legacyReads=0,primaryReads=0,unavailableReads=0;
template<class T>static void Release(T*&p){if(p)p->Release();p=nullptr;}
static void ClearOwnership(){for(auto&o:ownership){Release(o.depth);Release(o.identity);}ownership.clear();allScenesOwned=false;}
static void SyncFrame(){
    if(ownershipFrame==WosDeferred::frameSerial)return;
    ClearOwnership();adapter.Invalidate();ownershipFrame=WosDeferred::frameSerial;
}
static void Invalidate(){SyncFrame();adapter.Invalidate();}
static void Reset(){
    adapter.Reset();Release(conversion);Release(deviceOwner);ClearOwnership();ownershipFrame=0;
}
static void Touch(){
    SyncFrame();
    const auto scene=WosDeferred::camera.scene;auto*depth=WosDeferred::depthTexture;
    if(!scene||!depth){allScenesOwned=true;return;}
    for(const auto&o:ownership)if(o.scene==scene&&o.depth==depth)return;
    if(ownership.size()>=64){allScenesOwned=true;return;}
    Ownership o;o.scene=scene;o.depth=depth;
    if(FAILED(depth->QueryInterface(IID_IUnknown,reinterpret_cast<void**>(&o.identity)))){allScenesOwned=true;return;}
    depth->AddRef();ownership.push_back(o);
}
static bool Owned(uint32_t scene,IDirect3DTexture9*depth){
    if(allScenesOwned)return true;
    for(const auto&o:ownership)if(o.scene==scene&&o.depth==depth)return true;
    if(!depth)return false;
    IUnknown*identity=nullptr;if(FAILED(depth->QueryInterface(IID_IUnknown,reinterpret_cast<void**>(&identity))))return !ownership.empty();
    bool owned=false;for(const auto&o:ownership)if(o.scene==scene&&o.identity==identity){owned=true;break;}
    Release(identity);return owned;
}
static void ClearDepth(IDirect3DDevice9*d){
    SyncFrame();if(allScenesOwned){adapter.Invalidate();return;}if(ownership.empty())return;
    IDirect3DSurface9*surface=nullptr;IDirect3DTexture9*depth=nullptr;
    if(FAILED(d->GetDepthStencilSurface(&surface))||!surface)return;
    const bool texture=SUCCEEDED(surface->GetContainer(IID_IDirect3DTexture9,reinterpret_cast<void**>(&depth)));Release(surface);
    if(texture){for(const auto&o:ownership)if(o.depth==depth){adapter.Invalidate();break;}}
    else if(allScenesOwned)adapter.Invalidate();
    Release(depth);
}
static bool Shader(IDirect3DDevice9*d){
    if(deviceOwner&&deviceOwner!=d)Reset();
    if(conversion)return true;
    auto code=WosIndirectMaterials::Read(WosDeferred::Directory()+"\\gi_adapter.cso");
    if(code.size()<8||code.size()%4||FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(code.data()),&conversion)))return false;
    deviceOwner=d;deviceOwner->AddRef();return true;
}
static void Merge(IDirect3DDevice9*d){
    Touch();
    if(!WosIndirect::enabled||!WosDeferred::enabled||WosDeferred::unsafeDeviceState){adapter.Invalidate();return;}
    if(!Shader(d)){adapter.Invalidate();++mergeFailures;return;}
    WosPrimaryGI::Snapshot s;
    s.frame=WosDeferred::frameSerial;s.generation=WosDeferred::receiverGeneration;
    s.scene=WosDeferred::camera.scene;s.width=WosDeferred::width;s.height=WosDeferred::height;
    s.depth=WosDeferred::depthTexture;s.normalShadow=WosDeferred::gbuffer[1];s.diffuseFog=WosDeferred::gbuffer[2];s.current=WosDeferred::pending;
    if(!adapter.Merge(d,s,conversion)){
        ++mergeFailures;
        if(adapter.StateRestoreFailed()){
            WosDeferred::unsafeDeviceState=true;WosDeferred::Fault("GI receiver conversion restore failed; primary disabled until reset");
        }
    }
}
static bool Get(IDirect3DDevice9*d,IDirect3DTexture9*depth,uint32_t scene,IDirect3DTexture9*&normal,IDirect3DTexture9*&rho){
    normal=rho=nullptr;SyncFrame();
    if(WosDeferred::pending&&!WosDeferred::busy)WosDeferred::Flush(d);
    if(WosDeferred::unsafeDeviceState){++unavailableReads;return false;}
    if(Owned(scene,depth)){
        const bool ok=WosIndirect::enabled&&adapter.Get(d,WosDeferred::frameSerial,scene,depth,normal,rho);
        if(ok)++primaryReads;else ++unavailableReads;
        return ok;
    }
    ++legacyReads;return WosIndirectMaterials::Get(depth,scene,normal,rho);
}
static void Install(){
    WosDeferred::receiverWrite=Touch;WosDeferred::receiverMerge=Merge;
    WosDeferred::receiverInvalidate=Invalidate;WosDeferred::receiverReset=Reset;
    WosIndirectMaterials::receiverProvider=Get;
    WosIndirectMaterials::receiverProviderReset=Reset;
}
}
