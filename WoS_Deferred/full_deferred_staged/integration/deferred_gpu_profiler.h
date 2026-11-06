#pragma once
// Optional GPU timestamp profiler for the primary deferred renderer.
//
// It is OFF by default and never changes device state, bound resources or
// pixels: it only issues D3D9 timestamp queries around work that already
// happens. Results are read from an older ring slot with GetData(...,0), so no
// forced flush and no same-frame wait serialize the pipeline. A device without
// timestamp support reports unsupported instead of failing a frame, and every
// disjoint window is discarded rather than reported as a duration.
//
// The frame span is measured between consecutive frame boundaries and therefore
// includes Present, vsync and any idle time. It is not a sum of GPU busy work
// and must not be compared against a CPU submission timer.
//
// Primary geometry is thousands of draws per frame, far beyond any per-draw
// stamp budget, so it is measured as one interval per batch: from the batch
// opening to the start of its resolve. Native draws interleaved in that span
// are inside the number; it is a batch span, not a per-draw geometry cost.
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <vector>
namespace WosDeferredGpu {
enum Section{SectionPrimaryBatch=0,SectionCopyIn,SectionResolve,SectionCopyOut,SectionGiMerge,SectionVolumetric,SectionCount};
static const char* const kSectionKeys[SectionCount]={"primary_batch","hdr_copy_in","resolve_quad","hdr_copy_out","gi_merge","volumetric"};
static const unsigned kRing=4;        // frames in flight before a slot is reused
static const unsigned kStamps=128;    // timestamp queries per slot, frame pair included
static const unsigned kWindow=256;    // completed frames kept for statistics

static bool enabled=false;            // user request from settings or control API
static bool supported=true;           // cleared once the device refuses a query
static const char* status="GPU profiler OFF";

struct Interval {unsigned char section;unsigned short begin,end;};
struct Slot {
    IDirect3DQuery9* disjoint=nullptr;
    IDirect3DQuery9* frequency=nullptr;
    IDirect3DQuery9* stamp[kStamps]={};
    Interval interval[kStamps/2]={};
    unsigned used=0,intervals=0,frameEnd=0;
    bool open=false,awaiting=false,truncated=false;
};
struct Sample {float section[SectionCount];unsigned char calls[SectionCount];float span;bool truncated;};

static IDirect3DDevice9* owner=nullptr;
static Slot ring[kRing];
static unsigned cursor=0;             // slot currently open, or the next to open
static std::vector<Sample> window;
static unsigned windowAt=0;
static unsigned completed=0,discardedDisjoint=0,skippedRingFull=0,truncatedFrames=0;

template<class T> static void ReleaseQuery(T*&q){if(q)q->Release();q=nullptr;}
static void ReleaseSlot(Slot&s){
    ReleaseQuery(s.disjoint);ReleaseQuery(s.frequency);
    for(unsigned i=0;i<kStamps;++i)ReleaseQuery(s.stamp[i]);
    s.used=s.intervals=s.frameEnd=0;s.open=s.awaiting=s.truncated=false;
}
// Called from the renderer Reset path. No query survives a device reset.
static void Release(){
    for(auto&s:ring)ReleaseSlot(s);
    owner=nullptr;cursor=0;window.clear();windowAt=0;
    completed=discardedDisjoint=skippedRingFull=truncatedFrames=0;
    if(supported)status=enabled?"GPU profiler armed":"GPU profiler OFF";
}
static void Unsupported(const char*why){supported=false;status=why;for(auto&s:ring)ReleaseSlot(s);cursor=0;}
static bool Create(IDirect3DDevice9*d,D3DQUERYTYPE type,IDirect3DQuery9*&out){
    if(out)return true;
    HRESULT hr=d->CreateQuery(type,&out);
    if(FAILED(hr)||!out){out=nullptr;return false;}
    return true;
}
static bool Prepare(IDirect3DDevice9*d,Slot&s){
    if(!Create(d,D3DQUERYTYPE_TIMESTAMPDISJOINT,s.disjoint)||!Create(d,D3DQUERYTYPE_TIMESTAMPFREQ,s.frequency)){
        Unsupported("GPU profiler unavailable: device has no timestamp queries");return false;
    }
    return true;
}
// A stamp is created on first use so a frame that touches few sections never
// allocates the whole array. The last slot belongs to the frame-end stamp: a
// frame that runs out of section stamps is still reported, marked truncated.
static int IssueStamp(IDirect3DDevice9*d,Slot&s,bool closing=false){
    if(s.used>=(closing?kStamps:kStamps-1)){s.truncated=true;return -1;}
    unsigned index=s.used;
    if(!Create(d,D3DQUERYTYPE_TIMESTAMP,s.stamp[index])){Unsupported("GPU profiler unavailable: timestamp query creation failed");return -1;}
    if(FAILED(s.stamp[index]->Issue(D3DISSUE_END))){s.truncated=true;return -1;}
    ++s.used;return int(index);
}
static bool Active(){return enabled&&supported&&ring[cursor].open;}
// An open interval. Begin issues its first stamp; End issues the second and
// records the pair. Outside an open frame, with the profiler off, or past the
// stamp budget both are no-ops. A mark from another ring slot is discarded so a
// pair can never straddle two frames.
struct Mark {int index=-1;unsigned slot=0;};
static Mark Begin(IDirect3DDevice9*d,Section){
    Mark mark;if(!d||!Active())return mark;
    mark.index=IssueStamp(d,ring[cursor]);mark.slot=cursor;return mark;
}
static void End(IDirect3DDevice9*d,Section which,Mark&mark){
    int begin=mark.index;unsigned slot=mark.slot;mark=Mark{};
    if(begin<0||!d||!Active()||slot!=cursor)return;
    Slot&s=ring[cursor];
    int end=IssueStamp(d,s);
    if(end<0||s.intervals>=kStamps/2){s.truncated=true;return;}
    s.interval[s.intervals++]={static_cast<unsigned char>(which),static_cast<unsigned short>(begin),static_cast<unsigned short>(end)};
}
// RAII form of Begin/End for regions that open and close in one function.
struct Scope {
    IDirect3DDevice9* device=nullptr;Section section;Mark mark;
    Scope(IDirect3DDevice9*d,Section which):device(d),section(which),mark(Begin(d,which)){}
    ~Scope(){End(device,section,mark);}
    Scope(const Scope&)=delete;Scope&operator=(const Scope&)=delete;
};
static void Push(const Sample&sample){
    if(window.size()<kWindow)window.push_back(sample);
    else{window[windowAt]=sample;windowAt=(windowAt+1)%kWindow;}
    ++completed;
}
// Reads one finished slot. Returns false while the GPU has not finished it; the
// caller then leaves the slot awaiting instead of stalling on the result.
static bool Collect(Slot&s){
    if(!s.awaiting)return true;
    BOOL disjoint=FALSE;UINT64 frequency=0;
    HRESULT hr=s.disjoint->GetData(&disjoint,sizeof(disjoint),0);
    if(hr==S_FALSE)return false;
    if(FAILED(hr)){Unsupported("GPU profiler stopped: timestamp query lost");return true;}
    hr=s.frequency->GetData(&frequency,sizeof(frequency),0);
    if(hr==S_FALSE)return false;
    bool usable=SUCCEEDED(hr)&&!disjoint&&frequency>0&&s.used>=2;
    Sample sample={};
    if(usable){
        UINT64 value[kStamps]={};
        for(unsigned i=0;i<s.used&&usable;++i){
            HRESULT got=s.stamp[i]->GetData(&value[i],sizeof(UINT64),0);
            if(got==S_FALSE)return false;
            usable=SUCCEEDED(got);
        }
        // Stamp 0 opens the frame, frameEnd closes it; sections sit between.
        const double scale=1000.0/double(frequency);
        if(usable&&s.frameEnd&&s.frameEnd<s.used&&value[s.frameEnd]>=value[0]){
            sample.span=float(double(value[s.frameEnd]-value[0])*scale);
            for(unsigned i=0;i<s.intervals;++i){
                const Interval&in=s.interval[i];
                if(in.section>=SectionCount||in.end>=s.used||in.begin>=s.used||value[in.end]<value[in.begin])continue;
                sample.section[in.section]+=float(double(value[in.end]-value[in.begin])*scale);
                if(sample.calls[in.section]<255)++sample.calls[in.section];
            }
            sample.truncated=s.truncated;
            if(s.truncated)++truncatedFrames;
            Push(sample);
        }else usable=false;
    }
    if(!usable)++discardedDisjoint;
    s.awaiting=false;s.used=s.intervals=s.frameEnd=0;s.truncated=false;
    return true;
}
static void CollectAll(){for(auto&s:ring)if(!Collect(s))break;}
// Frame boundary. Closes the open slot, polls finished ones, then opens the next
// slot. Both stamps of a frame belong to the same disjoint window.
static void FrameBoundary(IDirect3DDevice9*d){
    if(owner&&owner!=d)Release();
    if(!supported||!d){if(!enabled)status=supported?"GPU profiler OFF":status;return;}
    owner=d;
    Slot&open=ring[cursor];
    if(open.open){
        int last=IssueStamp(d,open,true);         // frame end stamp closes the pair
        open.frameEnd=last>0?unsigned(last):0;
        if(FAILED(open.disjoint->Issue(D3DISSUE_END)))open.truncated=true;
        open.open=false;open.awaiting=true;
        cursor=(cursor+1)%kRing;
    }
    CollectAll();
    if(!enabled){status=supported?"GPU profiler OFF":status;return;}
    Slot&next=ring[cursor];
    if(next.awaiting&&!Collect(next)){++skippedRingFull;status="GPU profiler waiting for older frame results";return;}
    if(!supported||!Prepare(d,next))return;
    if(FAILED(next.disjoint->Issue(D3DISSUE_BEGIN))||FAILED(next.frequency->Issue(D3DISSUE_END)))return;
    next.open=true;next.used=next.intervals=next.frameEnd=0;next.truncated=false;
    if(IssueStamp(d,next)!=0){                    // frame begin stamp, index 0
        next.open=false;if(FAILED(next.disjoint->Issue(D3DISSUE_END)))return;
        next.awaiting=true;return;
    }
    status=completed?"GPU profiler measuring":"GPU profiler armed";
}
struct Stat {double mean=0,median=0,p95=0;};
static Stat Percentiles(std::vector<float>&values){
    Stat out;if(values.empty())return out;
    double total=0;for(float v:values)total+=v;
    out.mean=total/double(values.size());
    size_t mid=values.size()/2;
    std::nth_element(values.begin(),values.begin()+mid,values.end());out.median=values[mid];
    size_t high=size_t(double(values.size()-1)*0.95);
    std::nth_element(values.begin(),values.begin()+high,values.end());out.p95=values[high];
    return out;
}
static Stat SectionStat(unsigned section){
    std::vector<float> values;values.reserve(window.size());
    for(const auto&s:window)values.push_back(s.section[section]);
    return Percentiles(values);
}
static Stat SpanStat(){
    std::vector<float> values;values.reserve(window.size());
    for(const auto&s:window)values.push_back(s.span);
    return Percentiles(values);
}
static double SectionCalls(unsigned section){
    if(window.empty())return 0;
    double total=0;for(const auto&s:window)total+=s.calls[section];
    return total/double(window.size());
}
static void ClearWindow(){window.clear();windowAt=0;completed=discardedDisjoint=skippedRingFull=truncatedFrames=0;}
// Appends the profiler object to an already open JSON object. The caller owns
// the surrounding braces and the preceding comma.
static void WriteJson(FILE*f){
    std::fprintf(f,"\"gpu_profiler\":{\"enabled\":%s,\"supported\":%s,\"status\":\"%s\",\"window_frames\":%u,\"completed_frames\":%u,\"discarded_frames\":%u,\"skipped_frames\":%u,\"truncated_frames\":%u,",
        enabled?"true":"false",supported?"true":"false",status,unsigned(window.size()),completed,discardedDisjoint,skippedRingFull,truncatedFrames);
    Stat span=SpanStat();
    std::fprintf(f,"\"frame_span_ms\":{\"mean\":%.4f,\"median\":%.4f,\"p95\":%.4f},\"sections\":{",span.mean,span.median,span.p95);
    for(unsigned i=0;i<SectionCount;++i){
        Stat s=SectionStat(i);
        std::fprintf(f,"%s\"%s\":{\"mean\":%.4f,\"median\":%.4f,\"p95\":%.4f,\"calls\":%.2f}",i?",":"",kSectionKeys[i],s.mean,s.median,s.p95,SectionCalls(i));
    }
    std::fprintf(f,"},\"scope\":\"gpu_timestamp_deltas_frame_span_includes_present_and_idle\"}");
}
}
