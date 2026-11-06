#pragma once
// Four dynamic sun-shadow cascades consumed per pixel by the deferred resolve.
//
// The cascade fitting and the light-space replay of the native far-shadow queue
// already exist in WosCascadeProbe and are unchanged here. This module owns the
// deferred half of the feature:
//   * one INTZ atlas of 2x2 tiles, so the resolve needs a single sampler and no
//     per-tap branch over four maps,
//   * the camera-relative shadow matrices the resolve reconstructs positions in,
//   * the PCSS parameters, persistence and status.
//
// Every cascade is refreshed every frame, animated casters included; the F11
// hybrid could stagger updates only because the native shadow hid the stale
// silhouette, and a per-pixel consumer cannot.
//
// Caster coverage is exactly what the native far-shadow queue submits through
// the two verified vertex-shader paths. Objects the game never submits there are
// absent from these maps, which is why the default mode combines the cascade
// term with the native one by min() and can only add shadow.
#include "shadow_work_cache.h"
namespace WosCascadeShadow {

// User settings, persisted through WosDeferred::SaveSettings.
static bool enabled=false;
// 0 = off, 1 = min(native, cascade), 2 = cascade replaces the native term.
static int mode=1;
static int resolution=1024;          // tile edge; the atlas is 2x2 tiles
static float distance=150.f;         // end of the last cascade, view units
static float sunAngle=.02f;          // penumbra width per unit of blocker distance
static float searchDistance=25.f;    // longest blocker distance the search covers
static float depthBias=.06f;         // world units along the light direction
static float normalBias=1.5f;        // shadow texels along the surface normal
static float strength=1.f;
// Width of the dissolve that replaces a hard line at a cascade boundary,
// in cascade clip units. Zero makes the selection deterministic.
static float boundaryDither=.06f;
// Far-cascade refresh stagger, 0..2; the schedule lives in WosCascadeProbe.
static int stagger=1;
// GTA SA district casters from SA_CollisionLoader (inert without that DLL).
static bool saCasters=true;
static unsigned staggerUpdated=4,staggerForced=0;

// Host seam: the deferred runtime publishes its busy flag so that resource
// creation inside our own frame work is not mistaken for an engine asset.
static bool* hostBusy=nullptr;
// Set to the deferred renderer's own enable flag: with no resolve running
// nothing consumes the maps, so the extra shadow pass is not worth paying.
static bool* hostActive=nullptr;
struct HostScope {
    bool previous=false;
    HostScope(){if(hostBusy){previous=*hostBusy;*hostBusy=true;}}
    ~HostScope(){if(hostBusy)*hostBusy=previous;}
};

static IDirect3DDevice9* device=nullptr;
static IDirect3DTexture9* atlas=nullptr;
static IDirect3DSurface9* atlasDepth=nullptr;
static IDirect3DSurface9* atlasColor=nullptr;
static unsigned tile=0;

// Matrices describing the atlas content. Latched when the first replayed draw
// of a frame reaches the atlas, so they always describe what the atlas holds.
static bool mapsValid=false;
static float absoluteRows[4][12];    // per cascade: three dot-product rows, world space
static float texelWorld[4]={},depthRange[4]={},uvPerWorld[4]={};
static uint32_t mapsScene=0;
// Eye the cascades were fitted for. The plan is absolute world space and
// is rebased at bind time, so a frame of drift is still geometrically
// correct; this only rejects a genuinely different camera.
static double mapsEye[3]={};
static float eyeTolerance=25.f;
static bool pendingValid=false;
static float pendingRows[4][12];
static float pendingTexel[4]={},pendingRange[4]={},pendingUv[4]={};
static uint32_t pendingScene=0;
static double pendingEye[3]={};

static unsigned frameRenders=0,frameBinds=0,binds=0,sceneSkips=0,failures=0,lastRenders=0,lastBinds=0;
// Replay statistics pushed in by the integration each frame; they belong
// to the probe, which this header deliberately does not depend on.
static unsigned sourceDraws=0,skippedShader=0,skippedInstance=0;
static unsigned probeReady=0,probeFailed=0,probeCalls=0,probeEntries=0;
static const char* probeStatus="";
static uint32_t lastCameraScene=0;
static double lastEyeDistance=-1;
static const char* status="Cascade shadows OFF";

static void ReleaseAtlas(){
    if(atlasDepth){atlasDepth->Release();atlasDepth=nullptr;}
    if(atlasColor){atlasColor->Release();atlasColor=nullptr;}
    if(atlas){atlas->Release();atlas=nullptr;}
    if(device){device->Release();device=nullptr;}
    tile=0;mapsValid=pendingValid=false;
}
static void Reset(){ReleaseAtlas();frameRenders=lastRenders=frameBinds=lastBinds=binds=sceneSkips=failures=0;}

static bool Create(IDirect3DDevice9* d,unsigned request){
    if(!d||request<64||request>4096)return false;
    if(device==d&&atlas&&tile==request)return true;
    HostScope scope;
    D3DCAPS9 caps={};if(FAILED(d->GetDeviceCaps(&caps)))return false;
    const unsigned edge=request*2;
    if(edge>caps.MaxTextureWidth||edge>caps.MaxTextureHeight)return false;
    IDirect3DTexture9* texture=nullptr;IDirect3DSurface9* depth=nullptr;IDirect3DSurface9* color=nullptr;
    bool ok=SUCCEEDED(d->CreateTexture(edge,edge,1,D3DUSAGE_DEPTHSTENCIL,
        static_cast<D3DFORMAT>(MAKEFOURCC('I','N','T','Z')),D3DPOOL_DEFAULT,&texture,nullptr));
    if(ok)ok=SUCCEEDED(texture->GetSurfaceLevel(0,&depth));
    if(ok)ok=SUCCEEDED(d->CreateRenderTarget(edge,edge,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&color,nullptr));
    if(!ok){if(color)color->Release();if(depth)depth->Release();if(texture)texture->Release();++failures;return false;}
    ReleaseAtlas();
    device=d;d->AddRef();atlas=texture;atlasDepth=depth;atlasColor=color;tile=request;
    return true;
}

// Render-target binding for the atlas. Stateblocks do not restore targets, so
// every slot, the depth surface and the viewport are saved explicitly.
struct Binding {
    IDirect3DDevice9* d=nullptr;IDirect3DSurface9* saved[4]={},*ds=nullptr;
    D3DVIEWPORT9 viewport={};unsigned slots=0;bool active=false;
    bool Begin(IDirect3DDevice9* dev){
        if(active||!dev||!atlasColor||!atlasDepth)return false;
        if(FAILED(dev->GetViewport(&viewport)))return false;
        slots=WosShadowWork::RenderTargetSlots(dev);if(!slots)return false;
        d=dev;
        for(unsigned i=0;i<slots;++i){
            HRESULT hr=d->GetRenderTarget(i,&saved[i]);
            if(FAILED(hr)&&hr!=D3DERR_NOTFOUND){for(auto& s:saved){if(s)s->Release();s=nullptr;}d=nullptr;return false;}
        }
        HRESULT hr=d->GetDepthStencilSurface(&ds);
        if(FAILED(hr)&&hr!=D3DERR_NOTFOUND){for(auto& s:saved){if(s)s->Release();s=nullptr;}d=nullptr;return false;}
        active=true;
        bool ok=true;
        for(unsigned i=1;i<slots;++i)if(saved[i])ok=SUCCEEDED(d->SetRenderTarget(i,nullptr))&&ok;
        ok=SUCCEEDED(d->SetRenderTarget(0,atlasColor))&&ok;
        ok=SUCCEEDED(d->SetDepthStencilSurface(atlasDepth))&&ok;
        if(!ok){End();return false;}
        return true;
    }
    bool Tile(unsigned index)const{
        if(!active||index>=4||!tile)return false;
        D3DVIEWPORT9 target={(index&1)*tile,(index>>1)*tile,tile,tile,0,1};
        return SUCCEEDED(d->SetViewport(&target));
    }
    bool Whole()const{
        if(!active||!tile)return false;
        D3DVIEWPORT9 target={0,0,tile*2,tile*2,0,1};
        return SUCCEEDED(d->SetViewport(&target));
    }
    bool End(){
        if(!active)return true;
        bool ok=true;
        for(unsigned i=0;i<slots;++i)if(i==0||saved[i])ok=SUCCEEDED(d->SetRenderTarget(i,saved[i]))&&ok;
        ok=SUCCEEDED(d->SetDepthStencilSurface(ds))&&ok;
        ok=SUCCEEDED(d->SetViewport(&viewport))&&ok;
        for(auto& s:saved){if(s)s->Release();s=nullptr;}
        if(ds){ds->Release();ds=nullptr;}
        active=false;d=nullptr;return ok;
    }
    ~Binding(){End();}
};
static Binding replayBinding;

// Pack one cascade of the current plan. The plan matrices use the engine's row
// vector convention; the resolve wants dot-product rows. The translation is
// rebased at bind time, not here, because the resolve works in camera-relative
// world space and the origin changes every frame.
static void Latch(const WosCascades::Plan& plan,uint32_t scene,const double eye[3]){
    pendingValid=false;
    for(unsigned i=0;i<4;++i){
        const auto& c=plan.cascades[i];
        for(unsigned row=0;row<3;++row){
            pendingRows[i][row*4+0]=c.vp.v[0*4+row];
            pendingRows[i][row*4+1]=c.vp.v[1*4+row];
            pendingRows[i][row*4+2]=c.vp.v[2*4+row];
            pendingRows[i][row*4+3]=c.vp.v[3*4+row];
        }
        double texel=c.texelWorld,range=c.depthMax-c.depthMin;
        if(!(texel>1e-6)||!(range>1e-6)||!tile)return;
        pendingTexel[i]=float(texel);
        pendingRange[i]=float(range);
        // Cascade clip x,y in [-1,1] cover one tile, which is half the atlas.
        pendingUv[i]=float(.5/(texel*double(tile)));
        for(unsigned k=0;k<12;++k)if(!std::isfinite(pendingRows[i][k]))return;
    }
    pendingScene=scene;
    for(unsigned i=0;i<3;++i)pendingEye[i]=eye[i];
    pendingValid=true;
}
static void Promote(){
    if(!pendingValid)return;
    memcpy(absoluteRows,pendingRows,sizeof(absoluteRows));
    memcpy(texelWorld,pendingTexel,sizeof(texelWorld));
    memcpy(depthRange,pendingRange,sizeof(depthRange));
    memcpy(uvPerWorld,pendingUv,sizeof(uvPerWorld));
    mapsScene=pendingScene;for(unsigned i=0;i<3;++i)mapsEye[i]=pendingEye[i];mapsValid=true;
}

// Sink entry points. The integration binds these to WosCascadeProbe.
// updateMask names the tiles replayed this frame; the others keep last
// frame's depth and the plan already carries their held matrices.
static unsigned lastUpdateMask=15;
static bool Prepare(IDirect3DDevice9* d,unsigned request,const WosCascades::Plan& plan,uint32_t scene,const double eye[3],unsigned updateMask=15){
    if(!enabled)return false;
    updateMask&=15u;
    const bool partial=updateMask!=15u;
    const bool existed=device==d&&atlas&&tile==request;
    if(!Create(d,request))return false;
    // A new atlas holds no held tiles; failing makes the probe refresh all four next frame.
    if(partial&&!existed)return false;
    HostScope scope;Binding binding;
    if(!binding.Begin(d)){++failures;return false;}
    bool ok=true;
    if(!partial)ok=binding.Whole()&&SUCCEEDED(d->Clear(0,nullptr,D3DCLEAR_ZBUFFER,0,1.f,0));
    else for(unsigned i=0;i<4&&ok;++i)if(updateMask&(1u<<i))ok=binding.Tile(i)&&SUCCEEDED(d->Clear(0,nullptr,D3DCLEAR_ZBUFFER,0,1.f,0));
    ok=binding.End()&&ok;
    if(!ok){++failures;return false;}
    lastUpdateMask=updateMask;
    // A full refresh leaves the atlas empty, so its matrices are published only
    // once a replayed draw reaches it. A partial one still holds valid tiles,
    // and a refreshed tile no caster reaches is correctly empty: publish now,
    // or a frame where the near cascades see no caster would drop every shadow.
    mapsValid=false;Latch(plan,scene,eye);
    if(partial)Promote();
    return true;
}
static bool BeginReplay(IDirect3DDevice9* d){
    if(!enabled||!atlasDepth)return false;
    if(!replayBinding.Begin(d)){++failures;return false;}
    return true;
}
static bool SelectTile(unsigned index){
    if(!replayBinding.Tile(index))return false;
    if(!mapsValid)Promote();
    ++frameRenders;return true;
}
static bool EndReplay(){
    bool ok=replayBinding.End();
    if(!ok)++failures;
    return ok;
}
// Whether the cascades should be rendered at all this frame: the maps are
// worth their shadow pass only while a resolve is there to consume them.
static bool Armed(){return enabled&&mode>0&&hostActive&&*hostActive;}
static unsigned TileRequest(){return unsigned(resolution<64?64:(resolution>2048?2048:resolution));}
static void FrameEnd(){
    lastRenders=frameRenders;frameRenders=0;lastBinds=frameBinds;frameBinds=0;
    if(!enabled||mode<=0){status="Cascade shadows OFF";return;}
    if(!atlas)status="Cascade shadows: atlas unavailable";
    else if(!lastRenders)status="Cascade shadows: no replayed casters this frame";
    else if(!lastBinds)status="Cascade shadows: maps ready, resolve not consuming";
    else status=mode==2?"Four dynamic cascades, PCSS replaces native shadow":"Four dynamic cascades, PCSS combined with native";
}

// Bind the atlas and the shadow constants for the resolve. Returns false when
// the resolve must keep the native shadow term, which the shader does whenever
// the mode constant is zero.
static bool Bind(IDirect3DDevice9* d,const double origin[3],uint32_t scene){
    // -2 means replacement requested but no valid atlas: unshadowed, never native.
    float off[4]={0,0,0,(enabled&&mode==2)?-2.f:0.f};
    if(!enabled||mode<=0||!mapsValid||!atlas||d!=device){
        d->SetPixelShaderConstantF(26,off,1);return false;
    }
    lastCameraScene=scene;
    double dx=origin[0]-mapsEye[0],dy=origin[1]-mapsEye[1],dz=origin[2]-mapsEye[2];
    lastEyeDistance=std::sqrt(dx*dx+dy*dy+dz*dz);
    if(!std::isfinite(lastEyeDistance)||lastEyeDistance>eyeTolerance){
        ++sceneSkips;d->SetPixelShaderConstantF(26,off,1);return false;
    }
    float rows[48]={};
    for(unsigned i=0;i<4;++i)for(unsigned row=0;row<3;++row){
        const float* r=absoluteRows[i]+row*4;
        // Camera-relative: clip = r . (p + origin, 1) = r.xyz . p + (r.w + r.xyz . origin)
        double translation=double(r[3])+double(r[0])*origin[0]+double(r[1])*origin[1]+double(r[2])*origin[2];
        if(!std::isfinite(translation)||std::fabs(translation)>1e9){++failures;d->SetPixelShaderConstantF(26,off,1);return false;}
        float* out=rows+(i*3+row)*4;
        out[0]=r[0];out[1]=r[1];out[2]=r[2];out[3]=float(translation);
    }
    float params[4]={sunAngle,searchDistance,strength,float(mode)};
    float bias[4]={depthBias,normalBias,tile?.5f/float(tile*2):0.f,boundaryDither};
    bool ok=SUCCEEDED(d->SetPixelShaderConstantF(11,rows,12));
    ok=SUCCEEDED(d->SetPixelShaderConstantF(23,texelWorld,1))&&ok;
    ok=SUCCEEDED(d->SetPixelShaderConstantF(24,depthRange,1))&&ok;
    ok=SUCCEEDED(d->SetPixelShaderConstantF(25,uvPerWorld,1))&&ok;
    ok=SUCCEEDED(d->SetPixelShaderConstantF(26,params,1))&&ok;
    ok=SUCCEEDED(d->SetPixelShaderConstantF(27,bias,1))&&ok;
    ok=SUCCEEDED(d->SetTexture(7,atlas))&&ok;
    if(ok){
        const D3DSAMPLERSTATETYPE types[6]={D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER,D3DSAMP_MIPFILTER,
                                            D3DSAMP_ADDRESSU,D3DSAMP_ADDRESSV,D3DSAMP_SRGBTEXTURE};
        const DWORD values[6]={D3DTEXF_POINT,D3DTEXF_POINT,D3DTEXF_NONE,D3DTADDRESS_CLAMP,D3DTADDRESS_CLAMP,0};
        for(unsigned i=0;i<6;++i)ok=SUCCEEDED(d->SetSamplerState(7,types[i],values[i]))&&ok;
    }
    if(!ok){++failures;d->SetPixelShaderConstantF(26,off,1);d->SetTexture(7,nullptr);return false;}
    ++binds;++frameBinds;return true;
}
static void Unbind(IDirect3DDevice9* d){if(d)d->SetTexture(7,nullptr);}
}
