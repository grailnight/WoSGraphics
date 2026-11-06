#pragma once
// Where the draw hook spends its time, segment by segment. Off unless
// [Profiler] Hooks=1; when off every mark is one predictable branch.
// Ticks are TSC cycles, converted to milliseconds per frame when written.
#include <intrin.h>
#include <cstdio>
namespace WosHookProfile {
enum Segment {
    Smaa,MaterialContext,NativeRsm,CameraObserve,ShaderConstants,PreScopes,Deferred,IndirectMaterials,
    BloomPasses,IndirectProbe,CascadeReplay,LocalReplay,PostScopes,Execute,Native,Destructors,
    SetterNative,SetterOurs,
    CascadePre,CascadeCull,CascadeBind,CascadeDraw,CascadeEnd,Count
};
static const char*const names[Count]={"smaa","material_context","native_rsm","camera_observe","shader_constants",
    "pcss_normals_cascade_material","deferred_scope","indirect_materials","bloom_passes","indirect_probe",
    "cascade_replay","local_replay","rsm_soft_taa_scopes","execute_chain","native_draw","destructors",
    "setters_native","setters_ours",
    "cascade.pre","cascade.cull","cascade.bind","cascade.draw","cascade.end"};
static bool enabled=false;
static unsigned depth=0;
static unsigned long long acc[Count]={},last=0;
static unsigned long long draws=0,frames=0;
static bool inside=false;  // a profiled draw is open
static unsigned long long windowTsc=0;static LARGE_INTEGER windowQpc={};
struct Draw {
    bool on=false;
    Draw(){on=enabled&&depth==0;++depth;if(on){++draws;inside=true;last=__rdtsc();}}
    ~Draw(){--depth;if(on)inside=false;}
    void Mark(Segment s){if(on){unsigned long long t=__rdtsc();acc[s]+=t-last;last=t;}}
};
// A sub-segment inside a draw segment; the draw's own mark takes the rest.
static void Sub(Segment s){if(inside&&depth==1){unsigned long long t=__rdtsc();acc[s]+=t-last;last=t;}}
// Wraps the native call so that its own time is separated from the chain.
template<class F> static HRESULT TimeNative(F&&f){
    if(!enabled||depth!=1)return f();
    unsigned long long t=__rdtsc();HRESULT hr=f();acc[Native]+=__rdtsc()-t;return hr;
}
// Setter hooks: the native call and our own work around it, outside draws only.
struct Setter {
    bool on=false;unsigned long long t=0;
    Setter(){on=enabled&&depth==0;if(on)t=__rdtsc();}
    void Native(){if(on){unsigned long long n=__rdtsc();acc[SetterNative]+=n-t;t=n;}}
    ~Setter(){if(on)acc[SetterOurs]+=__rdtsc()-t;}
};
static void EndFrame(){if(enabled)++frames;}
static void WriteJson(FILE*f){
    LARGE_INTEGER now,freq;QueryPerformanceCounter(&now);QueryPerformanceFrequency(&freq);
    unsigned long long tsc=__rdtsc();
    double ms=windowQpc.QuadPart?1000.0*double(now.QuadPart-windowQpc.QuadPart)/double(freq.QuadPart):0;
    double tscPerMs=ms>0?double(tsc-windowTsc)/ms:0;
    std::fprintf(f,"\"hook_profile\":{\"enabled\":%s,\"frames\":%llu,\"draws_per_frame\":%.1f,\"ms_per_frame\":{",enabled?"true":"false",frames,frames?double(draws)/frames:0.0);
    for(int i=0;i<Count;++i){
        double v=(frames&&tscPerMs>0)?double(acc[i])/tscPerMs/double(frames):0;
        // The chain's own share: what remains once the native draw inside it is taken out.
        if(i==Execute&&frames&&tscPerMs>0)v=double(acc[Execute]-(acc[Native]<acc[Execute]?acc[Native]:acc[Execute]))/tscPerMs/double(frames);
        std::fprintf(f,"%s\"%s\":%.4f",i?",":"",names[i],v);
    }
    std::fprintf(f,"}},");
    for(auto&a:acc)a=0;draws=frames=0;windowQpc=now;windowTsc=tsc;
}
}
// Live switches for the CPU work of 2026-10-02: one bit per optimisation, all
// on by default. control.ini CpuOpt=<mask> flips them in game, so an A/B runs
// interleaved at one spot instead of across restarts. Never saved.
namespace WosCpuOpt {
static unsigned mask=0xffffffffu;
static inline bool On(unsigned bit){return (mask>>bit)&1u;}
}
