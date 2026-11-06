#pragma once
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// F7-only geometry diagnostic: piggybacks on the native shadow draw, never
// replays it. Native color/position stay intact. No diffuse flux or GI here.
// Include after renderer_probe.h and native_indirect.h (for SaveTexture).
namespace WosRSMGeometry {
struct Map {
    IDirect3DSurface9* color=nullptr;IDirect3DSurface9* depth=nullptr;IUnknown* identity=nullptr;
    IDirect3DTexture9* normal=nullptr;IDirect3DSurface9* normalSurface=nullptr;
    uint32_t scene=0;unsigned width=0,height=0,rtId=0,dsId=0,rtFormat=0;
    float vp[16]={};bool isNear=false,invalid=false;
    unsigned attempted=0,draws=0,guardDraws=0,unknown=0,failed=0,vs2=0,vs3=0,vs8=0,clears=0;
    const char*reason="not captured";
};
static std::vector<Map> maps;
static IDirect3DDevice9* owner=nullptr;
static IDirect3DPixelShader9* pixelShader=nullptr;
static IDirect3DVertexShader9* vertexShaders[3]={};
static std::vector<unsigned char> pixelInput,vertexInputs[3];
static IDirect3DPixelShader9* foliageGuards[2]={};
static std::vector<unsigned char> foliagePixelInputs[2],foliageVertexInputs[2];
static bool foliageTried=false,foliageOk=false;
static std::string folder;
static bool loadTried=false,loadOk=false;
static unsigned globalFailures=0,extraMaps=0,mrtSlots=0,noDepthDraws=0;
static const char* status="F7 shadow geometry diagnostic inactive";
static void Invalidate(Map& m,const char* reason,bool unknown=false){if(!m.invalid)m.reason=reason;m.invalid=true;if(unknown)++m.unknown;else ++m.failed;}
static void ReleaseMap(Map& m){if(m.normalSurface)m.normalSurface->Release();if(m.normal)m.normal->Release();if(m.color)m.color->Release();if(m.depth)m.depth->Release();if(m.identity)m.identity->Release();m=Map{};}
static void Reset(){for(auto&m:maps)ReleaseMap(m);maps.clear();if(pixelShader)pixelShader->Release();pixelShader=nullptr;for(auto&v:vertexShaders){if(v)v->Release();v=nullptr;}for(auto&p:foliageGuards){if(p)p->Release();p=nullptr;}for(auto&b:foliagePixelInputs)b.clear();for(auto&b:foliageVertexInputs)b.clear();foliageTried=foliageOk=false;if(owner)owner->Release();owner=nullptr;pixelInput.clear();for(auto&v:vertexInputs)v.clear();folder.clear();loadTried=loadOk=false;globalFailures=extraMaps=mrtSlots=noDepthDraws=0;status="F7 shadow geometry diagnostic inactive";}
static std::vector<unsigned char> ReadFile(const std::string&path){FILE*f=std::fopen(path.c_str(),"rb");if(!f)return {};std::fseek(f,0,SEEK_END);long n=std::ftell(f);std::rewind(f);std::vector<unsigned char>b;if(n>=4&&n<=1024*1024&&!(n%4)){b.resize(size_t(n));if(std::fread(b.data(),1,b.size(),f)!=b.size())b.clear();}std::fclose(f);return b;}
template<class T> static bool ExactShader(T* shader,const std::vector<unsigned char>&reference){if(!shader||reference.empty())return false;UINT n=0;if(FAILED(shader->GetFunction(nullptr,&n))||n!=reference.size())return false;std::vector<unsigned char>b(n);return SUCCEEDED(shader->GetFunction(b.data(),&n))&&n==reference.size()&&b==reference;}
static bool Load(IDirect3DDevice9*d){
    if(loadTried)return loadOk;loadTried=true;D3DCAPS9 caps={};
    if(FAILED(d->GetDeviceCaps(&caps))||caps.NumSimultaneousRTs<2||
       !(caps.PrimitiveMiscCaps&D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS)||
       !(caps.PrimitiveMiscCaps&D3DPMISCCAPS_INDEPENDENTWRITEMASKS)){status="F7 shadow MRT capabilities unavailable";return false;}
    mrtSlots=caps.NumSimultaneousRTs;
    const std::string directory=WosProbe::Root()+"\\rsm_geometry\\";
    pixelInput=ReadFile(directory+"ps_0001.input.cso");auto p=ReadFile(directory+"ps_0001.mrt.cso");
    bool ok=!pixelInput.empty()&&!p.empty()&&SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(p.data()),&pixelShader));
    for(unsigned i=0;i<3&&ok;++i){std::string name=i==0?"vs_0002":i==1?"vs_0003":"vs_0008";vertexInputs[i]=ReadFile(directory+name+".input.cso");auto v=ReadFile(directory+name+".mrt.cso");ok=!vertexInputs[i].empty()&&!v.empty()&&SUCCEEDED(d->CreateVertexShader(reinterpret_cast<const DWORD*>(v.data()),&vertexShaders[i]));}
    if(!ok){status="F7 shadow geometry shader load failed";return false;}
    owner=d;owner->AddRef();loadOk=true;return true;
}
// Exact foliage guards preserve the original VS (including wind/cards), PS
// color and texkill, but zero auxiliary normals where native foliage wins Z.
static bool LoadFoliage(IDirect3DDevice9*d){
    if(foliageTried)return foliageOk;foliageTried=true;
    const std::string directory=WosProbe::Root()+"\\rsm_geometry\\";
    bool ok=true;
    for(unsigned i=0;i<2&&ok;++i){
        const char* p=i?"ps_0006":"ps_0004";const char* v=i?"vs_0007":"vs_0005";
        foliagePixelInputs[i]=ReadFile(directory+p+".input.cso");foliageVertexInputs[i]=ReadFile(directory+v+".input.cso");
        auto bytes=ReadFile(directory+p+".guard.cso");
        ok=!foliagePixelInputs[i].empty()&&!foliageVertexInputs[i].empty()&&!bytes.empty()&&SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(bytes.data()),&foliageGuards[i]));
    }
    foliageOk=ok;return ok;
}
static bool SceneVP(uint32_t scene,float (&vp)[16]){
    uint32_t arena[3]={};size_t used=0;
    if(!WosEngineProbe::Read(WosEngineProbe::imageBase+0xd15c64,arena,sizeof(arena))||!WosEngineProbe::ArenaBounds(arena,used)||scene<arena[0]||uint64_t(scene)+0x4e0>arena[2]||!WosEngineProbe::Read(uintptr_t(scene)+0x190,vp,sizeof(vp)))return false;
    double magnitude=0;for(float v:vp){if(!std::isfinite(v)||std::abs(v)>1e8f)return false;magnitude+=std::abs(v);}return magnitude>1e-8;
}
static void FrameEnd(){
    if(folder.empty())return;
    FILE*f=std::fopen((folder+"\\rsm_geometry.json").c_str(),"wb");
    if(f){std::fprintf(f,"{\"version\":1,\"diagnostic_only\":true,\"flux_available\":false,\"native_clears_observed\":false,\"coverage_scope\":\"observed_depth_writers_only\",\"global_failures\":%u,\"extra_maps\":%u,\"no_depth_draws\":%u,\"maps\":[",globalFailures,extraMaps,noDepthDraws);
        for(size_t i=0;i<maps.size();++i){auto&m=maps[i];const bool valid=!m.invalid&&m.draws>0&&m.normal&&m.clears==1;char filename[80];std::snprintf(filename,sizeof(filename),"rsm_normal_%u.raw",unsigned(i));
            std::fprintf(f,"%s{\"index\":%u,\"scene\":%u,\"shadow_kind\":\"%s\",\"width\":%u,\"height\":%u,\"original_rt_id\":%u,\"original_ds_id\":%u,\"original_rt_format\":%u,\"scene_vp\":[",i?",":"",unsigned(i),m.scene,m.isNear?"near":"far",m.width,m.height,m.rtId,m.dsId,m.rtFormat);
            for(unsigned j=0;j<16;++j)std::fprintf(f,"%s%.9g",j?",":"",m.vp[j]);
            std::fprintf(f,"],\"valid\":%s,\"partial\":%s,\"draws\":%u,\"guard_draws\":%u,\"attempted\":%u,\"unknown\":%u,\"failed\":%u,\"vs2_draws\":%u,\"vs3_draws\":%u,\"vs8_draws\":%u,\"clear_count\":%u,\"reason\":\"%s\",\"normal_file\":\"%s\",",valid?"true":"false","true",m.draws,m.guardDraws,m.attempted,m.unknown,m.failed,m.vs2,m.vs3,m.vs8,m.clears,m.reason,filename);
            WosIndirect::SaveTexture(owner,m.normal,folder+"\\"+filename,f,"normal");std::fputc('}',f);
        }std::fputs("]}\n",f);std::fclose(f);
    }
    Reset();
}
struct DrawScope {
    IDirect3DDevice9*device=nullptr;IDirect3DPixelShader9*originalPS=nullptr;IDirect3DVertexShader9*originalVS=nullptr;
    IDirect3DSurface9*color=nullptr,*depth=nullptr,*originalRT1=nullptr;IUnknown*identity=nullptr;
    Map*map=nullptr;DWORD mask1=0;int variant=-1,guardVariant=-1;bool changed=false,executed=false;
    void Restore(){
        if(!changed)return;bool ok=SUCCEEDED(g_original_set_pixel_shader(device,originalPS));
        ok=SUCCEEDED(device->SetVertexShader(originalVS))&&ok;ok=SUCCEEDED(device->SetRenderTarget(1,originalRT1))&&ok;ok=SUCCEEDED(device->SetRenderState(D3DRS_COLORWRITEENABLE1,mask1))&&ok;changed=false;
        if(!ok&&map)Invalidate(*map,"native draw state restore failed");
    }
    explicit DrawScope(IDirect3DDevice9*d,bool allowed=true){
        // This branch precedes every engine read, D3D query, allocation or I/O.
        if(!allowed||!WosProbe::trace||WosProbe::suppress)return;
        if(folder!=WosProbe::folder){Reset();folder=WosProbe::folder;maps.reserve(2);}
        if(!d)return;
        DWORD zwrite=0,ztest=0;
        if(FAILED(d->GetRenderState(D3DRS_ZWRITEENABLE,&zwrite))||FAILED(d->GetRenderState(D3DRS_ZENABLE,&ztest))){++globalFailures;for(auto&m:maps)Invalidate(m,"depth-writer state query failed");return;}
        if(!zwrite||!ztest)return;
        HRESULT depthResult=d->GetDepthStencilSurface(&depth);
        // Postprocess draws can retain ZENABLE/ZWRITEENABLE with no DS bound.
        // Documented absence cannot change either held shadow-depth surface.
        if(depthResult==D3DERR_NOTFOUND&&!depth){++noDepthDraws;return;}
        if(FAILED(depthResult)||!depth||FAILED(depth->QueryInterface(IID_IUnknown,reinterpret_cast<void**>(&identity)))||!identity){++globalFailures;for(auto&m:maps)Invalidate(m,"depth target identity unavailable");return;}
        for(auto&m:maps)if(m.identity==identity){map=&m;break;}
        auto reject=[&](const char*reason){if(map)Invalidate(*map,reason,true);};
        // WosEngineProbe::enabled is set only by exact executable SHA/signature
        // verification at F7 Begin. An unverified probe cannot start a map.
        auto globals=WosEngineProbe::ReadGlobals();float vp[16]={};
        if(!WosEngineProbe::enabled||globals.valid!=7||!globals.shadowActive||
           (globals.current!=globals.nearScene&&globals.current!=globals.farScene)||!SceneVP(globals.current,vp)){reject("same-depth writer outside verified native shadow scene");return;}
        if(FAILED(d->GetRenderTarget(0,&color))||!color){reject("native color target query failed");return;}
        D3DSURFACE_DESC cd={},dd={};
        if(FAILED(color->GetDesc(&cd))||FAILED(depth->GetDesc(&dd))||cd.Width!=2048||cd.Height!=2048||dd.Width!=cd.Width||dd.Height!=cd.Height||dd.Format!=D3DFORMAT(MAKEFOURCC('I','N','T','Z'))){reject("native shadow target dimensions or INTZ mismatch");return;}
        if(map&&(map->scene!=globals.current||map->color!=color||std::memcmp(map->vp,vp,sizeof(vp)))){reject("same-depth scene color or VP changed");return;}
        if(!map){
            if(maps.size()>=2){++extraMaps;return;}
            Map m;m.color=color;color->AddRef();m.depth=depth;depth->AddRef();m.identity=identity;identity->AddRef();m.scene=globals.current;m.isNear=m.scene==globals.nearScene;m.width=cd.Width;m.height=cd.Height;m.rtId=WosProbe::Id(color);m.dsId=WosProbe::Id(depth);m.rtFormat=unsigned(cd.Format);std::memcpy(m.vp,vp,sizeof(vp));maps.push_back(m);map=&maps.back();
        }
        ++map->attempted;if(map->invalid)return;
        D3DVIEWPORT9 viewport={};DWORD blend=0,srgb=0,stencil=0;
        if(ztest!=D3DZB_TRUE||cd.MultiSampleType!=D3DMULTISAMPLE_NONE||dd.MultiSampleType!=D3DMULTISAMPLE_NONE||
           FAILED(d->GetViewport(&viewport))||viewport.X||viewport.Y||viewport.Width!=cd.Width||viewport.Height!=cd.Height||viewport.MinZ!=0||viewport.MaxZ!=1||
           FAILED(d->GetRenderState(D3DRS_ALPHABLENDENABLE,&blend))||blend||FAILED(d->GetRenderState(D3DRS_SRGBWRITEENABLE,&srgb))||srgb||FAILED(d->GetRenderState(D3DRS_STENCILENABLE,&stencil))||stencil){reject("unsupported native depth-writer state");return;}
        HRESULT r=d->GetRenderTarget(1,&originalRT1);
        if(originalRT1||(FAILED(r)&&r!=D3DERR_NOTFOUND)){reject("native auxiliary render target occupied or unavailable");return;}
        if(FAILED(d->GetRenderState(D3DRS_COLORWRITEENABLE1,&mask1))||FAILED(d->GetPixelShader(&originalPS))||!originalPS||FAILED(d->GetVertexShader(&originalVS))||!originalVS){reject("native shader or mask query failed");return;}
        if(!Load(d)){Invalidate(*map,"geometry shader or MRT capability unavailable");return;}
        if(owner!=d){Invalidate(*map,"capture device changed");return;}
        for(unsigned slot=2;slot<mrtSlots;++slot){IDirect3DSurface9*aux=nullptr;HRESULT h=d->GetRenderTarget(slot,&aux);const bool occupied=aux!=nullptr;if(aux)aux->Release();if(occupied||(FAILED(h)&&h!=D3DERR_NOTFOUND)){reject("native higher auxiliary render target occupied or unavailable");return;}}
        if(ExactShader(originalPS,pixelInput)){
            for(unsigned i=0;i<3;++i)if(ExactShader(originalVS,vertexInputs[i])){variant=int(i);break;}
            if(variant<0){reject("unknown native shadow vertex shader");return;}
        }else{
            if(!LoadFoliage(d)){reject("unknown native shadow pixel shader or foliage guard unavailable");return;}
            int pv=-1,vv=-1;
            for(unsigned i=0;i<2;++i){if(ExactShader(originalPS,foliagePixelInputs[i]))pv=int(i);if(ExactShader(originalVS,foliageVertexInputs[i]))vv=int(i);}
            // Only PS4/VS5, PS6/VS5 and PS6/VS7 were verified. In particular,
            // PS4/VS7 and any native-normal VS paired with foliage stay unknown.
            if(pv<0||vv<0||(pv==0&&vv!=0)){reject("unknown native shadow foliage shader pair");return;}
            guardVariant=pv;
        }
        if(!map->normal){
            bool ok=SUCCEEDED(d->CreateTexture(cd.Width,cd.Height,1,D3DUSAGE_RENDERTARGET,D3DFMT_A16B16G16R16F,D3DPOOL_DEFAULT,&map->normal,nullptr));
            if(ok)ok=SUCCEEDED(map->normal->GetSurfaceLevel(0,&map->normalSurface));
            // ColorFill touches only this new unbound auxiliary surface. It does
            // not change native RT0, depth, viewport, scissor or write masks.
            if(ok)ok=SUCCEEDED(d->ColorFill(map->normalSurface,nullptr,0));
            if(!ok){Invalidate(*map,"auxiliary normal target allocation or clear failed");return;}++map->clears;
        }
        device=d;changed=true;
        bool ok=SUCCEEDED(d->SetRenderTarget(1,map->normalSurface));
        if(ok)ok=SUCCEEDED(d->SetRenderState(D3DRS_COLORWRITEENABLE1,15));
        if(ok&&guardVariant<0)ok=SUCCEEDED(d->SetVertexShader(vertexShaders[variant]));
        if(ok)ok=SUCCEEDED(g_original_set_pixel_shader(d,guardVariant<0?pixelShader:foliageGuards[guardVariant]));
        if(!ok){Invalidate(*map,"diagnostic shader or MRT binding failed");Restore();return;}
        status=guardVariant<0?"F7 native shadow geometric normals captured":"F7 native foliage occluders guarded";
    }
    template<class F> HRESULT Execute(F&& draw){
        if(executed){if(map)Invalidate(*map,"draw scope executed more than once");return D3DERR_INVALIDCALL;}
        executed=true;HRESULT result=draw();
        if(changed&&map){if(SUCCEEDED(result)){if(guardVariant>=0)++map->guardDraws;else {++map->draws;if(variant==0)++map->vs2;else if(variant==1)++map->vs3;else ++map->vs8;}if(!map->invalid)map->reason=map->guardDraws?"exact native geometry and guarded foliage captured":"exact native geometry captured";}else Invalidate(*map,"native draw failed");}
        Restore();return result;
    }
    ~DrawScope(){if(changed&&!executed&&map)Invalidate(*map,"draw completion not reported");Restore();if(originalPS)originalPS->Release();if(originalVS)originalVS->Release();if(originalRT1)originalRT1->Release();if(color)color->Release();if(depth)depth->Release();if(identity)identity->Release();}
};
}
