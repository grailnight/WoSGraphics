#pragma once
// Composite native map GI while opaque receiver data is still current, before
// forward depth-writing transparency invalidates it. One commit per frame.
namespace WosForwardGI {
static uint64_t committed=0,unavailable=0,failed=0;
static float lastSource[4]={};
static const char*reason="waiting for opaque geometry";
static void Status(){
 static ULONGLONG previous=0;auto now=GetTickCount64();if(previous&&now-previous<1000)return;previous=now;
 FILE*f=std::fopen((WosDeferred::Directory()+"/forward_gi_status.json").c_str(),"wb");if(!f)return;
 std::fprintf(f,"{\"committed\":%llu,\"unavailable\":%llu,\"failed\":%llu,\"reason\":\"%s\",\"source\":[%.6g,%.6g,%.6g,%.6g]}\n",(unsigned long long)committed,(unsigned long long)unavailable,(unsigned long long)failed,reason,lastSource[0],lastSource[1],lastSource[2],lastSource[3]);std::fclose(f);
}
struct Inputs {
 IDirect3DSurface9*target=nullptr;IDirect3DTexture9*source=nullptr,*depth=nullptr;
 ~Inputs(){if(target)target->Release();if(source)source->Release();if(depth)depth->Release();}
};
static void BeforeForward(IDirect3DDevice9*d){
 if(!d||!WosIndirect::enabled||WosIndirect::busy||WosIndirect::rendered||WosIndirect::skipLateGather||WosDeferred::busy||WosDeferred::pending||WosDeferred::unsafeDeviceState)return;
 Inputs input;D3DSURFACE_DESC desc={};
 if(FAILED(d->GetRenderTarget(0,&input.target))||!input.target||FAILED(input.target->GetDesc(&desc))||desc.Format!=D3DFMT_A16B16G16R16F)return;
 WosIndirectCamera::Camera camera;
 if(!WosIndirectCamera::Acquire(desc.Width,desc.Height,camera,input.depth,WosIndirectCamera::CurrentScene()))return;
 if(!WosPrimaryGIProvider::Owned(camera.scene,input.depth))return;
 // Primary scenes must not silently fall back to screen-colour donors later.
 WosIndirect::skipLateGather=true;
 IDirect3DTexture9*normal=nullptr,*rho=nullptr;
 if(!WosPrimaryGIProvider::Get(d,input.depth,camera.scene,normal,rho)){
  ++unavailable;reason="opaque receiver map unavailable before transparency";Status();return;
 }
 if(FAILED(input.target->GetContainer(IID_IDirect3DTexture9,reinterpret_cast<void**>(&input.source)))||!input.source||!WosIndirect::Create(d,desc.Width,desc.Height)){
  ++failed;reason="GI source or targets unavailable";Status();return;
 }
 // Native maps carry actual emitter flux and geometry; no uniform environment
 // fill is added. Existing ambient occlusion remains in the primary resolve.
 const float savedAmbient=WosIndirect::ambient,savedRadius=WosIndirect::radius;WosIndirect::ambient=0;
 // User-approved neutral matte reflectance for unknown materials only.
 // Native material colours remain authoritative where source alpha is one.
 float savedFallback[4];std::memcpy(savedFallback,WosIndirect::nativeFallbackSource,16);
 for(unsigned i=0;i<3;++i)WosIndirect::nativeFallbackSource[i]=WosVolumetric::bounceSunValid?(std::max)(0.f,WosVolumetric::bounceSunColour[i])*.35f:0;
 WosIndirect::nativeFallbackSource[3]=WosVolumetric::bounceSunValid?1.f:0.f;
 WosIndirect::radius=(std::max)(WosIndirect::radius,35.f);
 std::memcpy(lastSource,WosIndirect::nativeFallbackSource,16);
 bool ok=WosIndirect::nativeRsmRender&&WosIndirect::nativeRsmRender(d,input.source,input.depth,camera,normal,rho);
 WosIndirect::ambient=savedAmbient;WosIndirect::radius=savedRadius;std::memcpy(WosIndirect::nativeFallbackSource,savedFallback,16);WosIndirect::lastNativeRsm=ok;
 if(!ok){++unavailable;reason="native emitter maps unavailable before transparency";Status();return;}
 if(FAILED(d->StretchRect(WosIndirect::compositeRT,nullptr,input.target,nullptr,D3DTEXF_NONE))){++failed;reason="GI commit failed";Status();return;}
 WosIndirect::rendered=true;WosIndirect::status="Native geometry GI before transparency";
 ++committed;reason="native geometry bounce committed before transparency";Status();
}
static void Install(){WosDeferred::receiverBeforeForward=BeforeForward;}
}
