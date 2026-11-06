#pragma once
// Volumetric sun shafts for the deferred renderer.
//
// The pass is a consumer of the cascade atlas, nothing more: it renders no
// geometry, owns no shadow map, and reads the same INTZ scene depth the
// resolve reads. Its two draws happen once per frame, at the native bloom
// composite, because that is the only point where the scene HDR target is
// complete and still linear. The resolve cannot host them: it runs once per
// light batch, it clips every pixel it does not own, and the sky it would
// have to draw shafts against is exactly the set of pixels it discards.
//
// Both draws are additive into the game's own HDR texture with alpha masked
// off. Nothing else in the frame is touched, so a failure leaves the original
// image rather than a half-lit one; the counters below say which failure it
// was, and the feature disables itself for the session after a restore fault.
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/deferred_cascade_shadow.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/deferred_local_shadow.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
namespace WosVolumetric {

// User settings, persisted through WosDeferred::SaveSettings.
static bool enabled=false;
static int steps=24;                 // march samples per half-resolution pixel
static float distance=220.f;         // longest march, world units
static float density=.012f;          // extinction and scattering per world unit
static float strength=1.f;           // artistic multiplier on the composite
static float anisotropy=.72f;        // Henyey-Greenstein g, forward scattering
static float rejection=.35f;         // upsample distance rejection per world unit
static bool fogEnabled=false; // atmosphere, independent of shaft density
static bool fogHeightEnabled=false;
static float fogBaseHeight=0.f, fogHeightFalloff=.012f;
static float fogDensity=.0008f, fogStart=20.f, fogDistance=600.f;
static float fogColour[3]={.25f,.32f,.40f}; // linear HDR ambient in-scattering
static int debugView=0;              // 0 normal, 1 shafts alone

// Light shafts of local point lights, marched in the same half-resolution
// pass. The engine's lights are points (position, range, RGB) with no
// direction, so the medium around each one glows with a closed-form
// 1/(1+(d/core)^2) density, shifted to reach zero at the light's radius. The
// lights that own a cube shadow map this frame are marched through it instead
// (equiangular samples), so their housings and anything in front cut the glow
// into beams. No geometry, no extra draw.
static bool localEnabled=false;      // [Volumetric] LocalEnabled
static int localMaxLights=16;        // lights per frame, 1..24
static float localDensity=.03f;      // scattering per world unit
static float localStrength=1.f;      // artistic multiplier
static float localCore=2.f;          // world radius of the bright core
static float localRadiusScale=1.f;   // multiplier on the native range
static int localShadowSteps=16;      // samples through a cube map, 0 = analytic only
static float localMaxDistance=250.f; // lights farther than this are skipped
static constexpr unsigned kShaftLights=24;
struct ShaftLight {double p[3];float radius,colour[3],slot,score;};
static ShaftLight shaftLights[kShaftLights];
static unsigned shaftCount=0,shaftShadowed=0,shaftObserved=0;
static unsigned lastShaftCount=0,lastShaftShadowed=0,lastShaftObserved=0,localFrames=0,lastLocalFrames=0;

// Sun published by the deferred runtime from the light table it already builds.
// Direction points toward the sun in world axes; colour is the native radiance.
static float sunDirection[3]={0,0,0};
static float sunColour[3]={0,0,0};
static bool sunValid=false;

// Host seam, mirroring the cascade consumer: resource creation inside our own
// frame work must not be mistaken for an engine asset by the native registries.
static bool* hostBusy=nullptr;
struct HostScope {
    bool previous=false;
    HostScope(){if(hostBusy){previous=*hostBusy;*hostBusy=true;}}
    ~HostScope(){if(hostBusy)*hostBusy=previous;}
};

static std::string directory;        // asset folder, set by the host at load
static IDirect3DDevice9* device=nullptr;
static IDirect3DTexture9* marchTexture=nullptr;
static IDirect3DSurface9* marchSurface=nullptr;
static IDirect3DPixelShader9* marchShader=nullptr;
static IDirect3DPixelShader9* compositeShader=nullptr;
static unsigned width=0,height=0,marchWidth=0,marchHeight=0;
static bool loadAttempted=false,faulted=false;
static unsigned frames=0,lastFrames=0,failures=0,noCascade=0,noSun=0,restoreFailures=0;
static const char* status="Volumetric sun shafts OFF";
static bool skyGlowReady=false;      // this frame's march holds lamp glow

template<class T> static void ReleasePointer(T*& p){if(p){p->Release();p=nullptr;}}
static void ReleaseTargets(){
    ReleasePointer(marchSurface);ReleasePointer(marchTexture);
    width=height=marchWidth=marchHeight=0;
}
static void Reset(){
    ReleaseTargets();ReleasePointer(marchShader);ReleasePointer(compositeShader);
    ReleasePointer(device);loadAttempted=false;faulted=false;
    frames=lastFrames=failures=noCascade=noSun=restoreFailures=0;
    shaftCount=shaftShadowed=shaftObserved=lastShaftCount=lastShaftShadowed=lastShaftObserved=localFrames=lastLocalFrames=0;
    sunValid=false;status="Volumetric sun shafts OFF";
}
static std::vector<unsigned char> ReadFile(const std::string& path){
    FILE* f=std::fopen(path.c_str(),"rb");if(!f)return{};
    std::fseek(f,0,SEEK_END);long n=std::ftell(f);std::rewind(f);
    std::vector<unsigned char> bytes;
    if(n>0&&n<4194304){bytes.resize(size_t(n));if(std::fread(bytes.data(),1,bytes.size(),f)!=bytes.size())bytes.clear();}
    std::fclose(f);return bytes;
}
// A shader file is accepted only as a whole ps_3_0 token stream ending in END;
// a truncated asset must fail here, not inside the driver.
static bool Tokens(const std::vector<unsigned char>& bytes){
    if(bytes.size()<8||bytes.size()%4)return false;
    uint32_t first=0,last=0;
    std::memcpy(&first,bytes.data(),4);std::memcpy(&last,bytes.data()+bytes.size()-4,4);
    return first==0xffff0300u&&last==0x0000ffffu;
}
static bool Load(IDirect3DDevice9* d){
    if(!d)return false;
    if(device&&device!=d)Reset();
    if(loadAttempted)return device==d&&marchShader&&compositeShader;
    loadAttempted=true;
    HostScope scope;
    auto create=[&](const char* name,IDirect3DPixelShader9*& out){
        auto bytes=ReadFile(directory+"\\"+name);
        return Tokens(bytes)&&SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(bytes.data()),&out));
    };
    if(!create("deferred_volumetric.cso",marchShader)||!create("deferred_volumetric_composite.cso",compositeShader)){
        ReleasePointer(marchShader);ReleasePointer(compositeShader);
        status="Volumetric shaders unavailable";return false;
    }
    device=d;d->AddRef();return true;
}
static bool Targets(IDirect3DDevice9* d,unsigned w,unsigned h){
    if(width==w&&height==h&&marchSurface)return true;
    ReleaseTargets();HostScope scope;
    const unsigned mw=(w+1)/2,mh=(h+1)/2;
    bool ok=mw&&mh&&SUCCEEDED(d->CreateTexture(mw,mh,1,D3DUSAGE_RENDERTARGET,D3DFMT_A16B16G16R16F,
                                               D3DPOOL_DEFAULT,&marchTexture,nullptr));
    if(ok)ok=SUCCEEDED(marchTexture->GetSurfaceLevel(0,&marchSurface));
    if(!ok){ReleaseTargets();status="Volumetric target allocation failed";return false;}
    width=w;height=h;marchWidth=mw;marchHeight=mh;return true;
}
// Pick the sun for this frame out of the light table the deferred runtime has
// already interned. Every light set in an outdoor scene carries the same
// directional light at a different intensity, so the brightest one is the one
// whose shafts a viewer expects to see. Row zero is the reserved uncovered
// row and never holds a light. A table with no directional light at all is
// reported rather than replaced with an invented direction.
static void ObserveLights(const std::vector<float>& rows,unsigned rowFloats){
    sunValid=false;
    if(!rowFloats||rows.size()<rowFloats||rows.size()%rowFloats)return;
    const size_t count=rows.size()/rowFloats;
    double best=-1;const float* chosen=nullptr;
    for(size_t row=1;row<count;++row){
        const float* texels=rows.data()+row*rowFloats;
        const double lengthSquared=double(texels[0])*texels[0]+double(texels[1])*texels[1]+double(texels[2])*texels[2];
        if(!(lengthSquared>1e-12))continue;                       // baked or unlit set
        const double luminance=.2126*double(texels[4])+.7152*double(texels[5])+.0722*double(texels[6]);
        if(!std::isfinite(luminance)||luminance<=best)continue;
        best=luminance;chosen=texels;
    }
    if(!chosen)return;
    const double lengthSquared=double(chosen[0])*chosen[0]+double(chosen[1])*chosen[1]+double(chosen[2])*chosen[2];
    const double scale=1.0/std::sqrt(lengthSquared);
    float direction[3]={},colour[3]={};
    for(unsigned i=0;i<3;++i){
        direction[i]=float(chosen[i]*scale);
        colour[i]=chosen[4+i];
        if(!std::isfinite(direction[i])||!std::isfinite(colour[i]))return;
    }
    std::memcpy(sunDirection,direction,sizeof(direction));
    std::memcpy(sunColour,colour,sizeof(colour));
    sunValid=true;
}
static bool SunArmed(){return enabled&&WosCascadeShadow::enabled&&WosCascadeShadow::mode>0;}
static bool Armed(){return !faulted&&(SunArmed()||localEnabled);}
// Picks this frame's shaft lights out of the lights the primary draws have
// reported so far; the bloom draw comes after every one of them. The list is
// ranked again for the current camera, lights whose sphere lies wholly behind
// it are dropped, and the lights that own a cube map this frame take the
// cube's exact position so the march addresses the faces they were drawn with.
static void SelectLights(const double origin[3],const float inverse[16]){
    shaftCount=shaftShadowed=0;
    const auto&list=WosLocalShadow::observed;
    shaftObserved=unsigned(list.size());
    if(!localEnabled||list.empty())return;
    // Camera forward from the inverse view-projection: NDC depth 0 to 1 at
    // the screen centre. Positions here are camera-relative.
    float forward[3]={0,0,0};
    {
        const double w0=inverse[15],w1=double(inverse[14])+inverse[15];
        double length=0;
        if(std::abs(w0)>1e-12&&std::abs(w1)>1e-12)
            for(int i=0;i<3;++i){
                const double a=double(inverse[i*4+3])/w0,b=(double(inverse[i*4+2])+inverse[i*4+3])/w1;
                forward[i]=float(b-a);length+=double(forward[i])*forward[i];
            }
        if(length>1e-12){const double s=1/std::sqrt(length);for(float&f:forward)f=float(f*s);}
        else forward[0]=forward[1]=forward[2]=0;
    }
    const unsigned limit=unsigned((std::min)((std::max)(localMaxLights,1),int(kShaftLights)));
    std::vector<ShaftLight> pick;pick.reserve(list.size());
    for(const auto&l:list){
        ShaftLight s={};
        double rel[3],dist2=0;
        for(int j=0;j<3;++j){rel[j]=l.p[j]-origin[j];dist2+=rel[j]*rel[j];s.p[j]=l.p[j];s.colour[j]=l.colour[j];}
        {const float gain=WosNightLamps::ShaftGain(l.p);for(int j=0;j<3;++j)s.colour[j]*=gain;}
        const double dist=std::sqrt(dist2);
        s.radius=(std::min)(l.range*localRadiusScale,60.f);
        if(!(s.radius>.05f)||!std::isfinite(dist)||dist-s.radius>localMaxDistance)continue;
        if(rel[0]*forward[0]+rel[1]*forward[1]+rel[2]*forward[2]<-s.radius)continue;
        const float peak=(std::max)(s.colour[0],(std::max)(s.colour[1],s.colour[2]));
        if(!(peak>0))continue;
        s.score=float(peak*s.radius*s.radius/(s.radius*s.radius+dist2));
        s.slot=-1;
        pick.push_back(s);
    }
    const size_t keep=(std::min)(pick.size(),size_t(limit));
    std::partial_sort(pick.begin(),pick.begin()+keep,pick.end(),[](const ShaftLight&a,const ShaftLight&b){return a.score>b.score;});
    const bool cubes=WosLocalShadow::enabled&&WosLocalShadow::valid&&localShadowSteps>0;
    for(size_t i=0;i<keep;++i){
        ShaftLight s=pick[i];
        if(cubes)for(unsigned c=0;c<WosLocalShadow::count&&c<2;++c){
            const auto&owner=WosLocalShadow::lights[c];
            double d=0;for(int j=0;j<3;++j)d+=(owner.p[j]-s.p[j])*(owner.p[j]-s.p[j]);
            if(d<.0001){std::memcpy(s.p,owner.p,sizeof(s.p));s.slot=float(c);++shaftShadowed;break;}
        }
        shaftLights[shaftCount++]=s;
    }
}
// hdr is the game's own linear scene texture; depth is the INTZ it was drawn
// with. Both belong to the caller and are neither retained nor resized here.
static bool Render(IDirect3DDevice9* d,IDirect3DTexture9* hdr,IDirect3DTexture9* depth,
                   const float relativeInverse[16],const double origin[3],uint32_t scene){
    if(!Armed()||!d||!hdr||!depth||!relativeInverse||!origin)return false;
    const bool wantSun=SunArmed()&&sunValid;
    if(SunArmed()&&!sunValid){++noSun;status="Volumetric: no directional light in the current light table";}
    SelectLights(origin,relativeInverse);
    if(!wantSun&&!shaftCount)return false;
    D3DSURFACE_DESC desc={};
    if(FAILED(hdr->GetLevelDesc(0,&desc))||desc.Format!=D3DFMT_A16B16G16R16F||
       desc.Width<64||desc.Height<64||desc.Width>4096||desc.Height>4096)return false;
    if(!Load(d)||!Targets(d,desc.Width,desc.Height))return false;
    IDirect3DSurface9* target=nullptr;
    if(FAILED(hdr->GetSurfaceLevel(0,&target)))return false;
    HostScope busy;
    WosIndirect::State state(d);
    bool ok=state.valid;
    auto check=[&](HRESULT hr){ok=SUCCEEDED(hr)&&ok;};
    if(ok){
        check(d->SetDepthStencilSurface(nullptr));
        for(unsigned i=1;i<state.slots;++i)check(d->SetRenderTarget(i,nullptr));
        check(d->SetVertexShader(nullptr));check(d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1));
        // Native wrapping changes fullscreen UV interpolation even for XYZRHW.
        check(d->SetRenderState(D3DRS_WRAP0,0));
        for(auto rs:{D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHABLENDENABLE,D3DRS_ALPHATESTENABLE,
                     D3DRS_STENCILENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_SRGBWRITEENABLE,D3DRS_FOGENABLE,
                     D3DRS_CLIPPLANEENABLE,D3DRS_SEPARATEALPHABLENDENABLE})check(d->SetRenderState(rs,0));
        check(d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));check(d->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID));
        check(d->SetRenderState(D3DRS_COLORWRITEENABLE,15));check(d->SetRenderState(D3DRS_MULTISAMPLEMASK,0xffffffff));
        check(d->SetRenderState(D3DRS_DITHERENABLE,FALSE));
        // Slot 0 carries the half-resolution shafts, slot 4 the scene depth.
        // Both stay point-sampled: the composite weighs its own four taps by
        // the distance in their alpha, and a bilinear tap would blend those
        // distances into a value no ray ever had.
        const unsigned samplers[2]={0,4};
        for(unsigned index=0;index<2;++index){
            const unsigned slot=samplers[index];
            check(d->SetSamplerState(slot,D3DSAMP_MINFILTER,D3DTEXF_POINT));
            check(d->SetSamplerState(slot,D3DSAMP_MAGFILTER,D3DTEXF_POINT));
            check(d->SetSamplerState(slot,D3DSAMP_MIPFILTER,D3DTEXF_NONE));
            check(d->SetSamplerState(slot,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));
            check(d->SetSamplerState(slot,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));
            check(d->SetSamplerState(slot,D3DSAMP_SRGBTEXTURE,0));
        }
        check(d->SetPixelShaderConstantF(0,relativeInverse,4));
        check(d->SetTexture(4,depth));
    }
    // The cascade rows, the atlas and the depth bias are the resolve's, bound
    // by the same code path. A false return means the maps do not describe the
    // current camera, and the frame gets no sun shafts; the lamps still glow.
    const bool bound=ok&&wantSun&&WosCascadeShadow::Bind(d,origin,scene);
    if(ok&&wantSun&&!bound){++noCascade;status="Volumetric: cascade maps do not match the current camera";}
    const bool local=shaftCount>0;
    const bool draw=ok&&(bound||local);
    if(draw){
        const float march[4]={distance,anisotropy,marchWidth?1.f/float(marchWidth):0.f,
                              marchHeight?1.f/float(marchHeight):0.f};
        // Zero sun steps is the shader's "no sun" case: the cascade registers
        // are not bound then and must not be read.
        const float sun[4]={sunDirection[0],sunDirection[1],sunDirection[2],bound?float(steps):0.f};
        const float scatter[4]={sunColour[0],sunColour[1],sunColour[2],density};
        const float composite[4]={float(marchWidth),float(marchHeight),strength,rejection};
        check(d->SetPixelShaderConstantF(28,sun,1));
        check(d->SetPixelShaderConstantF(29,scatter,1));
        check(d->SetPixelShaderConstantF(30,march,1));
        check(d->SetPixelShaderConstantF(31,composite,1));
        const float fog[4]={fogEnabled?fogDensity:0.f,fogStart,fogDistance,0.f};
        const float fogAmbient[4]={fogColour[0],fogColour[1],fogColour[2],0.f};
        check(d->SetPixelShaderConstantF(32,fog,1));
        check(d->SetPixelShaderConstantF(33,fogAmbient,1));
        const float fogHeight[4]={float(origin[1]-double(fogBaseHeight)),fogHeightEnabled?fogHeightFalloff:0.f,0.f,0.f};
        check(d->SetPixelShaderConstantF(34,fogHeight,1));
        // Lamps: c40 parameters, c41.. positions, c65.. colours. Colour is
        // premultiplied by density and strength; w is the cube slot or -1.
        // The sun's composite strength also scales the lamps, so a lamp's
        // own strength is divided by it here.
        const float lampScale=localDensity*localStrength/(std::max)(strength,1e-3f);
        const float params[4]={float(shaftCount),float((std::min)((std::max)(localShadowSteps,0),48)),
                               (std::max)(localCore,.05f),0.f};
        float positions[kShaftLights*4]={},colours[kShaftLights*4]={};
        for(unsigned i=0;i<shaftCount;++i){
            const ShaftLight&s=shaftLights[i];
            for(int j=0;j<3;++j){
                positions[i*4+j]=float(s.p[j]-origin[j]);
                colours[i*4+j]=s.colour[j]*lampScale;
            }
            positions[i*4+3]=s.radius;colours[i*4+3]=s.slot;
        }
        check(d->SetPixelShaderConstantF(40,params,1));
        if(local){
            check(d->SetPixelShaderConstantF(41,positions,kShaftLights));
            check(d->SetPixelShaderConstantF(65,colours,kShaftLights));
            // The cube atlas and its rows, exactly as the resolve gets them.
            if(shaftShadowed)WosLocalShadow::Bind(d,origin);
        }
        WosDeferredGpu::Scope gpu(d,WosDeferredGpu::SectionVolumetric);
        check(d->SetRenderTarget(0,marchSurface));
        check(d->SetPixelShader(marchShader));
        if(ok)ok=WosIndirect::Quad(d,marchWidth,marchHeight);
        check(d->SetTexture(9,nullptr));check(d->SetTexture(10,nullptr));
        check(d->SetTexture(0,marchTexture));
        check(d->SetRenderTarget(0,target));
        check(d->SetPixelShader(compositeShader));
        check(d->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE));
        check(d->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_ONE));
        // The debug view replaces the scene instead of adding to it, so the
        // shafts can be read without the exposure of the lit image on top.
        check(d->SetRenderState(D3DRS_DESTBLEND,debugView?D3DBLEND_ZERO:(fogEnabled?D3DBLEND_SRCALPHA:D3DBLEND_ONE)));
        check(d->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD));
        // Scene alpha belongs to the native material contract; only RGB moves.
        check(d->SetRenderState(D3DRS_COLORWRITEENABLE,7));
        if(ok)ok=WosIndirect::Quad(d,width,height);
        check(d->SetTexture(0,nullptr));
    }
    check(d->SetTexture(4,nullptr));
    WosCascadeShadow::Unbind(d);
    const bool restored=!state.valid||state.Restore();
    target->Release();
    if(!restored){
        ++restoreFailures;faulted=true;
        status="Volumetric restore failed; shafts disabled for this session";
        return false;
    }
    if(!ok){++failures;status="Volumetric pass failed; original image retained";return false;}
    if(!draw)return false;
    ++frames;if(local)++localFrames;
    skyGlowReady=local&&!debugView;
    status=bound?(local?"Volumetric sun shafts and lamp shafts ON":"Volumetric sun shafts ON (half-resolution cascade march)")
                :"Volumetric lamp shafts ON (no sun shafts this frame)";
    return true;
}
// Lamp glow over the sky. The bloom shader composites the cloud layer as
// hdr*T + scatter, after this pass has added the shafts, so clouds swallowed
// the glow of lamps standing in front of them (2026-10-05 screenshot). The
// composite therefore leaves sky pixels alone, and the bloom draw gets the
// half-resolution march on s5 with c37 = (width, height, strength, 11): it
// adds the sky taps after the clouds. Without the marker the bloom shader is
// unchanged.
static unsigned long skyBinds=0;
struct SkyScope{
    IDirect3DDevice9*d=nullptr;IDirect3DBaseTexture9*savedTexture=nullptr;
    DWORD savedSampler[5]={};float savedConstant[4]={};bool bound=false;
    static constexpr D3DSAMPLERSTATETYPE kStates[5]={D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER,D3DSAMP_MIPFILTER,D3DSAMP_ADDRESSU,D3DSAMP_ADDRESSV};
    SkyScope(IDirect3DDevice9*device,bool bloom){
        if(!bloom||!skyGlowReady||!marchTexture||!device)return;
        d=device;
        if(FAILED(d->GetTexture(5,&savedTexture)))savedTexture=nullptr;
        for(int i=0;i<5;++i)d->GetSamplerState(5,kStates[i],&savedSampler[i]);
        d->GetPixelShaderConstantF(37,savedConstant,1);
        const DWORD values[5]={D3DTEXF_POINT,D3DTEXF_POINT,D3DTEXF_NONE,D3DTADDRESS_CLAMP,D3DTADDRESS_CLAMP};
        bool ok=SUCCEEDED(d->SetTexture(5,marchTexture));
        for(int i=0;i<5;++i)ok=SUCCEEDED(d->SetSamplerState(5,kStates[i],values[i]))&&ok;
        const float info[4]={float(marchWidth),float(marchHeight),strength,ok?11.f:0.f};
        ok=SUCCEEDED(d->SetPixelShaderConstantF(37,info,1))&&ok;
        bound=true;if(ok)++skyBinds;
    }
    ~SkyScope(){
        if(!bound)return;
        d->SetTexture(5,savedTexture);
        for(int i=0;i<5;++i)d->SetSamplerState(5,kStates[i],savedSampler[i]);
        d->SetPixelShaderConstantF(37,savedConstant,1);
        if(savedTexture)savedTexture->Release();
    }
};
static void FrameEnd(){
    skyGlowReady=false;
    lastFrames=frames;frames=0;
    lastLocalFrames=localFrames;localFrames=0;
    lastShaftCount=shaftCount;lastShaftShadowed=shaftShadowed;lastShaftObserved=shaftObserved;
    shaftCount=shaftShadowed=0;
    // Discovery has to run during the next frame's primary draws.
    WosLocalShadow::collectForShafts=localEnabled;
    if(!enabled&&!localEnabled)status="Volumetric sun shafts OFF";
    else if(!enabled){
        if(faulted)status="Volumetric shafts disabled after a device fault";
        else if(!lastLocalFrames)status=lastShaftObserved?"Volumetric lamp shafts: no local light in view":"Volumetric lamp shafts: no local lights this frame";
    }
    else if(faulted)status="Volumetric sun shafts disabled after a device fault";
    else if(!WosCascadeShadow::enabled||WosCascadeShadow::mode<=0)status="Volumetric needs the four dynamic cascades ON";
    else if(!lastFrames&&!noSun&&!noCascade)status="Volumetric: no bloom composite seen this frame";
}
}
