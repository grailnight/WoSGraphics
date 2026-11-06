#include "../cloud_sun_sync.h"
#pragma once
// Primary opaque rendering: one native geometry invocation writes scene residual
// and G-buffer data. Resolve failures disable future primary frames; already
// written residual pixels cannot be described as a complete native fallback.
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/deferred_lights.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/deferred_paired_lights.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/deferred_primary_state.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/primary_guard_builder.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/deferred_gpu_profiler.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/deferred_state_shadow.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/deferred_cascade_shadow.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/deferred_sky_height.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/deferred_local_shadow.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/night_lamps/night_lamps.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/deferred_volumetric.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/deferred_cloud_light.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/deferred_cloud_shadow.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/material_classes.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/gbuffer_dump.h"
#include <cstdlib>
#include "../../sky_ibl/sky_ibl_runtime.h"
#include "native_rain.h"
#include "shadow_fast_globals.h"
#include "shadow_work_cache.h"
#include "hook_profile.h"
#ifndef WOS_DEFERRED_ASSET_DIRECTORY
#define WOS_DEFERRED_ASSET_DIRECTORY "\\full_deferred"
#endif
namespace WosDeferred {
static bool enabled=false, busy=false, faulted=false, unsafeDeviceState=false;
// Native registry hooks must not classify private deferred shaders/targets as
// engine assets. Preserve nesting while resources are created outside a draw.
struct ResourceCreationScope {
    bool previous=busy;
    ResourceCreationScope(){busy=true;}
    ~ResourceCreationScope(){busy=previous;}
};
static uint64_t frameSerial=1,receiverGeneration=0;
static void (*receiverMerge)(IDirect3DDevice9*)=nullptr;
static void (*receiverWrite)()=nullptr;
// With GI on, the targets used to go back after every primary draw so the
// GI material scope of the next non-primary draw found RT1..3 empty. The draw
// hooks now release them right before that scope instead ([Cache] GiHoldMrt).
static bool giHoldMrt=true;
// Light-read reuse, see ReadLights below ([Cache] LightReuse).
static bool lightReuse=true;
// Eligibility verdict reuse, see DrawScope ([Cache] VerdictReuse). The epoch
// moves whenever the variant table or the shader cache changes.
static bool verdictReuse=true;
static unsigned verdictEpoch=1;
static void (*receiverInvalidate)()=nullptr;
static void (*receiverReset)()=nullptr;
static void InvalidateReceivers(){if(receiverInvalidate)receiverInvalidate();}
static float intensity=1.f, specularIntensity=1.f;
static int debugView=0;
// Screen-space contact shadows in the resolve. Length 0 keeps the shader on
// its original path; the forward matrix is derived from the camera inverse.
static bool rainEnabled=false;
static float rainfallIntensity=.45f;
static float rainWetness=.8f,rainPuddles=.55f,rainRipples=.6f,rainSize=5.f,rainReflections=.65f;
static float RainClass(const std::string&name){
    if(name=="ps_0119.road_mix_diffuse" || name.find(".ground_mix")!=std::string::npos ||
       name.find(".ground_small")!=std::string::npos || name.find(".ground_extra")!=std::string::npos ||
       name.find(".ground4")!=std::string::npos || name.find(".ground10")!=std::string::npos)return 1.f;
    if(name=="ps_0086.bodycolor_diffuse.scene_alpha"||name=="ps_0045.skinned_transition")return 2.f;
    return 0.f;
}
static bool contactEnabled=false;
static float contactLength=.8f,contactThickness=.35f,contactStrength=1.f,contactBias=.02f;
// Contact v2 (c30): longest ray on screen in pixels, softness toward the far end,
// thickness added per view unit, and per-frame noise for TAA to average.
static float contactPixels=48.f,contactSoftness=.6f,contactThicknessGrowth=.01f;
static bool contactTemporal=true;
static int contactSteps=16;
// Geometric specular AA (c29): strength (0 = exact no-op), clamp on added alpha^2,
// relative depth step treated as a silhouette.
static float specularAA=1.f,specularAAClamp=.18f,specularAAEdge=.05f;
static unsigned contactMatrixFailures=0;
// Gauss-Jordan inverse in double of a 4x4 stored in dot-product row order.
static bool InvertMatrix(const float in[16],float out[16]){
    double a[4][8]={};
    for(unsigned r=0;r<4;++r){for(unsigned c=0;c<4;++c)a[r][c]=in[r*4+c];a[r][4+r]=1;}
    for(unsigned col=0;col<4;++col){
        unsigned pivot=col;for(unsigned r=col+1;r<4;++r)if(std::fabs(a[r][col])>std::fabs(a[pivot][col]))pivot=r;
        if(std::fabs(a[pivot][col])<1e-18)return false;
        if(pivot!=col)for(unsigned c=0;c<8;++c){double s=a[col][c];a[col][c]=a[pivot][c];a[pivot][c]=s;}
        double inv=1.0/a[col][col];for(unsigned c=0;c<8;++c)a[col][c]*=inv;
        for(unsigned r=0;r<4;++r)if(r!=col){double f=a[r][col];if(f!=0)for(unsigned c=0;c<8;++c)a[r][c]-=f*a[col][c];}
    }
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c){double v=a[r][4+c];if(!std::isfinite(v)||std::fabs(v)>1e12)return false;out[r*4+c]=float(v);}
    return true;
}
static const char* status="Primary deferred OFF";
static void Fault(const char*message){enabled=false;faulted=true;status=message;InvalidateReceivers();}
static IDirect3DDevice9* owner=nullptr;
static IDirect3DTexture9 *gbuffer[4]={}, *lightTexture=nullptr, *resolvedTexture=nullptr;
static IDirect3DSurface9 *surfaces[4]={}, *resolvedSurface=nullptr;
static IDirect3DPixelShader9* resolveShader=nullptr;
// Ambient occlusion, computed from the G-buffer this pipeline already fills and
// consumed only by the resolve, where the engine environment term is still a
// separate quantity from the sun. Half of each axis for the term itself, full
// resolution after the upsample so the resolve pays one point sample for it.
static IDirect3DPixelShader9 *ambientShader=nullptr,*ambientUpsampleShader=nullptr,*ambientTemporalShader=nullptr;
// Temporal accumulation of the half-resolution sky visibility. The two buffers
// alternate on every resolve pass (one per geometry batch, each passing the
// other batch's pixels through), and the camera the history belongs to is
// latched once per frame in FrameEnd.
static IDirect3DTexture9 *ambientHistory[2]={};static IDirect3DSurface9 *ambientHistorySurface[2]={};
static unsigned ambientHistoryIndex=0,ambientTemporalFrames=0,ambientTemporalResets=0,ambientFrameNumber=0;
static bool ambientHistoryLatched=false,ambientTemporalThisFrame=false,ambientTemporalLastFrame=false;
static float ambientPrevRelativeInverse[16]={},ambientFrameRelativeInverse[16]={};
static double ambientPrevOrigin[3]={},ambientFrameOrigin[3]={};static uint32_t ambientPrevScene=0,ambientFrameScene=0;
static IDirect3DTexture9 *ambientTexture=nullptr,*ambientFullTexture=nullptr;
static IDirect3DSurface9 *ambientSurface=nullptr,*ambientFullSurface=nullptr;
static UINT ambientWidth=0,ambientHeight=0;
static float ambientStrength=0.f,ambientRadius=4.f,ambientDistance=160.f,ambientBias=.06f;
// Soft lamp light ([SoftLight], after Proper Shaders): smoothstep share of the
// falloff (resolve c28.z), lamp highlight cut (c6.w), and on the CPU a radius
// scale and a dimming of every point light in the table. All zero/one = native.
static float lampSmooth=1.f,lampSpecularCut=.6f,lampRadiusScale=1.f,lampDim=0.f;
static unsigned ambientFrames=0,ambientFailures=0;
static IDirect3DSurface9 *sceneColor=nullptr, *sceneDepth=nullptr;
static IDirect3DTexture9 *depthTexture=nullptr,*sceneTexture=nullptr;
static WosIndirectCamera::Camera camera;
static UINT width=0,height=0;
static bool pending=false, loadAttempted=false;
static unsigned frames=0,supportedDraws=0,guardDraws=0,fallbackDraws=0,resolves=0,failures=0;
// Cumulative diagnostics for state caching decisions: a state block Apply
// replaces device state wholesale, outside every per-call hook.
static unsigned stateBlockCreates=0,stateBlockBegins=0,stateBlockEnds=0,stateBlockApplies=0,stateBlockAppliesDuringBatch=0;
static void NoteStateBlockApply(){++stateBlockApplies;if(pending)++stateBlockAppliesDuringBatch;}
static unsigned lastSupported=0,lastGuards=0,lastFallback=0,lastResolves=0;
static ULONGLONG lastStatus=0,presentWindowStart=0;
static unsigned presentWindowFrames=0;
static double observedPresentFps=0;
// CPU wall time around submission phases, including any driver stalls. These
// counters are not GPU timestamps. The draw field measures the ONE real primary draw.
struct CpuTimings {
    double eligibilityMs=0,captureMs=0,bindMs=0,replayMs=0,restoreMs=0,flushMs=0;
    // Sub-phases that were previously untimed. find is inside eligibility; the
    // others sit between eligibility and capture, or after restore (release).
    double findMs=0,bindingsMs=0,lightsMs=0,internMs=0,releaseMs=0;
    unsigned eligibilityCalls=0,replayCalls=0,flushCalls=0;
};
static CpuTimings cpuTimings,lastCpuTimings;
static double CpuMillisecondsPerTick(){
    static const double scale=[](){LARGE_INTEGER frequency={};return QueryPerformanceFrequency(&frequency)&&frequency.QuadPart>0?1000.0/double(frequency.QuadPart):0.0;}();
    return scale;
}
// Diagnostic phase timers cost ~1 ms/frame of QPC calls; they run only while [Profiler] Hooks=1.
static LONGLONG CpuTick(){if(!WosHookProfile::enabled)return 0;LARGE_INTEGER value={};QueryPerformanceCounter(&value);return value.QuadPart;}
static double CpuElapsed(LONGLONG start){if(!start)return 0;LONGLONG now=CpuTick();return now?double(now-start)*CpuMillisecondsPerTick():0;}
struct CpuPhase {
    double&total;const double*exclude;double before;LONGLONG start;
    explicit CpuPhase(double&destination,const double*excluded=nullptr):total(destination),exclude(excluded),before(excluded?*excluded:0),start(CpuTick()){}
    ~CpuPhase(){double elapsed=CpuElapsed(start)-(exclude?*exclude-before:0);if(elapsed>0)total+=elapsed;}
};
static WosDeferredLights::Registry lightRegistry;
// The runtime sets pixel shaders through the original entry, bypassing the
// integration hook, so it reports those changes to the shadow itself.
static HRESULT STDMETHODCALLTYPE TrackedSetPixelShader(IDirect3DDevice9*d,IDirect3DPixelShader9*p){
    HRESULT hr=g_original_set_pixel_shader(d,p);if(SUCCEEDED(hr))WosDeferredShadow::OnSetPixelShader(p);return hr;
}
// Open GPU interval of the current primary batch; see deferred_gpu_profiler.h.
static WosDeferredGpu::Mark batchMark;
// Consecutive primary draws mostly see the same native light registers. While
// no forwarded write has touched them since the last read (WosConstCache
// stamps), the raw read is returned again instead of re-fetched and re-packed.
// Every 64th hit reads anyway and compares; a mismatch turns reuse off.
struct LightReuseCache {
    bool valid=false,ok=false,broken=false;unsigned multiple=0,epoch=0,verify=0;
    IDirect3DDevice9*device=nullptr;WosDeferredLights::Binding binding;double origin[3]={};
    unsigned long long serial=0,hits=0,misses=0,checks=0,mismatches=0;
    WosDeferredLights::LightSet set;
};
static LightReuseCache lightCache;
static bool SameRegisters(const WosDeferredLights::Binding&a,const WosDeferredLights::Binding&b){
    auto same=[](const WosDeferredLights::Register&x,const WosDeferredLights::Register&y){return x.index==y.index&&x.count==y.count;};
    return same(a.direction,b.direction)&&same(a.color,b.color)&&same(a.pointPosition,b.pointPosition)&&
           same(a.pointColor,b.pointColor)&&same(a.pointCount,b.pointCount)&&a.hasDirectional==b.hasDirectional&&
           a.hasPoints==b.hasPoints&&a.bakedDirectional==b.bakedDirectional&&a.valid==b.valid;
}
static bool LightRegistersUntouched(const WosDeferredLights::Binding&b,unsigned long long serial){
    const WosDeferredLights::Register*r[]={&b.direction,&b.color,&b.pointPosition,&b.pointColor,&b.pointCount};
    for(auto*x:r)if(x->count&&!WosConstCache::PsUntouched(x->index,x->count,serial))return false;
    return true;
}
static bool ReadLights(IDirect3DDevice9*d,const WosDeferredLights::Binding&binding,const double origin[3],
                       WosDeferredLights::LightSet&out,unsigned multiple){
    auto&c=lightCache;
    // Not while the shadow is suspended (Reset in progress or the device lost):
    // the registers read back then are not what the game set.
    const bool usable=lightReuse&&!c.broken&&WosConstCache::psHooked&&!WosDeferredShadow::suspended;
    if(usable&&c.valid&&c.device==d&&c.epoch==WosConstCache::epoch&&c.multiple==multiple&&
       !std::memcmp(c.origin,origin,sizeof(c.origin))&&SameRegisters(c.binding,binding)&&
       LightRegistersUntouched(c.binding,c.serial)){
        if(++c.verify&63){++c.hits;out=c.set;return c.ok;}
        ++c.checks;WosDeferredLights::LightSet fresh;
        const bool ok=WosDeferredPairedLights::Read(d,binding,origin,fresh,multiple);
        if(ok!=c.ok||(ok&&std::memcmp(fresh.texels.data(),c.set.texels.data(),sizeof(float)*fresh.texels.size()))){++c.mismatches;c.broken=true;c.valid=false;}
        out=fresh;return ok;
    }
    ++c.misses;
    const unsigned long long serial=WosConstCache::stampSerial;
    const bool ok=WosDeferredPairedLights::Read(d,binding,origin,out,multiple);
    if(usable){
        c.valid=true;c.ok=ok;c.device=d;c.binding=binding;c.multiple=multiple;c.epoch=WosConstCache::epoch;
        c.serial=serial;std::memcpy(c.origin,origin,sizeof(c.origin));c.set=out;
    }
    return ok;
}
struct Variant {
    std::vector<unsigned char> input;
    IDirect3DPixelShader9* shader=nullptr;
    IDirect3DPixelShader9* alphaShader=nullptr;
    WosDeferredLights::Binding lights;
    bool supported=false,requiresAlphaOff=false;
    unsigned pointCountMultiple=1;
    float rainClass=0;
    // Diffuse-layer sampler registers from the CTAB; -1 until first used.
    uint8_t diffuseRegs[4]={};int diffuseCount=-1;
};
static std::vector<Variant> variants;
static std::unordered_map<IDirect3DPixelShader9*,int> shaderCache;
static std::string Directory(){return WosProbe::Root()+WOS_DEFERRED_ASSET_DIRECTORY;}
static std::string SettingsPath(){return Directory()+"\\settings.ini";}
static float ReadSetting(const char*section,const char*name,float fallback,float low,float high){
    char value[64]={},def[64]={};std::snprintf(def,sizeof(def),"%.9g",fallback);
    GetPrivateProfileStringA(section,name,def,value,sizeof(value),SettingsPath().c_str());
    char*end=nullptr;float v=std::strtof(value,&end);
    return end!=value&&*end==0&&std::isfinite(v)?(std::max)(low,(std::min)(v,high)):fallback;
}
static void LoadSettings(){
    static bool loaded=false;if(loaded)return;loaded=true;
    if(GetFileAttributesA(SettingsPath().c_str())==INVALID_FILE_ATTRIBUTES)return;
    enabled=GetPrivateProfileIntA("Deferred","Enabled",0,SettingsPath().c_str())!=0;
    rainEnabled=ReadSetting("Rain","Enabled",0,0,1)>.5f;
    rainfallIntensity=ReadSetting("Rain","Rainfall",.45f,0,1);
    rainWetness=ReadSetting("Rain","Wetness",.8f,0,1);
    rainPuddles=ReadSetting("Rain","Puddles",.55f,0,1);
    rainRipples=ReadSetting("Rain","Ripples",.6f,0,1);
    rainSize=ReadSetting("Rain","Size",5,.2f,30);
    rainReflections=ReadSetting("Rain","Reflections",.65f,0,1);
    WosSkyIBL::enabled=ReadSetting("SkyIBL","Enabled",1,0,1)>.5f;
    WosSkyIBL::diffuseStrength=ReadSetting("SkyIBL","Diffuse",.65f,0,2);
    WosSkyIBL::specularStrength=ReadSetting("SkyIBL","Specular",1,0,3);
    WosSkyIBL::material[0]=ReadSetting("Pbr","RoughnessFloor",.4f,0.05f,1);
    WosSkyIBL::material[1]=ReadSetting("Pbr","RoughnessCeiling",.95f,.05f,1);
    WosSkyIBL::material[2]=ReadSetting("Pbr","RoughnessVariance",.12f,0,1);
    WosSkyIBL::material[3]=ReadSetting("Pbr","DielectricF0",.04f,.001f,.16f);
    WosSkyIBL::material[4]=ReadSetting("Pbr","MetalStrength",0,0,1);
    WosSkyIBL::material[5]=ReadSetting("Pbr","MetalThreshold",.35f,0,1);
    WosSkyIBL::material[6]=ReadSetting("Pbr","MetalWidth",.25f,.01f,1);
    WosSkyIBL::material[7]=ReadSetting("Pbr","MetalTint",.5f,0,1);
    intensity=ReadSetting("Deferred","Intensity",1,0,3);
    specularIntensity=ReadSetting("Deferred","Specular",1,0,2);
    debugView=int(ReadSetting("Deferred","View",0,0,7));
    WosLocalShadow::enabled=GetPrivateProfileIntA("LocalShadows","Enabled",1,SettingsPath().c_str())!=0;
    WosLocalShadow::resolution=int(ReadSetting("LocalShadows","Resolution",512,512,1024));
    WosLocalShadow::emitterRadius=ReadSetting("LocalShadows","EmitterRadius",.18f,0,2);
    WosLocalShadow::bias=ReadSetting("LocalShadows","Bias",.035f,.001f,.5f);
    WosLocalShadow::normalBias=ReadSetting("LocalShadows","NormalBias",.025f,0,.5f);
    WosLocalShadow::strength=ReadSetting("LocalShadows","Strength",1,0,1);
    contactEnabled=GetPrivateProfileIntA("Contact","Enabled",0,SettingsPath().c_str())!=0;
    contactLength=ReadSetting("Contact","Length",contactLength,.05f,10);
    contactThickness=ReadSetting("Contact","Thickness",contactThickness,.01f,10);
    contactSteps=int(ReadSetting("Contact","Steps",float(contactSteps),2,32));
    contactStrength=ReadSetting("Contact","Strength",contactStrength,0,1);
    contactBias=ReadSetting("Contact","Bias",contactBias,0,1);
    contactPixels=ReadSetting("Contact","Pixels",contactPixels,4,256);
    contactSoftness=ReadSetting("Contact","Softness",contactSoftness,0,1);
    contactThicknessGrowth=ReadSetting("Contact","ThicknessGrowth",contactThicknessGrowth,0,.1f);
    contactTemporal=ReadSetting("Contact","Temporal",1,0,1)>.5f;
    specularAA=ReadSetting("SpecularAA","Strength",specularAA,0,4);
    specularAAClamp=ReadSetting("SpecularAA","Clamp",specularAAClamp,0,1);
    specularAAEdge=ReadSetting("SpecularAA","Edge",specularAAEdge,.005f,1);
    WosMaterialClasses::tablePath=Directory()+"\\texture_classes.bin";
    WosMaterialClasses::strength=ReadSetting("PbrClasses","Strength",WosMaterialClasses::strength,0,1);
    WosMaterialClasses::rainPull=ReadSetting("PbrClasses","RainPull",WosMaterialClasses::rainPull,0,1);
    // One line per class: floor ceiling metalness coverage.
    for(unsigned c=1;c<WosMaterialClasses::ClassCount;++c){
        if(!WosMaterialClasses::IsRow(c))continue;
        char value[128]={};float v[4]={};
        GetPrivateProfileStringA("PbrClasses",WosMaterialClasses::Names[c],"",value,sizeof(value),SettingsPath().c_str());
        if(std::sscanf(value,"%f %f %f %f",&v[0],&v[1],&v[2],&v[3])==4)
            for(unsigned i=0;i<4;++i)if(std::isfinite(v[i]))WosMaterialClasses::table[c][i]=(std::max)(0.f,(std::min)(v[i],1.f));
    }
    ambientStrength=ReadSetting("AmbientOcclusion","Strength",ambientStrength,0,1);
    ambientRadius=ReadSetting("AmbientOcclusion","Radius",ambientRadius,.5f,32);
    ambientDistance=ReadSetting("AmbientOcclusion","Distance",ambientDistance,20,600);
    ambientBias=ReadSetting("AmbientOcclusion","Bias",ambientBias,0,.5f);
    lampSmooth=ReadSetting("SoftLight","Smooth",lampSmooth,0,1);
    lampSpecularCut=ReadSetting("SoftLight","LampSpecularCut",lampSpecularCut,0,1);
    lampRadiusScale=ReadSetting("SoftLight","RadiusScale",lampRadiusScale,1,2);
    lampDim=ReadSetting("SoftLight","LampDim",lampDim,0,.9f);
    WosSkyHeight::enabled=ReadSetting("SkyHeight","Enabled",1,0,1)>.5f;
    WosSkyHeight::extent=ReadSetting("SkyHeight","Extent",WosSkyHeight::extent,64,1024);
    WosSkyHeight::rayLength=ReadSetting("SkyHeight","RayLength",WosSkyHeight::rayLength,4,120);
    WosSkyHeight::interval=int(ReadSetting("SkyHeight","Interval",float(WosSkyHeight::interval),1,30));
    WosCascadeShadow::enabled=GetPrivateProfileIntA("Cascades","Enabled",0,SettingsPath().c_str())!=0;
    WosCascadeShadow::mode=int(ReadSetting("Cascades","Mode",float(WosCascadeShadow::mode),0,2));
    WosCascadeShadow::resolution=int(ReadSetting("Cascades","Resolution",float(WosCascadeShadow::resolution),256,2048));
    WosCascadeShadow::distance=ReadSetting("Cascades","Distance",WosCascadeShadow::distance,20,400);
    WosCascadeShadow::stagger=int(ReadSetting("Cascades","Stagger",float(WosCascadeShadow::stagger),0,2));
    WosCascadeShadow::saCasters=ReadSetting("Cascades","SaCasters",WosCascadeShadow::saCasters?1.f:0.f,0,1)>=.5f;
    WosCascadeShadow::sunAngle=ReadSetting("Cascades","SunAngle",WosCascadeShadow::sunAngle,0,.2f);
    WosCascadeShadow::searchDistance=ReadSetting("Cascades","SearchDistance",WosCascadeShadow::searchDistance,1,120);
    WosCascadeShadow::depthBias=ReadSetting("Cascades","DepthBias",WosCascadeShadow::depthBias,0,2);
    WosCascadeShadow::normalBias=ReadSetting("Cascades","NormalBias",WosCascadeShadow::normalBias,0,8);
    WosCascadeShadow::strength=ReadSetting("Cascades","Strength",WosCascadeShadow::strength,0,1);
    WosVolumetric::fogEnabled=GetPrivateProfileIntA("AtmosphericFog","Enabled",0,SettingsPath().c_str())!=0;
    WosVolumetric::fogHeightEnabled=GetPrivateProfileIntA("AtmosphericFog","HeightEnabled",0,SettingsPath().c_str())!=0;
    WosVolumetric::fogBaseHeight=ReadSetting("AtmosphericFog","BaseHeight",WosVolumetric::fogBaseHeight,-10000,10000);
    WosVolumetric::fogHeightFalloff=ReadSetting("AtmosphericFog","HeightFalloff",WosVolumetric::fogHeightFalloff,0,.2f);
    WosVolumetric::fogDensity=ReadSetting("AtmosphericFog","Density",WosVolumetric::fogDensity,0,.01f);
    WosVolumetric::fogStart=ReadSetting("AtmosphericFog","Start",WosVolumetric::fogStart,0,500);
    WosVolumetric::fogDistance=ReadSetting("AtmosphericFog","Distance",WosVolumetric::fogDistance,20,2000);
    WosVolumetric::fogColour[0]=ReadSetting("AtmosphericFog","Red",WosVolumetric::fogColour[0],0,4);
    WosVolumetric::fogColour[1]=ReadSetting("AtmosphericFog","Green",WosVolumetric::fogColour[1],0,4);
    WosVolumetric::fogColour[2]=ReadSetting("AtmosphericFog","Blue",WosVolumetric::fogColour[2],0,4);
    WosVolumetric::enabled=GetPrivateProfileIntA("Volumetric","Enabled",0,SettingsPath().c_str())!=0;
    WosVolumetric::steps=int(ReadSetting("Volumetric","Steps",float(WosVolumetric::steps),4,64));
    WosVolumetric::distance=ReadSetting("Volumetric","Distance",WosVolumetric::distance,20,600);
    WosVolumetric::density=ReadSetting("Volumetric","Density",WosVolumetric::density,0,.2f);
    WosVolumetric::strength=ReadSetting("Volumetric","Strength",WosVolumetric::strength,0,4);
    WosVolumetric::anisotropy=ReadSetting("Volumetric","Anisotropy",WosVolumetric::anisotropy,0,.95f);
    WosVolumetric::rejection=ReadSetting("Volumetric","Rejection",WosVolumetric::rejection,0,4);
    WosVolumetric::debugView=int(ReadSetting("Volumetric","View",float(WosVolumetric::debugView),0,1));
    WosVolumetric::localEnabled=GetPrivateProfileIntA("Volumetric","LocalEnabled",0,SettingsPath().c_str())!=0;
    WosVolumetric::localMaxLights=int(ReadSetting("Volumetric","LocalMaxLights",float(WosVolumetric::localMaxLights),1,24));
    WosVolumetric::localDensity=ReadSetting("Volumetric","LocalDensity",WosVolumetric::localDensity,0,.5f);
    WosVolumetric::localStrength=ReadSetting("Volumetric","LocalStrength",WosVolumetric::localStrength,0,8);
    WosVolumetric::localCore=ReadSetting("Volumetric","LocalCore",WosVolumetric::localCore,.1f,10);
    WosVolumetric::localRadiusScale=ReadSetting("Volumetric","LocalRadius",WosVolumetric::localRadiusScale,.25f,3);
    WosVolumetric::localShadowSteps=int(ReadSetting("Volumetric","LocalShadowSteps",float(WosVolumetric::localShadowSteps),0,48));
    WosVolumetric::localMaxDistance=ReadSetting("Volumetric","LocalMaxDistance",WosVolumetric::localMaxDistance,20,1000);
    WosLocalShadow::collectForShafts=WosVolumetric::localEnabled;
    WosCloudLight::enabled=GetPrivateProfileIntA("CloudLight","Enabled",1,SettingsPath().c_str())!=0;
    WosCloudLight::blocking=ReadSetting("CloudLight","Blocking",WosCloudLight::blocking,0,1);
    WosCloudShadow::enabled=GetPrivateProfileIntA("CloudShadow","Enabled",1,SettingsPath().c_str())!=0;
    WosCloudShadow::strength=ReadSetting("CloudShadow","Strength",WosCloudShadow::strength,0,1);
    WosCloudShadow::steps=int(ReadSetting("CloudShadow","Steps",float(WosCloudShadow::steps),1,16));
    WosCloudShadow::minimum=ReadSetting("CloudShadow","Minimum",WosCloudShadow::minimum,0,1);
    WosCloudShadow::compensate=GetPrivateProfileIntA("CloudShadow","Compensate",1,SettingsPath().c_str())!=0;
    WosIndirect::enabled=GetPrivateProfileIntA("Indirect","Enabled",WosIndirect::enabled?1:0,SettingsPath().c_str())!=0;
    WosIndirect::nativeRsmCapture=GetPrivateProfileIntA("Indirect","NativeRSM",0,SettingsPath().c_str())!=0;
    WosIndirect::bounce=ReadSetting("Indirect","Bounce",WosIndirect::bounce,0,1.5f);
    WosIndirect::ambient=ReadSetting("Indirect","Ambient",WosIndirect::ambient,0,.3f);
    WosIndirect::radius=ReadSetting("Indirect","Radius",WosIndirect::radius,2,20);
    WosIndirect::maxDistance=ReadSetting("Indirect","Distance",WosIndirect::maxDistance,40,400);
    WosDeferredGpu::enabled=GetPrivateProfileIntA("Profiler","Gpu",0,SettingsPath().c_str())!=0;
    WosHookProfile::enabled=GetPrivateProfileIntA("Profiler","Hooks",0,SettingsPath().c_str())!=0;
    WosShadowFastGlobals::enabled=GetPrivateProfileIntA("Cache","ShadowGlobals",1,SettingsPath().c_str())!=0;
    WosShadowWork::enabled=GetPrivateProfileIntA("Cache","ShadowWork",1,SettingsPath().c_str())!=0;
    WosLocalShadow::staticEnabled=GetPrivateProfileIntA("Cache","LocalStatic",1,SettingsPath().c_str())!=0;
    WosShadowWork::cullEnabled=GetPrivateProfileIntA("Cache","CasterCull",1,SettingsPath().c_str())!=0;
    giHoldMrt=GetPrivateProfileIntA("Cache","GiHoldMrt",1,SettingsPath().c_str())!=0;
    lightReuse=GetPrivateProfileIntA("Cache","LightReuse",1,SettingsPath().c_str())!=0;
    verdictReuse=GetPrivateProfileIntA("Cache","VerdictReuse",1,SettingsPath().c_str())!=0;
    WosDeferredShadow::verifyReadOnly=verdictReuse;
    WosDeferredShadow::SetEnabled(GetPrivateProfileIntA("Cache","Eligibility",1,SettingsPath().c_str())!=0);
    status=enabled?"Deferred direct lighting armed":"Deferred direct lighting OFF";
}
static void SaveSettings(){
    auto put=[&](const char*section,const char*name,float value){
        char text[64]={};std::snprintf(text,sizeof(text),"%.9g",value);
        WritePrivateProfileStringA(section,name,text,SettingsPath().c_str());
    };
    put("SkyIBL","Enabled",WosSkyIBL::enabled?1.f:0.f);put("SkyIBL","Diffuse",WosSkyIBL::diffuseStrength);put("SkyIBL","Specular",WosSkyIBL::specularStrength);
    put("Pbr","RoughnessFloor",WosSkyIBL::material[0]);
    put("Pbr","RoughnessCeiling",WosSkyIBL::material[1]);
    put("Pbr","RoughnessVariance",WosSkyIBL::material[2]);
    put("Pbr","DielectricF0",WosSkyIBL::material[3]);
    put("Pbr","MetalStrength",WosSkyIBL::material[4]);
    put("Pbr","MetalThreshold",WosSkyIBL::material[5]);
    put("Pbr","MetalWidth",WosSkyIBL::material[6]);
    put("Pbr","MetalTint",WosSkyIBL::material[7]);
    put("Deferred","Enabled",enabled?1.f:0.f);put("Deferred","Intensity",intensity);
    put("Rain","Enabled",rainEnabled?1.f:0.f);put("Rain","Wetness",rainWetness);
    put("Rain","Puddles",rainPuddles);put("Rain","Ripples",rainRipples);
    put("Rain","Rainfall",rainfallIntensity);
    put("Rain","Size",rainSize);put("Rain","Reflections",rainReflections);
    put("Deferred","Specular",specularIntensity);put("Deferred","View",float(debugView));
    put("Indirect","Enabled",WosIndirect::enabled?1.f:0.f);put("Indirect","NativeRSM",WosIndirect::nativeRsmCapture?1.f:0.f);put("Indirect","Bounce",WosIndirect::bounce);
    put("Indirect","Ambient",WosIndirect::ambient);put("Indirect","Radius",WosIndirect::radius);put("Indirect","Distance",WosIndirect::maxDistance);
    put("Profiler","Gpu",WosDeferredGpu::enabled?1.f:0.f);
    put("Cache","ShadowGlobals",WosShadowFastGlobals::enabled?1.f:0.f);
    put("Cache","ShadowWork",WosShadowWork::enabled?1.f:0.f);
    put("Cache","LocalStatic",WosLocalShadow::staticEnabled?1.f:0.f);
    put("Cache","CasterCull",WosShadowWork::cullEnabled?1.f:0.f);
    put("Cache","GiHoldMrt",giHoldMrt?1.f:0.f);
    put("Cache","LightReuse",lightReuse?1.f:0.f);
    put("Cache","VerdictReuse",verdictReuse?1.f:0.f);
    put("LocalShadows","Enabled",WosLocalShadow::enabled?1.f:0.f);
    put("LocalShadows","Resolution",float(WosLocalShadow::resolution));
    put("LocalShadows","EmitterRadius",WosLocalShadow::emitterRadius);
    put("LocalShadows","Bias",WosLocalShadow::bias);put("LocalShadows","NormalBias",WosLocalShadow::normalBias);
    put("LocalShadows","Strength",WosLocalShadow::strength);
    put("Contact","Enabled",contactEnabled?1.f:0.f);put("Contact","Length",contactLength);put("Contact","Thickness",contactThickness);
    put("Contact","Steps",float(contactSteps));put("Contact","Strength",contactStrength);put("Contact","Bias",contactBias);
    put("Contact","Pixels",contactPixels);put("Contact","Softness",contactSoftness);
    put("Contact","ThicknessGrowth",contactThicknessGrowth);put("Contact","Temporal",contactTemporal?1.f:0.f);
    put("SpecularAA","Strength",specularAA);put("SpecularAA","Clamp",specularAAClamp);put("SpecularAA","Edge",specularAAEdge);
    put("PbrClasses","Strength",WosMaterialClasses::strength);put("PbrClasses","RainPull",WosMaterialClasses::rainPull);
    for(unsigned c=1;c<WosMaterialClasses::ClassCount;++c){
        if(!WosMaterialClasses::IsRow(c))continue;
        const float*v=WosMaterialClasses::table[c];char text[128]={};
        std::snprintf(text,sizeof(text),"%.3g %.3g %.3g %.3g",v[0],v[1],v[2],v[3]);
        WritePrivateProfileStringA("PbrClasses",WosMaterialClasses::Names[c],text,SettingsPath().c_str());
    }
    put("AmbientOcclusion","Strength",ambientStrength);put("AmbientOcclusion","Radius",ambientRadius);
    put("AmbientOcclusion","Distance",ambientDistance);put("AmbientOcclusion","Bias",ambientBias);
    put("SoftLight","Smooth",lampSmooth);put("SoftLight","LampSpecularCut",lampSpecularCut);
    put("SoftLight","RadiusScale",lampRadiusScale);put("SoftLight","LampDim",lampDim);
    put("SkyHeight","Enabled",WosSkyHeight::enabled?1.f:0.f);put("SkyHeight","Extent",WosSkyHeight::extent);
    put("SkyHeight","RayLength",WosSkyHeight::rayLength);put("SkyHeight","Interval",float(WosSkyHeight::interval));
    put("Cache","Eligibility",WosDeferredShadow::requested?1.f:0.f);
    put("Cascades","Enabled",WosCascadeShadow::enabled?1.f:0.f);put("Cascades","Mode",float(WosCascadeShadow::mode));
    put("Cascades","Resolution",float(WosCascadeShadow::resolution));put("Cascades","Distance",WosCascadeShadow::distance);put("Cascades","Stagger",float(WosCascadeShadow::stagger));put("Cascades","SaCasters",WosCascadeShadow::saCasters?1.f:0.f);
    put("Cascades","SunAngle",WosCascadeShadow::sunAngle);put("Cascades","SearchDistance",WosCascadeShadow::searchDistance);
    put("Cascades","DepthBias",WosCascadeShadow::depthBias);put("Cascades","NormalBias",WosCascadeShadow::normalBias);
    put("Cascades","Strength",WosCascadeShadow::strength);
    put("AtmosphericFog","Enabled",WosVolumetric::fogEnabled?1.f:0.f);
    put("AtmosphericFog","HeightEnabled",WosVolumetric::fogHeightEnabled?1.f:0.f);
    put("AtmosphericFog","BaseHeight",WosVolumetric::fogBaseHeight);
    put("AtmosphericFog","HeightFalloff",WosVolumetric::fogHeightFalloff);
    put("AtmosphericFog","Density",WosVolumetric::fogDensity);
    put("AtmosphericFog","Start",WosVolumetric::fogStart);
    put("AtmosphericFog","Distance",WosVolumetric::fogDistance);
    put("AtmosphericFog","Red",WosVolumetric::fogColour[0]);
    put("AtmosphericFog","Green",WosVolumetric::fogColour[1]);
    put("AtmosphericFog","Blue",WosVolumetric::fogColour[2]);
    put("Volumetric","Enabled",WosVolumetric::enabled?1.f:0.f);put("Volumetric","Steps",float(WosVolumetric::steps));
    put("Volumetric","Distance",WosVolumetric::distance);put("Volumetric","Density",WosVolumetric::density);
    put("Volumetric","Strength",WosVolumetric::strength);put("Volumetric","Anisotropy",WosVolumetric::anisotropy);
    put("Volumetric","Rejection",WosVolumetric::rejection);put("Volumetric","View",float(WosVolumetric::debugView));
    put("Volumetric","LocalEnabled",WosVolumetric::localEnabled?1.f:0.f);put("Volumetric","LocalMaxLights",float(WosVolumetric::localMaxLights));
    put("Volumetric","LocalDensity",WosVolumetric::localDensity);put("Volumetric","LocalStrength",WosVolumetric::localStrength);
    put("Volumetric","LocalCore",WosVolumetric::localCore);put("Volumetric","LocalRadius",WosVolumetric::localRadiusScale);
    put("Volumetric","LocalShadowSteps",float(WosVolumetric::localShadowSteps));put("Volumetric","LocalMaxDistance",WosVolumetric::localMaxDistance);
    put("CloudLight","Enabled",WosCloudLight::enabled?1.f:0.f);put("CloudLight","Blocking",WosCloudLight::blocking);
    put("CloudShadow","Enabled",WosCloudShadow::enabled?1.f:0.f);put("CloudShadow","Strength",WosCloudShadow::strength);
    put("CloudShadow","Steps",float(WosCloudShadow::steps));put("CloudShadow","Minimum",WosCloudShadow::minimum);
    put("CloudShadow","Compensate",WosCloudShadow::compensate?1.f:0.f);
}
template<class T> static void Release(T*& p){if(p)p->Release();p=nullptr;}
// Held G-buffer targets; see BindMrt below.
static bool mrtHeld=false,mrtSelf=false;
static IDirect3DSurface9* mrtNative[3]={};
static bool ReleaseMrt(IDirect3DDevice9*d);
static void DropMrtRefs();
static void DropBatch(){
    pending=false;batchMark={};Release(sceneColor);Release(sceneDepth);Release(depthTexture);Release(sceneTexture);
    camera={};lightRegistry.Clear();
}
static void ReleaseTargets(){
    if(mrtHeld)ReleaseMrt(owner);DropMrtRefs();mrtHeld=false;
    DropBatch();for(auto*&s:surfaces)Release(s);for(auto*&t:gbuffer)Release(t);
    Release(resolvedSurface);Release(resolvedTexture);Release(lightTexture);
    Release(ambientSurface);Release(ambientTexture);Release(ambientFullSurface);Release(ambientFullTexture);
    for(unsigned i=0;i<2;++i){Release(ambientHistorySurface[i]);Release(ambientHistory[i]);}
    ambientHistoryLatched=ambientTemporalThisFrame=ambientTemporalLastFrame=false;
    ambientWidth=ambientHeight=0;width=height=0;
}
static void Reset(){
    WosSkyIBL::Reset();WosSkyHeight::Reset();
    WosLocalShadow::Reset();
    WosCascadeShadow::Reset();WosVolumetric::Reset();WosRain::Reset();WosCloudLight::Reset();WosCloudSunSync::Reset();WosCloudShadow::Reset();
    if(receiverReset)receiverReset();WosDeferredGpu::Release();WosDeferredShadow::Reset();++verdictEpoch;faulted=false;unsafeDeviceState=false;++frameSerial;receiverGeneration=0;
    ReleaseTargets();Release(resolveShader);Release(ambientShader);Release(ambientUpsampleShader);Release(ambientTemporalShader);
    for(auto&v:variants){Release(v.shader);Release(v.alphaShader);}variants.clear();
    for(auto&entry:shaderCache){auto*p=entry.first;p->Release();}shaderCache.clear();
    owner=nullptr;loadAttempted=false;busy=false;
    presentWindowStart=0;presentWindowFrames=0;observedPresentFps=0;
    cpuTimings={};lastCpuTimings={};
}
namespace MaterialReloadDetail {static bool Tokens(const std::vector<unsigned char>&bytes);}
static bool Load(IDirect3DDevice9*d){
    if(owner&&owner!=d)Reset();
    if(loadAttempted)return owner==d&&resolveShader&&!variants.empty();
    loadAttempted=true;owner=d;
    WosCascadeShadow::hostBusy=&busy;WosCascadeShadow::hostActive=&enabled;
    WosSkyHeight::hostBusy=&busy;WosSkyHeight::hostActive=&enabled;
    WosVolumetric::hostBusy=&busy;WosVolumetric::directory=Directory();
    ResourceCreationScope privateResources;
    D3DCAPS9 caps={};
    if(FAILED(d->GetDeviceCaps(&caps))||caps.NumSimultaneousRTs<4||caps.PixelShaderVersion<D3DPS_VERSION(3,0)||
       !(caps.PrimitiveMiscCaps&D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING)||
       !(caps.PrimitiveMiscCaps&D3DPMISCCAPS_INDEPENDENTWRITEMASKS)){
        status="Deferred requires SM3 and four MRTs with alpha-test/write masks";return false;
    }
    auto bytes=WosIndirectMaterials::Read(Directory()+"\\deferred_resolve.cso");
    if(!MaterialReloadDetail::Tokens(bytes)||FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(bytes.data()),&resolveShader))){
        status="Deferred resolve shader unavailable";return false;
    }
    // An older resolve ignores sampler s10, so the static layer would be lost
    // there; it is only produced when the shader's constant table names it.
    {static const char kName[]="LocalStatic";
     WosLocalShadow::staticShaderReady=std::search(bytes.begin(),bytes.end(),kName,kName+sizeof(kName)-1)!=bytes.end();}
    // A missing or broken occlusion shader is not a fault. The resolve then
    // multiplies by a buffer that stays at one, which is exactly what it did
    // before this pass existed.
    for(auto entry:{std::make_pair("\\deferred_ambient_occlusion.cso",&ambientShader),
                    std::make_pair("\\deferred_ambient_upsample.cso",&ambientUpsampleShader),
                    std::make_pair("\\deferred_ambient_temporal.cso",&ambientTemporalShader)}){
        auto code=WosIndirectMaterials::Read(Directory()+entry.first);
        if(code.empty()||!MaterialReloadDetail::Tokens(code)||
           FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(code.data()),entry.second)))Release(*entry.second);
    }
    WIN32_FIND_DATAA data={};std::string folder=Directory()+"\\materials\\";
    HANDLE search=FindFirstFileA((folder+"*.input.cso").c_str(),&data);
    if(search!=INVALID_HANDLE_VALUE){
        do{
            if(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)continue;
            std::string name=data.cFileName;if(name.size()<=10||variants.size()>=512)continue;
            name.resize(name.size()-10);Variant v;v.rainClass=RainClass(name);
            v.input=WosIndirectMaterials::Read(folder+name+".input.cso");
            auto output=WosIndirectMaterials::Read(folder+name+".primary.cso");
            if(!MaterialReloadDetail::Tokens(v.input)||!MaterialReloadDetail::Tokens(output)||!WosDeferredLights::Parse(v.input.data(),v.input.size(),v.lights,name.find(".baked_page")!=std::string::npos)||
               FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(output.data()),&v.shader)))continue;
            char pointPolicy[16]={};
            GetPrivateProfileStringA("Material","PointCountMultiple","1",pointPolicy,sizeof(pointPolicy),(folder+name+".policy.ini").c_str());
            if(std::strcmp(pointPolicy,"1")&&std::strcmp(pointPolicy,"2")){Release(v.shader);continue;}
            v.pointCountMultiple=pointPolicy[0]=='2'?2u:1u;
            // Primary assets always preserve native alpha; no replay-only alpha policy.
            v.supported=true;variants.push_back(std::move(v));++verdictEpoch;
        }while(FindNextFileA(search,&data));FindClose(search);
    }
    status=variants.empty()?"Deferred material shaders unavailable":"Deferred material shaders ready";
    return !variants.empty();
}
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/deferred_material_reload.h"
static Variant* Find(IDirect3DDevice9*d,IDirect3DPixelShader9*ps){
    if(!ps||!Load(d))return nullptr;
    auto old=shaderCache.find(ps);
    if(old!=shaderCache.end())return old->second<0?nullptr:&variants[old->second];
    if(shaderCache.size()>=1024)return nullptr;
    UINT size=0;int index=-1;
    if(SUCCEEDED(ps->GetFunction(nullptr,&size))&&size>=8&&size<=1024*1024&&!(size%4)){
        std::vector<unsigned char> bytes(size);
        if(SUCCEEDED(ps->GetFunction(bytes.data(),&size))&&size<=bytes.size()){
            bytes.resize(size);
            for(size_t i=0;i<variants.size();++i)if(variants[i].input==bytes){index=int(i);break;}
            if(index<0&&variants.size()<1024){
                std::vector<unsigned char> guard;std::string reason;
                if(WosPrimaryGuard::Build(bytes,guard,reason)){
                    Variant v;v.input=std::move(bytes);
                    ResourceCreationScope privateResources;
                    if(SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(guard.data()),&v.shader))){
                        index=int(variants.size());variants.push_back(std::move(v));++verdictEpoch;
                    }
                }
            }
        }
    }
    ps->AddRef();shaderCache.emplace(ps,index);
    return index<0?nullptr:&variants[index];
}
static bool Targets(IDirect3DDevice9*d,UINT w,UINT h){
    if(width==w&&height==h&&surfaces[3]&&lightTexture&&resolvedSurface)return true;
    ReleaseTargets();ResourceCreationScope privateResources;bool ok=true;
    for(unsigned i=1;i<4&&ok;++i){
        ok=SUCCEEDED(d->CreateTexture(w,h,1,D3DUSAGE_RENDERTARGET,D3DFMT_A16B16G16R16F,D3DPOOL_DEFAULT,&gbuffer[i],nullptr));
        if(ok)ok=SUCCEEDED(gbuffer[i]->GetSurfaceLevel(0,&surfaces[i]));
    }
    if(ok)ok=SUCCEEDED(d->CreateTexture(w,h,1,D3DUSAGE_RENDERTARGET,D3DFMT_A16B16G16R16F,D3DPOOL_DEFAULT,&resolvedTexture,nullptr));
    if(ok)ok=SUCCEEDED(resolvedTexture->GetSurfaceLevel(0,&resolvedSurface));
    if(ok)ok=SUCCEEDED(d->CreateTexture(32,1024,1,D3DUSAGE_DYNAMIC,D3DFMT_A32B32G32R32F,D3DPOOL_DEFAULT,&lightTexture,nullptr));
    // Half of each axis for the occlusion term, full resolution after the
    // upsample. A failure here leaves both null and the resolve unmodulated,
    // so it must not fail the batch.
    ambientWidth=(w+1)/2;ambientHeight=(h+1)/2;
    if(ok&&ambientShader&&ambientUpsampleShader){
        bool aok=SUCCEEDED(d->CreateTexture(ambientWidth,ambientHeight,1,D3DUSAGE_RENDERTARGET,D3DFMT_R32F,D3DPOOL_DEFAULT,&ambientTexture,nullptr))
              &&SUCCEEDED(ambientTexture->GetSurfaceLevel(0,&ambientSurface))
              &&SUCCEEDED(d->CreateTexture(w,h,1,D3DUSAGE_RENDERTARGET,D3DFMT_R32F,D3DPOOL_DEFAULT,&ambientFullTexture,nullptr))
              &&SUCCEEDED(ambientFullTexture->GetSurfaceLevel(0,&ambientFullSurface));
        if(!aok){Release(ambientSurface);Release(ambientTexture);Release(ambientFullSurface);Release(ambientFullTexture);++ambientFailures;}
        // Optional: without the history the upsample reads the raw pass, as before.
        if(aok&&ambientTemporalShader){
            bool hok=true;
            for(unsigned i=0;i<2&&hok;++i)hok=SUCCEEDED(d->CreateTexture(ambientWidth,ambientHeight,1,D3DUSAGE_RENDERTARGET,D3DFMT_G32R32F,D3DPOOL_DEFAULT,&ambientHistory[i],nullptr))
                                  &&SUCCEEDED(ambientHistory[i]->GetSurfaceLevel(0,&ambientHistorySurface[i]));
            if(!hok)for(unsigned i=0;i<2;++i){Release(ambientHistorySurface[i]);Release(ambientHistory[i]);}
            ambientHistoryLatched=ambientTemporalLastFrame=false;
        }
    }
    if(!ok){ReleaseTargets();status="Deferred target allocation failed";return false;}
    width=w;height=h;return true;
}
// G-buffer targets held across consecutive primary draws.
//
// Binding RT1..3 and their write masks before every primary draw and putting
// the native ones back after it cost more CPU than the draw itself, and most
// primary draws follow another primary draw. So the targets stay bound until
// something that is not a primary draw happens: a non-primary draw, any
// target, depth or clear call, a state block, the resolve, the end of the
// scene. Every one of those releases them first, and from then on the device
// holds exactly what the per-draw restore used to leave behind. Inside our own
// work (busy) nothing is released, so code that runs within a primary draw sees
// the targets bound, as it always did.
static DWORD mrtNativeMasks[3]={};
static unsigned mrtBinds=0,mrtReleases=0;
static const D3DRENDERSTATETYPE kMrtMasks[3]={D3DRS_COLORWRITEENABLE1,D3DRS_COLORWRITEENABLE2,D3DRS_COLORWRITEENABLE3};
static void DropMrtRefs(){for(auto*&s:mrtNative){if(s)s->Release();s=nullptr;}}
static bool ReleaseMrt(IDirect3DDevice9*d){
    if(!mrtHeld)return true;
    mrtHeld=false;++mrtReleases;bool ok=d!=nullptr;
    if(d){mrtSelf=true;
        for(unsigned i=0;i<3;++i)ok=SUCCEEDED(d->SetRenderTarget(i+1,mrtNative[i]))&&ok;
        for(unsigned i=0;i<3;++i)ok=SUCCEEDED(d->SetRenderState(kMrtMasks[i],mrtNativeMasks[i]))&&ok;
        mrtSelf=false;}
    DropMrtRefs();
    if(!ok){unsafeDeviceState=true;Fault("Primary target release failed; disabled");}
    return ok;
}
static bool BindMrt(IDirect3DDevice9*d){
    if(mrtHeld)return true;
    for(unsigned i=0;i<3;++i){HRESULT hr=d->GetRenderTarget(i+1,&mrtNative[i]);if(FAILED(hr)&&hr!=D3DERR_NOTFOUND){DropMrtRefs();return false;}}
    for(unsigned i=0;i<3;++i)if(FAILED(d->GetRenderState(kMrtMasks[i],&mrtNativeMasks[i]))){DropMrtRefs();return false;}
    mrtHeld=true;++mrtBinds;bool ok=true;mrtSelf=true;
    for(unsigned i=1;i<4;++i)ok=SUCCEEDED(d->SetRenderTarget(i,surfaces[i]))&&ok;
    for(auto mask:kMrtMasks)ok=SUCCEEDED(d->SetRenderState(mask,15))&&ok;
    mrtSelf=false;
    if(!ok){ReleaseMrt(d);return false;}
    return true;
}
// Called by the integration hooks before anything that must see the native targets.
static void ReleaseMrtForNative(IDirect3DDevice9*d){if(mrtHeld&&!mrtSelf&&!busy)ReleaseMrt(d);}
static bool UploadLights(){
    const auto&rows=lightRegistry.Rows();
    if(rows.empty()||rows.size()>size_t(1024)*32*4||rows.size()%(32*4))return false;
    D3DLOCKED_RECT lock={};if(FAILED(lightTexture->LockRect(0,&lock,nullptr,D3DLOCK_DISCARD)))return false;
    for(size_t row=0;row<rows.size()/(32*4);++row)
        memcpy(static_cast<unsigned char*>(lock.pBits)+row*lock.Pitch,rows.data()+row*32*4,32*4*sizeof(float));
    return SUCCEEDED(lightTexture->UnlockRect(0));
}
// control.ini GbufferDump=1: every resolve batch of the next frame that has one
// (up to four) writes what the resolve reads to diagnostics\gbuffer_<pid>_<id>_b<k>.
static uint64_t gbufferDumpId=0;static unsigned gbufferDumpBatch=0;
static void DumpGbuffer(IDirect3DDevice9*d,bool ambientReady){
    WosGbufferDump::Inputs in;
    in.normalShadowSet=surfaces[1];in.rho=surfaces[2];in.specular=surfaces[3];in.residual=sceneColor;
    in.visibility=ambientReady?ambientFullSurface:nullptr;
    in.rows=&lightRegistry.Rows();in.rowFloats=WosDeferredLights::RowFloats;
    in.strength=WosMaterialClasses::Active()?WosMaterialClasses::strength:0.f;
    for(unsigned c=0;c<WosMaterialClasses::ClassCount&&c<WosGbufferDump::MaxClasses;++c)
        for(unsigned i=0;i<4;++i)in.table[c][i]=WosMaterialClasses::table[c][i];
    in.intensity=intensity;in.specularGain=specularIntensity;in.ambient=ambientReady?ambientStrength:0.f;
    in.skyEnabled=WosSkyIBL::enabled;in.skyDiffuse=WosSkyIBL::diffuseStrength;in.skySpecular=WosSkyIBL::specularStrength;in.view=debugView;
    const std::string folder=WosProbe::Root()+"\\full_deferred\\diagnostics";CreateDirectoryA(folder.c_str(),nullptr);
    char stem[80]={};std::snprintf(stem,sizeof(stem),"gbuffer_%lu_%llu",GetCurrentProcessId(),static_cast<unsigned long long>(gbufferDumpId));
    unsigned symbiote=0;WosGbufferDump::Dump(d,in,folder,stem,gbufferDumpBatch,symbiote);++gbufferDumpBatch;
}
// All GPU work happens within a native BeginScene/EndScene. Failure before the
// final copy is transactional, but sceneColor already contains primary residual.
// A failed resolve therefore disables future primary work; it cannot restore light.
static bool Flush(IDirect3DDevice9*d){
    if(busy)return true;
    ReleaseMrt(d);
    if(!pending)return true;
    CpuPhase timer(cpuTimings.flushMs);++cpuTimings.flushCalls;
    WosDeferredGpu::End(d,WosDeferredGpu::SectionPrimaryBatch,batchMark);
    busy=true;WosIndirect::State state(d);bool ok=state.valid&&owner==d&&sceneColor&&depthTexture&&sceneTexture;
    auto check=[&](HRESULT hr){ok=SUCCEEDED(hr)&&ok;};
    if(ok)ok=UploadLights();
    if(ok)WosVolumetric::ObserveLights(lightRegistry.Rows(),WosDeferredLights::RowFloats);
    if(ok){
        for(unsigned i=0;i<8;++i)check(d->SetTexture(i,nullptr));
        check(d->SetDepthStencilSurface(nullptr));for(unsigned i=1;i<state.slots;++i)check(d->SetRenderTarget(i,nullptr));
        check(d->SetRenderTarget(0,resolvedSurface));
        if(ok){WosDeferredGpu::Scope gpu(d,WosDeferredGpu::SectionCopyIn);check(d->StretchRect(sceneColor,nullptr,resolvedSurface,nullptr,D3DTEXF_NONE));}
        check(d->SetVertexShader(nullptr));check(d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1));
        // Native wrapping changes fullscreen UV interpolation even for XYZRHW vertices.
        check(d->SetRenderState(D3DRS_WRAP0,0));
        for(auto rs:{D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHABLENDENABLE,D3DRS_ALPHATESTENABLE,
                     D3DRS_STENCILENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_SRGBWRITEENABLE,D3DRS_FOGENABLE,
                     D3DRS_CLIPPLANEENABLE,D3DRS_SEPARATEALPHABLENDENABLE})check(d->SetRenderState(rs,0));
        check(d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));check(d->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID));
        check(d->SetRenderState(D3DRS_COLORWRITEENABLE,15));check(d->SetRenderState(D3DRS_MULTISAMPLEMASK,0xffffffff));
        check(d->SetRenderState(D3DRS_DITHERENABLE,FALSE));
        check(d->SetPixelShaderConstantF(0,camera.relativeInverse,4));
        // Occlusion runs here, between the geometry and the resolve, because
        // this is the first point at which the depth buffer and the G-buffer
        // normals are both complete and the ambient term has not yet been
        // added to anything. Nothing is reprojected and no frame is delayed.
        //
        // It must also run BEFORE the resolve's own constants and samplers are
        // uploaded, because these two passes share the low constant registers
        // and the first three sampler slots with it. Placed after, they left
        // Screen.z at zero and samplers 0..2 unbound, the id clip then
        // discarded every pixel, and turning occlusion on switched the whole
        // deferred lighting off. Everything the resolve needs is written below.
        bool ambientReady=false;
        if(ok&&ambientShader&&ambientUpsampleShader&&ambientSurface&&ambientFullSurface&&ambientStrength>0.f){
            WosDeferredGpu::Scope gpu(d,WosDeferredGpu::SectionResolve);
            float screenSize[4]={1.f/width,1.f/height,0,0};
            float ao[4]={ambientRadius,1.f,ambientDistance,ambientBias};
            float upsample[4]={1.f/float(ambientWidth),1.f/float(ambientHeight),.03f,0};
            for(unsigned i=0;i<3;++i){
                check(d->SetSamplerState(i,D3DSAMP_MINFILTER,D3DTEXF_POINT));check(d->SetSamplerState(i,D3DSAMP_MAGFILTER,D3DTEXF_POINT));
                check(d->SetSamplerState(i,D3DSAMP_MIPFILTER,D3DTEXF_NONE));check(d->SetSamplerState(i,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));
                check(d->SetSamplerState(i,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));check(d->SetSamplerState(i,D3DSAMP_SRGBTEXTURE,0));
            }
            bool aok=SUCCEEDED(d->SetPixelShaderConstantF(4,screenSize,1))
                   &&SUCCEEDED(d->SetPixelShaderConstantF(5,ao,1))
                   &&SUCCEEDED(d->SetPixelShaderConstantF(6,upsample,1))
                   &&SUCCEEDED(d->SetRenderTarget(0,ambientSurface))
                   &&SUCCEEDED(d->SetTexture(0,depthTexture))&&SUCCEEDED(d->SetTexture(1,gbuffer[1]))
                   &&SUCCEEDED(TrackedSetPixelShader(d,ambientShader));
            // World-space rays against the top-down height map. Unbound, c51.z
            // is zero and the pass is the screen-space estimate alone.
            const bool temporal=ambientTemporalShader&&ambientHistory[0]&&ambientHistory[1];
            // Golden-ratio shift of the slice pattern per frame; the temporal pass
            // integrates it. Without that pass the pattern stays fixed as before.
            const float shiftA=temporal?float(std::fmod(ambientFrameNumber*.618034,1.0)):0.f,shiftB=temporal?float(std::fmod(ambientFrameNumber*.7548776,1.0)):0.f;
            if(aok)WosSkyHeight::Bind(d,camera.worldOrigin,3,shiftA,shiftB);
            if(aok)aok=WosIndirect::Quad(d,ambientWidth,ambientHeight);
            IDirect3DTexture9*upsampleInput=ambientTexture;
            if(aok&&temporal){
                // History from the previous frame's camera: consecutive frame, same
                // scene, no jump. Rebased onto this camera in double precision.
                double delta[3];for(unsigned i=0;i<3;++i)delta[i]=camera.worldOrigin[i]-ambientPrevOrigin[i];
                bool valid=ambientHistoryLatched&&ambientTemporalLastFrame&&ambientPrevScene==camera.scene
                         &&delta[0]*delta[0]+delta[1]*delta[1]+delta[2]*delta[2]<32.*32.;
                float tc[24]={};
                if(valid){WosShadowTransform::Matrix4 m;memcpy(m.v,ambientPrevRelativeInverse,64);double prev[16];valid=WosCascades::Inverse(m,prev);
                    for(unsigned r=0;r<4&&valid;++r)for(unsigned col=0;col<4;++col){double v=prev[r*4+col];if(col==3)v+=prev[r*4]*delta[0]+prev[r*4+1]*delta[1]+prev[r*4+2]*delta[2];if(!std::isfinite(v)){valid=false;break;}tc[r*4+col]=float(v);}}
                tc[16]=valid?1.f:0.f;tc[17]=.15f;tc[18]=.03f;tc[19]=.05f;
                for(unsigned i=0;i<3;++i)tc[20+i]=float(delta[i]);
                const unsigned from=ambientHistoryIndex,to=ambientHistoryIndex^1u;
                check(d->SetSamplerState(3,D3DSAMP_MINFILTER,D3DTEXF_POINT));check(d->SetSamplerState(3,D3DSAMP_MAGFILTER,D3DTEXF_POINT));
                check(d->SetSamplerState(3,D3DSAMP_MIPFILTER,D3DTEXF_NONE));check(d->SetSamplerState(3,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));
                check(d->SetSamplerState(3,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));check(d->SetSamplerState(3,D3DSAMP_SRGBTEXTURE,0));
                aok=SUCCEEDED(d->SetPixelShaderConstantF(7,tc,6))
                    &&SUCCEEDED(d->SetRenderTarget(0,ambientHistorySurface[to]))
                    &&SUCCEEDED(d->SetTexture(0,ambientTexture))&&SUCCEEDED(d->SetTexture(1,depthTexture))
                    &&SUCCEEDED(d->SetTexture(2,ambientHistory[from]))&&SUCCEEDED(d->SetTexture(3,gbuffer[1]))
                    &&SUCCEEDED(TrackedSetPixelShader(d,ambientTemporalShader))
                    &&WosIndirect::Quad(d,ambientWidth,ambientHeight);
                check(d->SetTexture(2,nullptr));check(d->SetTexture(3,nullptr));
                if(aok){ambientHistoryIndex=to;upsampleInput=ambientHistory[to];ambientTemporalThisFrame=true;
                    if(!ambientHistoryLatched||ambientFrameScene!=camera.scene||std::memcmp(ambientFrameRelativeInverse,camera.relativeInverse,64)||
                       ambientFrameOrigin[0]!=camera.worldOrigin[0]||ambientFrameOrigin[1]!=camera.worldOrigin[1]||ambientFrameOrigin[2]!=camera.worldOrigin[2]){
                        memcpy(ambientFrameRelativeInverse,camera.relativeInverse,64);for(unsigned i=0;i<3;++i)ambientFrameOrigin[i]=camera.worldOrigin[i];ambientFrameScene=camera.scene;}
                    if(valid)++ambientTemporalFrames;else ++ambientTemporalResets;}
            }
            if(aok)aok=SUCCEEDED(d->SetRenderTarget(0,ambientFullSurface))
                    &&SUCCEEDED(d->SetTexture(0,upsampleInput))&&SUCCEEDED(d->SetTexture(1,depthTexture))
                    &&SUCCEEDED(d->SetTexture(2,gbuffer[1]))
                    &&SUCCEEDED(TrackedSetPixelShader(d,ambientUpsampleShader))
                    &&WosIndirect::Quad(d,width,height);
            for(unsigned i=0;i<4;++i)aok=SUCCEEDED(d->SetTexture(i,nullptr))&&aok;
            aok=SUCCEEDED(d->SetRenderTarget(0,resolvedSurface))&&aok;
            if(aok){ambientReady=true;++ambientFrames;}else{++ambientFailures;ok=false;}
        }
        float screen[4]={1.f/width,1.f/height,1024.f,float(debugView)};
        float forward[16]={};bool forwardValid=(contactEnabled||rainEnabled)&&InvertMatrix(camera.relativeInverse,forward);bool contact=contactEnabled&&forwardValid;
        if(contactEnabled&&!contact)++contactMatrixFailures;
        float settings[4]={intensity,specularIntensity,contact?contactLength:0.f,contactThickness};
        float contactParams[4]={float(contactSteps),contactStrength,contactBias,lampSpecularCut};
        check(d->SetPixelShaderConstantF(4,screen,1));check(d->SetPixelShaderConstantF(5,settings,1));
        check(d->SetPixelShaderConstantF(6,contactParams,1));
        float contactTune[4]={contactPixels,contactSoftness,contactThicknessGrowth,contactTemporal?float(ambientFrameNumber%64u):0.f};
        check(d->SetPixelShaderConstantF(30,contactTune,1));
        if(forwardValid)check(d->SetPixelShaderConstantF(7,forward,4));
        float rain[4]={rainEnabled?1.f:0.f,rainWetness,rainPuddles,rainRipples};
        float rainOrigin[4]={float(camera.worldOrigin[0]),float(camera.worldOrigin[1]),float(camera.worldOrigin[2]),float(GetTickCount64()%600000)*.001f};
        float rainReflection[4]={rainSize,forwardValid?rainReflections:0.f,60.f,.7f};
        check(d->SetPixelShaderConstantF(40,rain,1));
        check(d->SetPixelShaderConstantF(41,rainOrigin,1));
        check(d->SetPixelShaderConstantF(42,rainReflection,1));
        check(d->SetPixelShaderConstantF(43,WosSkyIBL::material,2));
        // Read by both the resolve and the sky IBL pass that follows it.
        float specularFilter[4]={specularAA,specularAAClamp,specularAAEdge,0};
        check(d->SetPixelShaderConstantF(29,specularFilter,1));
        // Material classes, also read by both passes. Zero strength is the
        // shaders' exact no-op, so an unloaded table uploads zero.
        float classControl[4]={WosMaterialClasses::Active()?WosMaterialClasses::strength:0.f,WosMaterialClasses::rainPull,0,0};
        check(d->SetPixelShaderConstantF(53,classControl,1));
        check(d->SetPixelShaderConstantF(56,&WosMaterialClasses::table[0][0],WosMaterialClasses::ClassCount));
        // Four dynamic cascades, sampled per pixel with PCSS. A false return
        // leaves the mode constant at zero and the resolve on the native term.
        if(ok)WosCascadeShadow::Bind(d,camera.worldOrigin,camera.scene);
        // The cloud deck is an occluder of the sun that no shadow map holds.
        // A false return writes a zero step count, and the resolve then draws
        // exactly what it drew before this module existed.
        if(ok)WosCloudShadow::Bind(d,camera.worldOrigin);
        WosLocalShadow::Bind(d,camera.worldOrigin);
        for(unsigned i=0;i<7;++i){
            check(d->SetSamplerState(i,D3DSAMP_MINFILTER,D3DTEXF_POINT));check(d->SetSamplerState(i,D3DSAMP_MAGFILTER,D3DTEXF_POINT));
            check(d->SetSamplerState(i,D3DSAMP_MIPFILTER,D3DTEXF_NONE));check(d->SetSamplerState(i,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));
            check(d->SetSamplerState(i,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));check(d->SetSamplerState(i,D3DSAMP_SRGBTEXTURE,0));
            check(d->SetTexture(i,i==0?sceneTexture:(i<4?gbuffer[i]:(i==4?depthTexture:(i==5?lightTexture:sceneTexture)))));
        }
        // Zero strength, or no buffer, leaves the resolve reproducing its
        // previous output bit for bit rather than multiplying by a rounded one.
        float ambient[4]={ambientReady?ambientStrength:0.f,0,lampSmooth,0};
        check(d->SetPixelShaderConstantF(28,ambient,1));
        check(d->SetTexture(8,ambientReady?ambientFullTexture:nullptr));
        check(d->SetSamplerState(8,D3DSAMP_MINFILTER,D3DTEXF_POINT));check(d->SetSamplerState(8,D3DSAMP_MAGFILTER,D3DTEXF_POINT));
        check(d->SetSamplerState(8,D3DSAMP_MIPFILTER,D3DTEXF_NONE));check(d->SetSamplerState(8,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));
        check(d->SetSamplerState(8,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));check(d->SetSamplerState(8,D3DSAMP_SRGBTEXTURE,0));
        if(ok&&gbufferDumpId&&gbufferDumpBatch<4)DumpGbuffer(d,ambientReady);
        check(TrackedSetPixelShader(d,resolveShader));
        if(ok){WosDeferredGpu::Scope gpu(d,WosDeferredGpu::SectionResolve);ok=WosIndirect::Quad(d,width,height);}
        if(ok){WosDeferredGpu::Scope gpu(d,WosDeferredGpu::SectionResolve);ok=WosSkyIBL::Render(d,camera.scene,debugView,[&](){return WosIndirect::Quad(d,width,height);},TrackedSetPixelShader);}
        for(unsigned i=0;i<10;++i)check(d->SetTexture(i,nullptr));
        // Restore the native target before committing the completed result.
        bool restored=state.Restore();if(!restored)unsafeDeviceState=true;ok=restored&&ok;
        if(ok){WosDeferredGpu::Scope gpu(d,WosDeferredGpu::SectionCopyOut);check(d->StretchRect(resolvedSurface,nullptr,sceneColor,nullptr,D3DTEXF_NONE));}
    }
    if(state.valid){bool restored=state.Restore();if(!restored)unsafeDeviceState=true;ok=restored&&ok;}
    if(ok){++resolves;faulted=false;status="Primary deferred active (one geometry draw)";if(receiverMerge){WosDeferredGpu::Scope gpu(d,WosDeferredGpu::SectionGiMerge);receiverMerge(d);}}
    else{++failures;Fault("Primary resolve failed; disabled, current frame may retain residual lighting");}
    DropBatch();busy=false;return ok;
}
static bool BeginBatch(IDirect3DDevice9*d,IDirect3DSurface9*color,IDirect3DSurface9*depth,
                       IDirect3DTexture9*texture,const WosIndirectCamera::Camera&cam,UINT w,UINT h){
    if(pending)return sceneColor==color&&sceneDepth==depth&&camera.scene==cam.scene;
    if(!Targets(d,w,h))return false;
    IDirect3DTexture9*nativeTexture=nullptr;
    if(FAILED(color->GetContainer(IID_IDirect3DTexture9,reinterpret_cast<void**>(&nativeTexture))))return false;
    WosIndirect::State state(d);bool ok=state.valid,restored=true;busy=true;
    if(ok){
        ok=SUCCEEDED(d->SetDepthStencilSurface(nullptr));
        for(unsigned i=1;i<state.slots;++i)ok=SUCCEEDED(d->SetRenderTarget(i,nullptr))&&ok;
        ok=SUCCEEDED(d->SetRenderTarget(0,surfaces[1]))&&ok;
        D3DVIEWPORT9 vp={0,0,w,h,0,1};ok=SUCCEEDED(d->SetViewport(&vp))&&ok;
        if(ok)ok=SUCCEEDED(d->Clear(0,nullptr,D3DCLEAR_TARGET,0,1,0));
        restored=state.Restore();ok=restored&&ok;
    }
    busy=false;
    if(!ok){nativeTexture->Release();++failures;if(!restored){unsafeDeviceState=true;Fault("Primary coverage-clear restore failed; disabled until reset");}else status="Primary coverage clear failed before geometry";return false;}
    color->AddRef();depth->AddRef();texture->AddRef();
    sceneColor=color;sceneDepth=depth;depthTexture=texture;sceneTexture=nativeTexture;camera=cam;pending=true;lightRegistry.Clear();
    batchMark=WosDeferredGpu::Begin(d,WosDeferredGpu::SectionPrimaryBatch);return true;
}
// Eligibility verdict reuse ([Cache] VerdictReuse).
//
// Consecutive draws mostly repeat the device state the eligibility check
// reads. The shadow key holds every value the check reads before the pixel
// shader; with the key and every outside input unchanged (device, scene,
// camera table, held targets) the check would read the same values, so a
// previous verdict is reused without the reads, the references and the camera
// lookup. Four verdicts are kept, because primary and fallback draws
// alternate between a few states (targets held or not). The pixel shader is
// compared on its own, and its variant comes from a one-entry memo that any
// change to the variant table voids (verdictEpoch). Every 64th reuse runs the
// full check and compares; a mismatch turns reuse off.
struct VerdictEntry {
    bool valid=false;
    WosDeferredShadow::VerdictKey key;
    IDirect3DDevice9*device=nullptr;uint32_t scene=0;unsigned cameraSerial=0;
    bool frameReady=false,allowPrimary=false,mrtHeld=false;
    IDirect3DSurface9*surfaces[4]={};
    // Verdict of the checks before the pixel shader.
    bool ok=false,depthWriter=false;DWORD zfunc=0;UINT w=0,h=0;
    IDirect3DSurface9*color=nullptr,*depth=nullptr;IDirect3DTexture9*texture=nullptr;
    WosIndirectCamera::Camera cam;
};
struct VerdictCache {
    bool broken=false;unsigned verify=0,last=0,next=0;
    // unkeyed: full checks with reuse on but no key (shadow off or entries unknown).
    unsigned long long hits=0,misses=0,checks=0,mismatches=0,peeks=0,unkeyed=0;
    VerdictEntry entries[4];
    IDirect3DPixelShader9*ps=nullptr;Variant*variant=nullptr;unsigned psEpoch=0;
    void Clear(){for(auto&e:entries)e.valid=false;ps=nullptr;variant=nullptr;psEpoch=0;}
};
static VerdictCache verdictCache;
static bool SameCamera(const WosIndirectCamera::Camera&a,const WosIndirectCamera::Camera&b){
    return !std::memcmp(a.inverse,b.inverse,sizeof(a.inverse))&&!std::memcmp(a.position,b.position,sizeof(a.position))&&
           !std::memcmp(a.relativeInverse,b.relativeInverse,sizeof(a.relativeInverse))&&
           !std::memcmp(a.worldOrigin,b.worldOrigin,sizeof(a.worldOrigin))&&a.scene==b.scene;
}
static VerdictEntry* FindVerdict(IDirect3DDevice9*d,const WosDeferredShadow::VerdictKey&key,uint32_t scene,bool allowPrimary){
    auto&vc=verdictCache;
    for(unsigned n=0;n<4;++n){
        const unsigned i=(vc.last+n)&3u;auto&e=vc.entries[i];
        if(e.valid&&e.device==d&&e.scene==scene&&e.cameraSerial==WosIndirectCamera::serial&&
           e.frameReady==WosIndirectCamera::frameReady&&e.allowPrimary==allowPrimary&&e.mrtHeld==mrtHeld&&
           !std::memcmp(e.surfaces,surfaces,sizeof(e.surfaces))&&WosDeferredShadow::SameKey(e.key,key)){vc.last=i;return &e;}
    }
    return nullptr;
}
// The variant of a pixel shader through the memo. Find may grow the table,
// which moves the epoch, so the epoch is taken after it.
static Variant* ShaderVariant(IDirect3DDevice9*d,IDirect3DPixelShader9*ps){
    auto&vc=verdictCache;
    if(vc.ps==ps&&vc.psEpoch==verdictEpoch)return vc.variant;
    Variant*v=nullptr;{CpuPhase find(cpuTimings.findMs);v=Find(d,ps);}
    vc.ps=ps;vc.variant=v;vc.psEpoch=verdictEpoch;return v;
}
struct DrawScope {
    IDirect3DDevice9*d=nullptr;
    IDirect3DPixelShader9*original=nullptr;
    IDirect3DSurface9 *color=nullptr,*depth=nullptr;
    IDirect3DTexture9*texture=nullptr;
    Variant*variant=nullptr;
    WosIndirectCamera::Camera cam;
    UINT w=0,h=0;DWORD zfunc=0;
    bool eligible=false,depthWriter=false;
    // A reused verdict borrows its pointers: the device keeps the targets and
    // depth bound, and the shader cache holds the pixel shader. Only the full
    // check takes references.
    bool ownTargets=false,ownShader=false;
    // The full check, every read through the shadow with references taken like
    // the real Get. okA is the verdict before the pixel shader (what the cache keeps).
    bool Evaluate(uint32_t scene,bool allowPrimary,bool&okA,UINT&tw,UINT&th){
        ownTargets=ownShader=true;
        DWORD z=0,write=0,blend=0,mask=0,srgb=0,stencil=0;
        D3DSURFACE_DESC cd={},dd={};D3DVIEWPORT9 vp={};
        auto rs=[&](D3DRENDERSTATETYPE type,DWORD&value){return WosDeferredShadow::RenderState(d,type,value);};
        // Same rule as WosIndirectMaterials::OverwriteBlend, read through the shadow.
        auto overwriteBlend=[&](DWORD on){
            if(!on)return true;DWORD src=0,dst=0,op=0,separate=0;
            if(!rs(D3DRS_SRCBLEND,src)||src!=D3DBLEND_ONE||!rs(D3DRS_DESTBLEND,dst)||dst!=D3DBLEND_ZERO||
               !rs(D3DRS_BLENDOP,op)||op!=D3DBLENDOP_ADD||!rs(D3DRS_SEPARATEALPHABLENDENABLE,separate))return false;
            return !separate||(rs(D3DRS_SRCBLENDALPHA,src)&&src==D3DBLEND_ONE&&rs(D3DRS_DESTBLENDALPHA,dst)&&dst==D3DBLEND_ZERO&&
                               rs(D3DRS_BLENDOPALPHA,op)&&op==D3DBLENDOP_ADD);
        };
        bool ok=allowPrimary&&scene&&WosIndirectCamera::frameReady&&
            rs(D3DRS_ZENABLE,z)&&z==D3DZB_TRUE&&
            rs(D3DRS_ZWRITEENABLE,write)&&write&&
            rs(D3DRS_ZFUNC,zfunc)&&zfunc>=D3DCMP_NEVER&&zfunc<=D3DCMP_ALWAYS&&
            rs(D3DRS_ALPHABLENDENABLE,blend)&&overwriteBlend(blend)&&
            rs(D3DRS_COLORWRITEENABLE,mask)&&mask==15&&
            rs(D3DRS_SRGBWRITEENABLE,srgb)&&!srgb&&
            rs(D3DRS_STENCILENABLE,stencil)&&!stencil&&
            WosDeferredShadow::RenderTarget(d,0,color)&&color&&WosDeferredShadow::TargetDesc(color,cd)&&
            WosDeferredShadow::DepthStencil(d,depth)&&depth&&WosDeferredShadow::DepthDesc(depth,dd)&&
            cd.Format==D3DFMT_A16B16G16R16F&&dd.Format==D3DFORMAT(MAKEFOURCC('I','N','T','Z'))&&
            cd.MultiSampleType==D3DMULTISAMPLE_NONE&&dd.MultiSampleType==D3DMULTISAMPLE_NONE&&
            cd.Width==dd.Width&&cd.Height==dd.Height&&cd.Width>=640&&cd.Height>=200&&cd.Width<=4096&&cd.Height<=4096&&
            WosDeferredShadow::Viewport(d,vp)&&!vp.X&&!vp.Y&&vp.Width==cd.Width&&vp.Height==cd.Height&&vp.MinZ==0&&vp.MaxZ==1&&
            WosDeferredShadow::DepthContainer(depth,texture)&&
            WosIndirectCamera::Find(texture,cd.Width,cd.Height,cam,scene);
        depthWriter=z==D3DZB_TRUE&&write!=0;
        if(ok)for(unsigned i=1;i<4;++i){
            IDirect3DSurface9*aux=nullptr;if(!WosDeferredShadow::RenderTarget(d,i,aux))ok=false;
            if(aux){if(!(mrtHeld&&aux==surfaces[i]))ok=false;aux->Release();}
        }
        okA=ok;tw=cd.Width;th=cd.Height;
        if(ok)ok=WosDeferredShadow::PixelShader(d,original)&&original;
        if(ok){CpuPhase find(cpuTimings.findMs);variant=Find(d,original);ok=variant!=nullptr;}
        return ok;
    }
    explicit DrawScope(IDirect3DDevice9*device,bool allowPrimary=true):d(device){
        if(busy||WosProbe::suppress)return;
        if(!enabled){Flush(d);return;}
        CpuPhase timing(cpuTimings.eligibilityMs,&cpuTimings.flushMs);++cpuTimings.eligibilityCalls;
        // Every read below is a shadow read: identical to the real Get, but
        // free when the setter hooks have kept the entry current.
        WosDeferredShadow::Tick(d);
        uint32_t scene=WosIndirectCamera::CurrentScene();
        auto&vc=verdictCache;
        WosDeferredShadow::VerdictKey key;
        const bool reuse=verdictReuse&&!vc.broken;
        const bool keyed=reuse&&WosDeferredShadow::Key(key);
        VerdictEntry*hit=keyed?FindVerdict(d,key,scene,allowPrimary):nullptr;
        bool ok=false;UINT tw=0,th=0;
        if(hit&&(++vc.verify&63u)){
            ++vc.hits;ok=hit->ok;depthWriter=hit->depthWriter;
            if(ok){
                color=hit->color;depth=hit->depth;texture=hit->texture;cam=hit->cam;zfunc=hit->zfunc;tw=hit->w;th=hit->h;
                // Always known here: the key requires the shader entry.
                ok=WosDeferredShadow::PeekPixelShader(original)&&original;
                if(ok){variant=ShaderVariant(d,original);ok=variant!=nullptr;}
            }
        }else{
            bool okA=false;ok=Evaluate(scene,allowPrimary,okA,tw,th);
            if(hit){
                // Every 64th reuse: the full check must agree with the cache.
                ++vc.checks;
                bool agree=okA==hit->ok&&depthWriter==hit->depthWriter;
                if(agree&&okA)agree=color==hit->color&&depth==hit->depth&&texture==hit->texture&&zfunc==hit->zfunc&&
                                    tw==hit->w&&th==hit->h&&SameCamera(cam,hit->cam);
                if(agree&&okA&&original&&vc.ps==original&&vc.psEpoch==verdictEpoch)agree=variant==vc.variant;
                if(!agree){++vc.mismatches;vc.broken=true;vc.Clear();}
            }else if(reuse){
                if(keyed)++vc.misses;else ++vc.unkeyed;
                // Keyed after the full check, whose reads filled the shadow, so an
                // entry is stored even when no key could be built before it.
                WosDeferredShadow::VerdictKey filled;
                if(WosDeferredShadow::Key(filled)){
                    const unsigned i=vc.next;vc.next=(vc.next+1)&3u;auto&e=vc.entries[i];
                    e.valid=true;e.key=filled;
                    vc.last=i;e.device=d;e.scene=scene;e.cameraSerial=WosIndirectCamera::serial;
                    e.frameReady=WosIndirectCamera::frameReady;e.allowPrimary=allowPrimary;e.mrtHeld=mrtHeld;
                    std::memcpy(e.surfaces,surfaces,sizeof(e.surfaces));
                    e.ok=okA;e.depthWriter=depthWriter;e.zfunc=zfunc;e.w=tw;e.h=th;
                    e.color=okA?color:nullptr;e.depth=okA?depth:nullptr;e.texture=okA?texture:nullptr;e.cam=cam;
                }
            }
            if(reuse&&okA&&original&&!vc.broken){vc.ps=original;vc.variant=variant;vc.psEpoch=verdictEpoch;}
        }
        if(!ok){ReleaseMrt(d);Flush(d);if(depthWriter)InvalidateReceivers();++fallbackDraws;return;}
        if(pending&&(sceneColor!=color||sceneDepth!=depth||camera.scene!=cam.scene||
                    memcmp(camera.relativeInverse,cam.relativeInverse,sizeof(cam.relativeInverse))||
                    memcmp(camera.worldOrigin,cam.worldOrigin,sizeof(cam.worldOrigin))))Flush(d);
        w=tw;h=th;eligible=enabled;
    }
    bool Handled()const{return eligible&&enabled;}
    bool CurrentBindings(){
        // Pointer comparisons need no references: borrowed shadow values when known.
        IDirect3DSurface9*rt[4]={},*zs=nullptr;IDirect3DPixelShader9*p=nullptr;
        if(verdictReuse&&!verdictCache.broken&&WosDeferredShadow::PeekBindings(rt,zs,p)){
            ++verdictCache.peeks;
            bool ok=rt[0]==color&&zs==depth&&p==original;
            for(unsigned i=1;i<4&&ok;++i)ok=rt[i]==nullptr||(mrtHeld&&rt[i]==surfaces[i]);
            return ok;
        }
        IDirect3DSurface9*c=nullptr,*z=nullptr;IDirect3DPixelShader9*ps=nullptr;
        bool ok=WosDeferredShadow::RenderTarget(d,0,c)&&c==color&&
            WosDeferredShadow::DepthStencil(d,z)&&z==depth&&
            WosDeferredShadow::PixelShader(d,ps)&&ps==original;
        Release(c);Release(z);Release(ps);
        for(unsigned i=1;i<4&&ok;++i){IDirect3DSurface9*a=nullptr;ok=WosDeferredShadow::RenderTarget(d,i,a)&&(a==nullptr||(mrtHeld&&a==surfaces[i]));Release(a);}
        return ok;
    }
    template<class Draw> HRESULT Execute(Draw&&nativeDraw){
        // This callback is the actual native geometry submission. It is never
        // replayed after a successful draw, including failure recovery.
        if(unsafeDeviceState)return D3DERR_INVALIDCALL;
        if(!Handled()){ReleaseMrtForNative(d);return nativeDraw();}
        bool bound;{CpuPhase bindings(cpuTimings.bindingsMs);bound=CurrentBindings();}
        if(!bound){
            ReleaseMrt(d);Flush(d);InvalidateReceivers();++fallbackDraws;eligible=false;if(unsafeDeviceState)return D3DERR_INVALIDCALL;return nativeDraw();
        }
        WosDeferredLights::LightSet lights;bool hasLights;
        {CpuPhase read(cpuTimings.lightsMs);hasLights=variant->supported&&ReadLights(d,variant->lights,cam.worldOrigin,lights,variant->pointCountMultiple);
         if(hasLights){WosCloudSunSync::Observe(lights.texels.data());WosCloudLight::ApplyToSun(lights.texels.data());WosNightLamps::Append(lights.texels.data(),cam.worldOrigin);WosLocalShadow::Observe(lights,cam.worldOrigin);
          // After the local-shadow observer, so shadow maps keep the native range.
          if(lampRadiusScale!=1.f||lampDim!=0.f){const unsigned count=unsigned(lights.texels[3]);const float inv=1.f/(std::max)(lampRadiusScale,.01f),gain=1.f-lampDim;
           for(unsigned k=0;k<count&&k<WosDeferredLights::MaxPoints;++k){lights.texels[(2+2*k)*4+3]*=inv;for(unsigned j=0;j<3;++j)lights.texels[(3+2*k)*4+j]*=gain;}}}}
        if(hasLights&&rainEnabled)lights.texels[31*4]=variant->rainClass;
        // Material class beside the rain class; written only while active, so
        // an inactive table leaves the light sets exactly as they were.
        if(hasLights&&WosMaterialClasses::Active()){
            if(variant->diffuseCount<0)variant->diffuseCount=int(WosMaterialClasses::DiffuseSamplers(variant->input,variant->diffuseRegs));
            lights.texels[31*4+1]=float(WosMaterialClasses::ClassOf(d,variant->diffuseRegs,unsigned(variant->diffuseCount)));
        }
        auto intern=[&](uint16_t&slot){CpuPhase phase(cpuTimings.internMs);return lightRegistry.Intern(lights,slot);};
        if(!BeginBatch(d,color,depth,texture,cam,w,h)){
            ReleaseMrt(d);if(unsafeDeviceState)return D3DERR_INVALIDCALL;
            InvalidateReceivers();++fallbackDraws;if(unsafeDeviceState)return D3DERR_INVALIDCALL;return nativeDraw();
        }
        uint16_t id=0;
        if(hasLights&&!intern(id)){
            // Commit previous primary pixels before recycling the light table.
            // Dropping them here would leave their residual permanently unlit.
            if(!Flush(d)||!enabled||!BeginBatch(d,color,depth,texture,cam,w,h)||!intern(id)){
                ReleaseMrt(d);InvalidateReceivers();++fallbackDraws;if(unsafeDeviceState)return D3DERR_INVALIDCALL;return nativeDraw();
            }
        }
        // Per draw only the pixel shader and the light-set tag change; the
        // G-buffer targets are held (BindMrt) until something else happens.
        LONGLONG start=CpuTick();float savedTag[4]={};
        bool ok=!variant->supported||SUCCEEDED(d->GetPixelShaderConstantF(221,savedTag,1));
        cpuTimings.captureMs+=CpuElapsed(start);busy=true;
        auto check=[&](HRESULT hr){ok=SUCCEEDED(hr)&&ok;};
        start=CpuTick();
        bool shaderSet=false;
        if(ok){
            ok=BindMrt(d);
            if(ok&&variant->supported){float tag[4]={float(id),0,0,0};check(d->SetPixelShaderConstantF(221,tag,1));}
            if(ok){check(TrackedSetPixelShader(d,variant->shader));shaderSet=true;}
        }
        cpuTimings.bindMs+=CpuElapsed(start);
        auto restoreDraw=[&](){
            bool r=true;
            if(shaderSet)r=SUCCEEDED(TrackedSetPixelShader(d,original))&&r;
            if(variant->supported)r=SUCCEEDED(d->SetPixelShaderConstantF(221,savedTag,1))&&r;
            return r;
        };
        if(!ok){
            bool restored=restoreDraw();busy=false;restored=ReleaseMrt(d)&&restored;
            if(!restored){++failures;unsafeDeviceState=true;Fault("Primary setup restore failed; disabled");return D3DERR_INVALIDCALL;}
            Flush(d);InvalidateReceivers();++fallbackDraws;if(unsafeDeviceState)return D3DERR_INVALIDCALL;return nativeDraw();
        }
        if(receiverWrite)receiverWrite();
        start=CpuTick();HRESULT hr=nativeDraw();cpuTimings.replayMs+=CpuElapsed(start);++cpuTimings.replayCalls;
        start=CpuTick();bool restored=restoreDraw();busy=false;
        // Trace captures and the native RSM writer render around the draw from
        // outer scopes; with either active the targets go back at once. Plain GI
        // keeps them held (the hooks release before the GI material scope).
        if((WosIndirect::enabled&&(!giHoldMrt||WosIndirect::nativeRsmCapture))||WosProbe::trace)restored=ReleaseMrt(d)&&restored;
        cpuTimings.restoreMs+=CpuElapsed(start);
        if(SUCCEEDED(hr)){
            ++receiverGeneration;if(hasLights)++supportedDraws;else ++guardDraws;
        }
        if(FAILED(hr)||!restored){
            ++failures;if(!restored)unsafeDeviceState=true;
            // Resolve previous successful pixels if possible, then fall back on
            // subsequent draws/frames. Never duplicate the real draw to recover.
            if(restored){ReleaseMrt(d);Flush(d);}Fault("Primary draw or restore failed; disabled without geometry replay");
        }
        return hr;
    }
    ~DrawScope(){CpuPhase release(cpuTimings.releaseMs);if(ownShader)Release(original);if(ownTargets){Release(color);Release(depth);Release(texture);}}
};
static void WriteStatus(bool force=false){
    ULONGLONG now=GetTickCount64();if(!force&&now-lastStatus<1000)return;lastStatus=now;
    FILE*f=std::fopen((WosProbe::Root()+"\\full_deferred_status.json").c_str(),"wb");if(!f)return;
    std::fprintf(f,"{\"revision\":3,\"pipeline\":\"primary_geometry\",\"geometry_replays\":0,\"enabled\":%s,\"status\":\"%s\",\"frames\":%u,\"observed_present_fps\":%.3f,\"supported_draws\":%u,\"guard_draws\":%u,\"fallback_draws\":%u,\"resolve_batches\":%u,\"failures\":%u,\"width\":%u,\"height\":%u,\"intensity\":%.4f,\"specular\":%.4f,\"debug_view\":%d,\"contact\":{\"enabled\":%s,\"length\":%.4f,\"thickness\":%.4f,\"steps\":%d,\"strength\":%.4f,\"bias\":%.4f,\"matrix_failures\":%u},\"full_material_coverage\":false,\"native_fallback_preserved\":false,\"unsupported_native_fallback\":true,",
        enabled?"true":"false",status,frames,observedPresentFps,lastSupported,lastGuards,lastFallback,lastResolves,failures,width,height,intensity,specularIntensity,debugView,contactEnabled?"true":"false",contactLength,contactThickness,contactSteps,contactStrength,contactBias,contactMatrixFailures);
    std::fprintf(f,"\"eligibility_cpu_ms\":%.4f,\"replay_capture_cpu_ms\":%.4f,\"replay_bind_cpu_ms\":%.4f,\"replay_draw_cpu_ms\":%.4f,\"replay_restore_cpu_ms\":%.4f,\"flush_cpu_ms\":%.4f,\"find_cpu_ms\":%.4f,\"bindings_cpu_ms\":%.4f,\"lights_read_cpu_ms\":%.4f,\"light_intern_cpu_ms\":%.4f,\"scope_release_cpu_ms\":%.4f,\"state_blocks\":{\"creates\":%u,\"begins\":%u,\"ends\":%u,\"applies\":%u,\"applies_during_batch\":%u},\"mrt_binds\":%u,\"mrt_releases\":%u,\"gi_hold_mrt\":%d,\"light_reuse\":{\"enabled\":%d,\"broken\":%d,\"hits\":%llu,\"misses\":%llu,\"checks\":%llu,\"mismatches\":%llu},\"verdict_reuse\":{\"enabled\":%d,\"broken\":%d,\"hits\":%llu,\"misses\":%llu,\"checks\":%llu,\"mismatches\":%llu,\"peeks\":%llu,\"unkeyed\":%llu},\"eligibility_calls\":%u,\"replay_calls\":%u,\"flush_calls\":%u,\"cpu_timing_scope\":\"last_frame_submission_wall_time_not_gpu_duration\",",
        lastCpuTimings.eligibilityMs,lastCpuTimings.captureMs,lastCpuTimings.bindMs,lastCpuTimings.replayMs,lastCpuTimings.restoreMs,lastCpuTimings.flushMs,lastCpuTimings.findMs,lastCpuTimings.bindingsMs,lastCpuTimings.lightsMs,lastCpuTimings.internMs,lastCpuTimings.releaseMs,stateBlockCreates,stateBlockBegins,stateBlockEnds,stateBlockApplies,stateBlockAppliesDuringBatch,mrtBinds,mrtReleases,giHoldMrt?1:0,lightReuse?1:0,lightCache.broken?1:0,lightCache.hits,lightCache.misses,lightCache.checks,lightCache.mismatches,verdictReuse?1:0,verdictCache.broken?1:0,verdictCache.hits,verdictCache.misses,verdictCache.checks,verdictCache.mismatches,verdictCache.peeks,verdictCache.unkeyed,lastCpuTimings.eligibilityCalls,lastCpuTimings.replayCalls,lastCpuTimings.flushCalls);
    std::fprintf(f,"\"sky_ibl\":{\"enabled\":%s,\"valid\":%s,\"profile\":%d,\"captures\":%u,\"binds\":%u,\"failures\":%u,\"sky_scene\":%u,\"resolve_scene\":%u},",WosSkyIBL::enabled?"true":"false",WosSkyIBL::valid?"true":"false",WosSkyIBL::profile,WosSkyIBL::captures,WosSkyIBL::binds,WosSkyIBL::failures,WosSkyIBL::skyScene,WosSkyIBL::resolveScene);
    std::fprintf(f,"\"sky_height\":{\"enabled\":%s,\"valid\":%s,\"renders\":%u,\"last_frame_renders\":%u,\"binds\":%u,\"failures\":%u,\"extent\":%.1f,\"ray_length\":%.1f,\"status\":\"%s\"},",WosSkyHeight::enabled?"true":"false",WosSkyHeight::currentValid?"true":"false",WosSkyHeight::renders,WosSkyHeight::lastRenders,WosSkyHeight::binds,WosSkyHeight::failures,WosSkyHeight::extent,WosSkyHeight::rayLength,WosSkyHeight::status);
    std::fprintf(f,"\"cascades\":{\"enabled\":%s,\"mode\":%d,\"resolution\":%d,\"distance\":%.2f,\"sun_angle\":%.4f,\"search_distance\":%.2f,\"depth_bias\":%.4f,\"normal_bias\":%.3f,\"strength\":%.3f,\"tile\":%u,\"stagger\":%d,\"updated_cascades_last_frame\":%u,\"stagger_forced\":%u,\"tile_renders_last_frame\":%u,\"resolve_binds_last_frame\":%u,\"scene_mismatches\":%u,\"failures\":%u,\"replayed_source_draws\":%u,\"skipped_shader_draws\":%u,\"skipped_instance_draws\":%u,\"eye_distance\":%.2f,\"maps_scene\":%u,\"camera_scene\":%u,\"probe_prepare_entries\":%u,\"probe_prepare_calls\":%u,\"probe_ready\":%u,\"probe_failed\":%u,\"probe_status\":\"%s\",\"status\":\"%s\"},",
        WosCascadeShadow::enabled?"true":"false",WosCascadeShadow::mode,WosCascadeShadow::resolution,WosCascadeShadow::distance,
        WosCascadeShadow::sunAngle,WosCascadeShadow::searchDistance,WosCascadeShadow::depthBias,WosCascadeShadow::normalBias,
        WosCascadeShadow::strength,WosCascadeShadow::tile,WosCascadeShadow::stagger,WosCascadeShadow::staggerUpdated,WosCascadeShadow::staggerForced,WosCascadeShadow::lastRenders,WosCascadeShadow::lastBinds,
        WosCascadeShadow::sceneSkips,WosCascadeShadow::failures,WosCascadeShadow::sourceDraws,WosCascadeShadow::skippedShader,
        WosCascadeShadow::skippedInstance,WosCascadeShadow::lastEyeDistance,WosCascadeShadow::mapsScene,
        WosCascadeShadow::lastCameraScene,WosCascadeShadow::probeEntries,WosCascadeShadow::probeCalls,
        WosCascadeShadow::probeReady,WosCascadeShadow::probeFailed,WosCascadeShadow::probeStatus,
        WosCascadeShadow::status);
    std::fprintf(f,"\"local_shadows\":{\"enabled\":%s,\"valid\":%s,\"lights\":%u,\"resolution\":%u,\"draws\":%u,\"skipped_casters\":%u,\"failures\":%u,\"source\":\"native_near_and_far_shadow_queues\",\"status\":\"%s\",\"static_layer\":{\"enabled\":%s,\"shader_ready\":%s,\"usable\":%s,\"held_draws_last_frame\":%u,\"static_draws_last_frame\":%u,\"live_draws_last_frame\":%u,\"keys\":[%u,%u],\"worlds\":[%u,%u],\"rebuilds\":%llu,\"rebuild_light\":%llu,\"rebuild_moved\":%llu,\"rebuild_unseen\":%llu,\"rebuild_overflow\":%llu,\"status\":\"%s\"}},",
        WosLocalShadow::enabled?"true":"false",WosLocalShadow::valid?"true":"false",WosLocalShadow::count,WosLocalShadow::tile,
        WosLocalShadow::draws,WosLocalShadow::skipped,WosLocalShadow::failures,WosLocalShadow::status,
        WosLocalShadow::staticEnabled?"true":"false",WosLocalShadow::staticShaderReady?"true":"false",WosLocalShadow::StaticUsable()?"true":"false",
        WosLocalShadow::lastSkippedDraws,WosLocalShadow::lastStaticDraws,WosLocalShadow::lastLiveDraws,
        unsigned(WosLocalShadow::slots[0].seen.size()),unsigned(WosLocalShadow::slots[1].seen.size()),
        unsigned(WosLocalShadow::slots[0].worldList.size()),unsigned(WosLocalShadow::slots[1].worldList.size()),
        WosLocalShadow::staticRebuilds,WosLocalShadow::rebuildLight,WosLocalShadow::rebuildMoved,WosLocalShadow::rebuildUnseen,WosLocalShadow::rebuildOverflow,
        WosLocalShadow::staticStatus);
    std::fprintf(f,"\"cloud_light\":{\"enabled\":%s,\"blocking\":%.3f,\"coverage\":%.3f,\"opacity\":%.3f,\"source_valid\":%s,\"sun_transmittance\":%.4f,\"light_sets_scaled_last_frame\":%u,\"model\":\"artistic_curve_not_sampled_cloud_optical_depth\",\"status\":\"%s\"},",
        WosCloudLight::enabled?"true":"false",WosCloudLight::blocking,WosCloudLight::coverage,WosCloudLight::opacity,
        WosCloudLight::sourceValid?"true":"false",WosCloudLight::Transmittance(),WosCloudLight::lastApplied,
        WosCloudLight::Status());
    std::fprintf(f,"\"cloud_shadow\":{\"enabled\":%s,\"strength\":%.3f,\"steps\":%d,\"minimum\":%.3f,\"compensate\":%s,\"compensation\":%.3f,\"source_valid\":%s,\"coverage\":%.3f,\"opacity\":%.3f,\"bottom\":%.1f,\"thickness\":%.1f,\"size\":%.1f,\"detail\":%.3f,\"wind\":%.4f,\"resolve_binds_last_frame\":%u,\"skips\":%u,\"failures\":%u,\"model\":\"per_pixel_march_of_the_same_procedural_cloud_density\",\"status\":\"%s\"},",
        WosCloudShadow::enabled?"true":"false",WosCloudShadow::Strength(),WosCloudShadow::steps,WosCloudShadow::Minimum(),
        WosCloudShadow::compensate?"true":"false",WosCloudShadow::Compensation(),
        WosCloudShadow::sourceValid?"true":"false",WosCloudShadow::coverage,WosCloudShadow::opacity,
        WosCloudShadow::bottom,WosCloudShadow::thickness,WosCloudShadow::size,WosCloudShadow::detail,WosCloudShadow::wind,
        WosCloudShadow::lastBinds,WosCloudShadow::skips,WosCloudShadow::failures,WosCloudShadow::Status());
    std::fprintf(f,"\"height_fog\":{\"enabled\":%s,\"base_height\":%.3f,\"falloff\":%.6f},",WosVolumetric::fogHeightEnabled?"true":"false",WosVolumetric::fogBaseHeight,WosVolumetric::fogHeightFalloff);
    std::fprintf(f,"\"atmospheric_fog\":{\"enabled\":%s,\"density\":%.6f,\"start\":%.2f,\"distance\":%.2f,\"colour\":[%.4f,%.4f,%.4f]},",
        WosVolumetric::fogEnabled?"true":"false",WosVolumetric::fogDensity,WosVolumetric::fogStart,WosVolumetric::fogDistance,
        WosVolumetric::fogColour[0],WosVolumetric::fogColour[1],WosVolumetric::fogColour[2]);
    std::fprintf(f,"\"volumetric\":{\"enabled\":%s,\"steps\":%d,\"distance\":%.2f,\"density\":%.5f,\"strength\":%.3f,\"anisotropy\":%.3f,\"rejection\":%.3f,\"view\":%d,\"rendered_frames_last_frame\":%u,\"failures\":%u,\"restore_failures\":%u,\"no_cascade\":%u,\"no_sun\":%u,\"sun_valid\":%s,\"sun_direction\":[%.4f,%.4f,%.4f],\"sun_colour\":[%.4f,%.4f,%.4f],\"march_width\":%u,\"march_height\":%u,\"scattering\":\"single_scattering_cascade_march\",\"status\":\"%s\"},",
        WosVolumetric::enabled?"true":"false",WosVolumetric::steps,WosVolumetric::distance,WosVolumetric::density,
        WosVolumetric::strength,WosVolumetric::anisotropy,WosVolumetric::rejection,WosVolumetric::debugView,
        WosVolumetric::lastFrames,WosVolumetric::failures,WosVolumetric::restoreFailures,WosVolumetric::noCascade,
        WosVolumetric::noSun,WosVolumetric::sunValid?"true":"false",
        WosVolumetric::sunDirection[0],WosVolumetric::sunDirection[1],WosVolumetric::sunDirection[2],
        WosVolumetric::sunColour[0],WosVolumetric::sunColour[1],WosVolumetric::sunColour[2],
        WosVolumetric::marchWidth,WosVolumetric::marchHeight,WosVolumetric::status);
    std::fprintf(f,"\"lamp_shafts\":{\"enabled\":%s,\"max_lights\":%d,\"density\":%.5f,\"strength\":%.3f,\"core\":%.3f,\"radius_scale\":%.3f,\"shadow_steps\":%d,\"max_distance\":%.1f,\"observed_lights\":%u,\"drawn_lights\":%u,\"shadowed_lights\":%u,\"rendered_frames_last_frame\":%u,\"sky_glow_binds\":%lu,\"cascades_oriented_by_sun\":%s,\"orient\":[%lu,%lu,%lu,%lu,%lu],\"orient_native_z\":[%.3f,%.3f,%.3f],\"orient_z\":[%.3f,%.3f,%.3f],\"lights\":[",
        WosVolumetric::localEnabled?"true":"false",WosVolumetric::localMaxLights,WosVolumetric::localDensity,WosVolumetric::localStrength,
        WosVolumetric::localCore,WosVolumetric::localRadiusScale,WosVolumetric::localShadowSteps,WosVolumetric::localMaxDistance,
        WosVolumetric::lastShaftObserved,WosVolumetric::lastShaftCount,WosVolumetric::lastShaftShadowed,WosVolumetric::lastLocalFrames,
        WosVolumetric::skyBinds,WosCascadeProbe::orientedBySun?"true":"false",
        WosCascadeProbe::orientCalls,WosCascadeProbe::orientNoFn,WosCascadeProbe::orientNoSun,WosCascadeProbe::orientBadBasis,WosCascadeProbe::orientOk,
        WosCascadeProbe::orientNativeZ[0],WosCascadeProbe::orientNativeZ[1],WosCascadeProbe::orientNativeZ[2],WosCascadeProbe::orientZ[0],WosCascadeProbe::orientZ[1],WosCascadeProbe::orientZ[2]);
    for(unsigned i=0;i<WosVolumetric::lastShaftCount&&i<WosVolumetric::kShaftLights;++i){
        const auto&l=WosVolumetric::shaftLights[i];
        std::fprintf(f,"%s{\"p\":[%.2f,%.2f,%.2f],\"radius\":%.2f,\"rgb\":[%.3f,%.3f,%.3f],\"cube\":%d}",i?",":"",
            l.p[0],l.p[1],l.p[2],l.radius,l.colour[0],l.colour[1],l.colour[2],int(l.slot));
    }
    std::fprintf(f,"]},");
    WosShadowWork::WriteJson(f);
    WosHookProfile::WriteJson(f);
    WosShadowFastGlobals::WriteJson(f);
    WosDeferredGpu::WriteJson(f);
    std::fprintf(f,",");WosDeferredShadow::WriteJson(f);
    std::fprintf(f,"}\n");
    std::fclose(f);
}
static void FrameEnd(){
    WosSkyIBL::FrameEnd();
    // The camera this frame's visibility history was written with becomes the
    // previous camera of the next frame; a frame without the pass breaks it.
    if(ambientTemporalThisFrame){memcpy(ambientPrevRelativeInverse,ambientFrameRelativeInverse,64);for(unsigned i=0;i<3;++i)ambientPrevOrigin[i]=ambientFrameOrigin[i];ambientPrevScene=ambientFrameScene;ambientHistoryLatched=true;}
    ambientTemporalLastFrame=ambientTemporalThisFrame;ambientTemporalThisFrame=false;++ambientFrameNumber;
    // Present cadence over a wall-clock window; this is not a GPU duration.
    ULONGLONG now=GetTickCount64();
    if(!presentWindowStart)presentWindowStart=now;
    ++presentWindowFrames;
    if(now-presentWindowStart>=1000){
        observedPresentFps=double(presentWindowFrames)*1000.0/double(now-presentWindowStart);
        presentWindowStart=now;presentWindowFrames=0;
    }
    if(enabled)++frames;
    if(gbufferDumpBatch){gbufferDumpId=0;gbufferDumpBatch=0;}
    lastCpuTimings=cpuTimings;cpuTimings={};WosMaterialClasses::EndFrame();WosDeferredShadow::FrameEnd();WosCascadeShadow::FrameEnd();WosSkyHeight::FrameEnd();WosVolumetric::FrameEnd();WosCloudLight::FrameEnd();WosCloudShadow::FrameEnd();
    lastSupported=supportedDraws;lastGuards=guardDraws;lastFallback=fallbackDraws;lastResolves=resolves;
    supportedDraws=guardDraws=fallbackDraws=resolves=0;
    // EndScene owns the final flush. No DrawPrimitiveUP is legal here after it.
    if(pending){DropBatch();++failures;Fault("Primary missing EndScene barrier; disabled, current frame retains residual");}
    ++frameSerial;InvalidateReceivers();
    if(enabled||faulted)WriteStatus();else status="Deferred direct lighting OFF";
}
}
