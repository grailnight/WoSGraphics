#pragma once
// Redundant shader-constant filter. The native fx binder (0x9EA5B0/0x9EA730)
// re-sends every VS float and PS bool register on every node, one bool per
// call; ~97% of the values equal what the device already holds. Each setter
// keeps a copy of what was last forwarded and passes on only the registers
// that differ. Off unless [Cache] Constants=1 or control.ini ConstCache=1.
//
// The copy is only trusted while every write goes through these hooks: a state
// block Apply, Reset or a device switch drops it, and while a state block is
// recording every call is forwarded untouched (recorded setters must reach the
// block) and the copy is dropped at EndStateBlock.
#include <cstring>
namespace WosConstCache {
static bool enabled=false;
static bool recording=false;
static IDirect3DDevice9*owner=nullptr;
static constexpr unsigned kVsF=256,kPsF=224,kB=16;
static float vsF[kVsF][4],psF[kPsF][4];static BOOL vsB[kB],psB[kB];
static bool vsFValid[kVsF],psFValid[kPsF],vsBValid[kB],psBValid[kB];
// Per-second totals for the status file: calls, registers asked, registers sent.
static unsigned long long calls=0,callsSkipped=0,regsAsked=0,regsSent=0;
static unsigned long long lastCalls=0,lastCallsSkipped=0,lastRegsAsked=0,lastRegsSent=0;
static ULONGLONG windowStart=0;
// Light-read reuse (WosDeferred::ReadLights): the serial of the last forwarded
// write that touched each PS float register, and an epoch that moves whenever
// the copy above is dropped. psHooked is set once vtable slot 109 is ours.
static unsigned long long psStamp[kPsF]={},stampSerial=1;
static unsigned epoch=1;static bool psHooked=false;
static void Stamp(unsigned long long*stamp,UINT start,UINT count,unsigned n){
    if(!stamp)return;const unsigned long long s=++stampSerial;
    for(unsigned r=start;r<n&&r-start<count;++r)stamp[r]=s;
}
// True when no forwarded write touched [index,index+count) after serial.
static bool PsUntouched(unsigned index,unsigned count,unsigned long long serial){
    for(unsigned r=index;r<kPsF&&r-index<count;++r)if(psStamp[r]>serial)return false;
    return true;
}
using SetFFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,UINT,const float*,UINT);
using SetBFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,UINT,const BOOL*,UINT);
static SetFFn originalVsF=nullptr,originalPsF=nullptr;
static SetBFn originalVsB=nullptr,originalPsB=nullptr;

static void Invalidate(){
    std::memset(vsFValid,0,sizeof(vsFValid));std::memset(psFValid,0,sizeof(psFValid));
    std::memset(vsBValid,0,sizeof(vsBValid));std::memset(psBValid,0,sizeof(psBValid));
    ++epoch;
}
static void OnApply(){Invalidate();}
static void OnReset(){Invalidate();recording=false;owner=nullptr;}
static void OnBeginStateBlock(){recording=true;}
static void OnEndStateBlock(){recording=false;Invalidate();}
static void LoadSettings(const char*path){enabled=GetPrivateProfileIntA("Cache","Constants",0,path)!=0;}

static void WriteStatus(){
    const ULONGLONG now=GetTickCount64();
    if(!windowStart){windowStart=now;return;}
    if(now-windowStart<1000)return;
    const double seconds=double(now-windowStart)/1000.0;windowStart=now;
    lastCalls=calls;lastCallsSkipped=callsSkipped;lastRegsAsked=regsAsked;lastRegsSent=regsSent;
    calls=callsSkipped=regsAsked=regsSent=0;
    FILE*f=std::fopen((WosProbe::Root()+"\\full_deferred\\const_cache_status.json").c_str(),"wb");if(!f)return;
    std::fprintf(f,"{\"enabled\":%s,\"calls_per_s\":%.0f,\"calls_skipped_per_s\":%.0f,\"regs_asked_per_s\":%.0f,\"regs_sent_per_s\":%.0f}\n",
        enabled?"true":"false",lastCalls/seconds,lastCallsSkipped/seconds,lastRegsAsked/seconds,lastRegsSent/seconds);
    std::fclose(f);
}
// Narrows [start,start+count) to the first..last register whose value differs
// from the copy, refreshing the copy. Returns false when nothing differs.
template<class T,unsigned N,unsigned W>
static bool Narrow(T(&copy)[N][W],bool(&valid)[N],UINT&start,const T*&data,UINT&count){
    unsigned first=count,last=0;
    for(unsigned i=0;i<count;++i){
        const T*v=data+size_t(i)*W;unsigned r=start+i;
        if(valid[r]&&!std::memcmp(copy[r],v,sizeof(T)*W))continue;
        if(first==count)first=i;last=i;
    }
    if(first==count)return false;
    for(unsigned i=first;i<=last;++i){std::memcpy(copy[start+i],data+size_t(i)*W,sizeof(T)*W);valid[start+i]=true;}
    data+=size_t(first)*W;start+=first;count=last-first+1;return true;
}
template<class T,unsigned N,unsigned W,class Fn>
static HRESULT Filter(Fn original,IDirect3DDevice9*d,UINT start,const T*data,UINT count,T(&copy)[N][W],bool(&valid)[N],unsigned long long*stamp=nullptr){
    ++calls;regsAsked+=count;
    if((calls&1023)==0)WriteStatus();
    if(d!=owner){owner=d;Invalidate();}
    const bool inRange=data&&count&&start<N&&count<=N-start;
    if(!enabled||recording||!inRange){
        HRESULT hr=original(d,start,data,count);regsSent+=count;Stamp(stamp,start,count,N);
        for(unsigned r=start;r<N&&r-start<count;++r)valid[r]=false;
        return hr;
    }
    if(!Narrow(copy,valid,start,data,count)){++callsSkipped;return D3D_OK;}
    HRESULT hr=original(d,start,data,count);regsSent+=count;Stamp(stamp,start,count,N);
    if(FAILED(hr))for(unsigned i=0;i<count;++i)valid[start+i]=false;
    return hr;
}
// Bool registers are one BOOL wide; view them as [N][1].
static BOOL(&AsRows(BOOL(&a)[kB]))[kB][1]{return *reinterpret_cast<BOOL(*)[kB][1]>(&a);}
static HRESULT STDMETHODCALLTYPE SetVsF(IDirect3DDevice9*d,UINT s,const float*v,UINT n){return Filter(originalVsF,d,s,v,n,vsF,vsFValid);}
static HRESULT STDMETHODCALLTYPE SetPsF(IDirect3DDevice9*d,UINT s,const float*v,UINT n){return Filter(originalPsF,d,s,v,n,psF,psFValid,psStamp);}
static HRESULT STDMETHODCALLTYPE SetVsB(IDirect3DDevice9*d,UINT s,const BOOL*v,UINT n){return Filter(originalVsB,d,s,v,n,AsRows(vsB),vsBValid);}
static HRESULT STDMETHODCALLTYPE SetPsB(IDirect3DDevice9*d,UINT s,const BOOL*v,UINT n){return Filter(originalPsB,d,s,v,n,AsRows(psB),psBValid);}
}
