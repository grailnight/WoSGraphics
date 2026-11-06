#pragma once
// Include after deferred_runtime.h and renderer_probe.h. Renderer-owned local
// diagnostics only: fixed paths, bounded input, no shell/network/UI automation.
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
namespace WosDeferredControl {
struct Request {
    bool hasSky=false,sky=false;
    uint64_t id=0;
    bool hasEnabled=false,enabled=false,hasView=false,hasIntensity=false,hasSpecular=false;
    int view=0;float intensity=0,specular=0;
    bool capture=false,trace=false,reload=false,hasIndirect=false,indirect=false;
    bool hasGpuProfiler=false,gpuProfiler=false,gpuProfilerReset=false;
    bool hasEligibilityCache=false,eligibilityCache=false;
    bool hasShadowGlobals=false,shadowGlobals=false;
    bool hasShadowWork=false,shadowWork=false;
    bool hasLocalStatic=false,localStatic=false;
    bool hasContactEnabled=false,contactEnabled=false,hasContactLength=false,hasContactThickness=false,hasContactSteps=false,hasContactStrength=false,hasContactBias=false;
    float contactLength=0,contactThickness=0,contactSteps=0,contactStrength=0,contactBias=0;bool hasHookProfile=false,hookProfile=false,hasStagger=false,hasCasterCull=false,casterCull=false,hasCpuOpt=false;int stagger=0;uint32_t cpuOpt=0;bool hasConstCache=false,constCache=false,hasGiHoldMrt=false,giHoldMrt=false,hasLightReuse=false,lightReuse=false,hasVerdictReuse=false,verdictReuse=false,gbufferDump=false;
};
static std::string Trim(const std::string&s){
    size_t a=s.find_first_not_of(" \t\r");if(a==std::string::npos)return {};
    return s.substr(a,s.find_last_not_of(" \t\r")-a+1);
}
static bool DecimalId(const std::string&s,uint64_t&out){
    out=0;if(s.empty())return false;
    for(char c:s){if(c<'0'||c>'9'||out>(UINT64_MAX-uint64_t(c-'0'))/10)return false;out=out*10+uint64_t(c-'0');}
    return out!=0;
}
static bool Number(const std::string&s,float low,float high,float&out){
    if(s.empty())return false;char*end=nullptr;errno=0;
    double value=std::strtod(s.c_str(),&end);
    if(errno||end!=s.c_str()+s.size()||!std::isfinite(value)||value<low||value>high)return false;
    out=static_cast<float>(value);return true;
}
static bool Parse(const std::string&source,Request&out,std::string&error){
    out={};error.clear();
    if(source.size()>4096||source.find('\0')!=std::string::npos){error="invalid input size or NUL";return false;}
    std::string text=source;if(text.compare(0,3,"\xef\xbb\xbf")==0)text.erase(0,3);
    unsigned seen=0;bool section=false;size_t at=0;
    while(at<text.size()){
        size_t end=text.find('\n',at);if(end==std::string::npos)end=text.size();
        std::string line=Trim(text.substr(at,end-at));at=end+1;
        if(line.empty()||line[0]==';'||line[0]=='#')continue;
        if(line[0]=='['){if(line!="[Control]"||section){error="expected one [Control] section";return false;}section=true;continue;}
        size_t equal=line.find('=');if(!section||equal==std::string::npos){error="invalid control line";return false;}
        std::string key=Trim(line.substr(0,equal)),value=Trim(line.substr(equal+1));
        const char*names[]={"RequestId","Enabled","View","Intensity","Specular","Capture","Trace","ReloadMaterials","IndirectEnabled","GpuProfiler","GpuProfilerReset","EligibilityCache","ContactEnabled","ContactLength","ContactThickness","ContactSteps","ContactStrength","ContactBias","ShadowGlobals","ShadowWork","SkyIBL","LocalStatic","HookProfile","Stagger","CasterCull","CpuOpt","ConstCache","GiHoldMrt","LightReuse","VerdictReuse","GbufferDump"};
        unsigned index=0;while(index<31&&key!=names[index])++index;
        if(index==31){error="unknown control key";return false;}
        unsigned bit=1u<<index;if(seen&bit){error="duplicate control key";return false;}seen|=bit;
        bool good=true;
        switch(index){
        case 0:good=DecimalId(value,out.id);break;
        case 1:good=value=="0"||value=="1";out.hasEnabled=true;out.enabled=value=="1";break;
        case 2:good=value.size()==1&&value[0]>='0'&&value[0]<='4';out.hasView=true;out.view=good?value[0]-'0':0;break;
        case 3:out.hasIntensity=true;good=Number(value,0,3,out.intensity);break;
        case 4:out.hasSpecular=true;good=Number(value,0,2,out.specular);break;
        case 5:good=value=="0"||value=="1";out.capture=value=="1";break;
        case 6:good=value=="0"||value=="1";out.trace=value=="1";break;
        case 7:good=value=="0"||value=="1";out.reload=value=="1";break;
        case 8:good=value=="0"||value=="1";out.hasIndirect=true;out.indirect=value=="1";break;
        case 9:good=value=="0"||value=="1";out.hasGpuProfiler=true;out.gpuProfiler=value=="1";break;
        case 10:good=value=="0"||value=="1";out.gpuProfilerReset=value=="1";break;
        case 11:good=value=="0"||value=="1";out.hasEligibilityCache=true;out.eligibilityCache=value=="1";break;
        case 12:good=value=="0"||value=="1";out.hasContactEnabled=true;out.contactEnabled=value=="1";break;
        case 13:out.hasContactLength=true;good=Number(value,.05f,10,out.contactLength);break;
        case 14:out.hasContactThickness=true;good=Number(value,.01f,10,out.contactThickness);break;
        case 15:out.hasContactSteps=true;good=Number(value,2,32,out.contactSteps)&&out.contactSteps==float(int(out.contactSteps));break;
        case 16:out.hasContactStrength=true;good=Number(value,0,1,out.contactStrength);break;
        case 17:out.hasContactBias=true;good=Number(value,0,1,out.contactBias);break;
        case 18:good=value=="0"||value=="1";out.hasShadowGlobals=true;out.shadowGlobals=value=="1";break;
        case 20:good=value=="0"||value=="1";out.hasSky=true;out.sky=value=="1";break;
        case 19:good=value=="0"||value=="1";out.hasShadowWork=true;out.shadowWork=value=="1";break;
        case 21:good=value=="0"||value=="1";out.hasLocalStatic=true;out.localStatic=value=="1";break;case 22:good=value=="0"||value=="1";out.hasHookProfile=true;out.hookProfile=value=="1";break;case 23:good=value.size()==1&&value[0]>='0'&&value[0]<='2';out.hasStagger=true;out.stagger=good?value[0]-'0':0;break;case 24:good=value=="0"||value=="1";out.hasCasterCull=true;out.casterCull=value=="1";break;case 25:{uint64_t v=0;good=value=="0"||(DecimalId(value,v)&&v<=0xffffffffull);out.hasCpuOpt=true;out.cpuOpt=uint32_t(v);}break;case 26:good=value=="0"||value=="1";out.hasConstCache=true;out.constCache=value=="1";break;case 27:good=value=="0"||value=="1";out.hasGiHoldMrt=true;out.giHoldMrt=value=="1";break;case 28:good=value=="0"||value=="1";out.hasLightReuse=true;out.lightReuse=value=="1";break;case 29:good=value=="0"||value=="1";out.hasVerdictReuse=true;out.verdictReuse=value=="1";break;case 30:good=value=="0"||value=="1";out.gbufferDump=value=="1";break;
        }
        if(!good){error="invalid value for "+key;return false;}
    }
    if(!(seen&1)){error="RequestId required";return false;}
    if(out.reload&&out.hasIndirect){error="GI switch and material reload require separate requests";return false;}return true;
}
static std::string Directory(){return WosProbe::Root()+"\\full_deferred";}
static std::string JsonString(const std::string&s){
    std::string out="\"";const char*hex="0123456789abcdef";
    for(unsigned char c:s){if(c=='"'||c=='\\'){out+='\\';out+=char(c);}else if(c<32){out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}else out+=char(c);}
    return out+'"';
}
static bool WriteBytes(const std::string&path,const void*data,size_t size){
    HANDLE file=CreateFileA(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    DWORD written=0;bool ok=size<=MAXDWORD&&WriteFile(file,data,static_cast<DWORD>(size),&written,nullptr)&&written==size;
    if(!CloseHandle(file))ok=false;return ok;
}
static bool AtomicWrite(const std::string&path,const void*data,size_t size){
    const std::string temp=path+".tmp";
    if(!WriteBytes(temp,data,size))return false;
    return MoveFileExA(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
}
struct Acknowledgement {
    uint64_t id=0;bool applied=false,capturePending=false,traceStarted=false,reloadRequested=false,reloaded=false;
    std::string phase="idle",screenshot,traceFolder,error;
};
using ReloadMaterialsFn=bool(*)(IDirect3DDevice9*,std::string&);
static ReloadMaterialsFn reloadMaterials=nullptr;
using SetIndirectEnabledFn=bool(*)(bool,std::string&);
static SetIndirectEnabledFn setIndirectEnabled=nullptr;
static Acknowledgement ack;
static uint64_t lastRequest=0;
static ULONGLONG lastPoll=0;
static bool polled=false;
static std::string lastInput;
static void WriteStatus(){
    CreateDirectoryA(Directory().c_str(),nullptr);
    char fields[1024]={};std::snprintf(fields,sizeof(fields),"{\"revision\":4,\"request_id\":%llu,\"applied\":%s,\"capture_pending\":%s,\"trace_started\":%s,\"reload_requested\":%s,\"reloaded\":%s,\"enabled\":%s,\"indirect_enabled\":%s,\"view\":%d,\"intensity\":%.6g,\"specular\":%.6g,\"gpu_profiler\":%s,\"eligibility_cache\":%s,\"contact_enabled\":%s,\"contact_length\":%.6g,",
        static_cast<unsigned long long>(ack.id),ack.applied?"true":"false",ack.capturePending?"true":"false",ack.traceStarted?"true":"false",ack.reloadRequested?"true":"false",ack.reloaded?"true":"false",WosDeferred::enabled?"true":"false",WosIndirect::enabled?"true":"false",WosDeferred::debugView,WosDeferred::intensity,WosDeferred::specularIntensity,WosDeferredGpu::enabled?"true":"false",WosDeferredShadow::requested?"true":"false",WosDeferred::contactEnabled?"true":"false",WosDeferred::contactLength);
    std::string json=std::string(fields)+"\"phase\":"+JsonString(ack.phase)+",\"screenshot\":"+JsonString(ack.screenshot)+",\"trace_folder\":"+JsonString(ack.traceFolder)+",\"error\":"+JsonString(ack.error)+"}\n";
    AtomicWrite(Directory()+"\\control_status.json",json.data(),json.size());
}
static unsigned PixelBytes(D3DFORMAT format){
    switch(format){case D3DFMT_A8R8G8B8:case D3DFMT_X8R8G8B8:case D3DFMT_A8B8G8R8:case D3DFMT_X8B8G8R8:return 4;
    case D3DFMT_R5G6B5:case D3DFMT_X1R5G5B5:case D3DFMT_A1R5G5B5:return 2;case D3DFMT_R8G8B8:return 3;default:return 0;}
}
static bool EncodeBmp(const D3DSURFACE_DESC&desc,const D3DLOCKED_RECT&locked,std::vector<unsigned char>&out,std::string&error){
    unsigned bpp=PixelBytes(desc.Format);uint64_t stride=(uint64_t(desc.Width)*3+3)&~uint64_t(3),size=stride*desc.Height;
    if(!bpp){error="unsupported backbuffer format";return false;}
    if(!desc.Width||!desc.Height||desc.Width>16384||desc.Height>16384||size>128*1024*1024||!locked.pBits||locked.Pitch<int(uint64_t(desc.Width)*bpp)){error="invalid backbuffer dimensions or pitch";return false;}
    BITMAPFILEHEADER file={};BITMAPINFOHEADER info={};
    file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info);file.bfSize=file.bfOffBits+static_cast<DWORD>(size);
    info.biSize=sizeof(info);info.biWidth=static_cast<LONG>(desc.Width);info.biHeight=-static_cast<LONG>(desc.Height);info.biPlanes=1;info.biBitCount=24;info.biCompression=BI_RGB;info.biSizeImage=static_cast<DWORD>(size);
    out.assign(file.bfSize,0);std::memcpy(out.data(),&file,sizeof(file));std::memcpy(out.data()+sizeof(file),&info,sizeof(info));
    for(unsigned y=0;y<desc.Height;++y){
        const auto*src=static_cast<const unsigned char*>(locked.pBits)+size_t(y)*locked.Pitch;
        auto*dst=out.data()+file.bfOffBits+size_t(y)*size_t(stride);
        for(unsigned x=0;x<desc.Width;++x,src+=bpp,dst+=3){
            if(bpp==2){unsigned v=unsigned(src[0])|(unsigned(src[1])<<8),gBits=desc.Format==D3DFMT_R5G6B5?6:5;
                unsigned r=(v>>(5+gBits))&31,g=(v>>5)&((1u<<gBits)-1),b=v&31;
                dst[0]=static_cast<unsigned char>((b*255+15)/31);dst[1]=static_cast<unsigned char>((g*255+((1u<<gBits)-1)/2)/((1u<<gBits)-1));dst[2]=static_cast<unsigned char>((r*255+15)/31);
            }else if(desc.Format==D3DFMT_A8B8G8R8||desc.Format==D3DFMT_X8B8G8R8){dst[0]=src[2];dst[1]=src[1];dst[2]=src[0];}
            else{dst[0]=src[0];dst[1]=src[1];dst[2]=src[2];}
        }
    }
    return true;
}
struct SurfaceRef {IDirect3DSurface9*p=nullptr;~SurfaceRef(){if(p)p->Release();}};
static bool Capture(IDirect3DDevice9*d,std::string&path,std::string&error){
    path.clear();error.clear();SurfaceRef back,cpu;D3DSURFACE_DESC desc={};
    if(!d||FAILED(d->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&back.p))||FAILED(back.p->GetDesc(&desc))){error="GetBackBuffer failed";return false;}
    if(desc.MultiSampleType!=D3DMULTISAMPLE_NONE){error="multisampled backbuffer unsupported";return false;}
    if(!PixelBytes(desc.Format)){error="unsupported backbuffer format";return false;}
    if(!desc.Width||!desc.Height||desc.Width>16384||desc.Height>16384||uint64_t(desc.Width)*desc.Height*4>128*1024*1024){error="backbuffer exceeds capture limit";return false;}
    if(FAILED(d->CreateOffscreenPlainSurface(desc.Width,desc.Height,desc.Format,D3DPOOL_SYSTEMMEM,&cpu.p,nullptr))||FAILED(d->GetRenderTargetData(back.p,cpu.p))){error="backbuffer readback failed";return false;}
    D3DLOCKED_RECT locked={};if(FAILED(cpu.p->LockRect(&locked,nullptr,D3DLOCK_READONLY))){error="backbuffer lock failed";return false;}
    std::vector<unsigned char>bmp;bool ok=EncodeBmp(desc,locked,bmp,error);HRESULT unlock=cpu.p->UnlockRect();
    if(FAILED(unlock)){error="backbuffer unlock failed";ok=false;}if(!ok)return false;
    const std::string folder=Directory()+"\\diagnostics";CreateDirectoryA(Directory().c_str(),nullptr);CreateDirectoryA(folder.c_str(),nullptr);
    char name[100]={};std::snprintf(name,sizeof(name),"\\capture_%lu_%llu.bmp",GetCurrentProcessId(),static_cast<unsigned long long>(ack.id));
    std::string target=folder+name;if(!AtomicWrite(target,bmp.data(),bmp.size())){error="screenshot write failed";return false;}path=target;return true;
}
static void BeforePresent(IDirect3DDevice9*d){
    // One frame boundary per Present, before the capture readback stall.
    WosDeferredGpu::FrameBoundary(d);
    if(!ack.capturePending)return;
    ack.capturePending=false;
    std::string error;if(!Capture(d,ack.screenshot,error)){if(!ack.error.empty())ack.error+="; ";ack.error+=error;}
    ack.phase=ack.error.empty()?"complete":"error";WriteStatus();
}
static bool ReadControl(std::string&text){
    HANDLE file=CreateFileA((Directory()+"\\control.ini").c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    LARGE_INTEGER size={};bool ok=GetFileSizeEx(file,&size)&&size.QuadPart>=0;
    if(ok&&size.QuadPart>4096){text.assign(4097,'\0');CloseHandle(file);return true;}
    if(ok){text.resize(static_cast<size_t>(size.QuadPart));DWORD got=0;ok=ReadFile(file,text.data(),static_cast<DWORD>(text.size()),&got,nullptr)&&got==text.size();}
    CloseHandle(file);return ok;
}
static void AfterPresent(IDirect3DDevice9*d,HRESULT hr){
    if(SUCCEEDED(hr))WosDeferred::WriteStatus(); // Existing one-second throttle also updates disabled-mode FPS.
    ULONGLONG now=GetTickCount64();if(polled&&now-lastPoll<300)return;polled=true;lastPoll=now;
    if(ack.capturePending)return;
    std::string input;if(!ReadControl(input)||input==lastInput)return;lastInput=input;
    Request request;std::string error;bool valid=Parse(input,request,error);
    ack={};ack.id=request.id;ack.reloadRequested=request.reload;
    if(!valid){ack.phase="error";ack.error=error;WriteStatus();return;}
    if(request.id<=lastRequest){ack.phase="ignored";ack.error="RequestId must increase";WriteStatus();return;}
    lastRequest=request.id;
    if(FAILED(hr)||!d){ack.phase="error";ack.error="Present failed; command not applied";WriteStatus();return;}
    if(request.hasIndirect){
        if(WosDeferred::pending)ack.error="GI switch rejected: pending batch";
        else if(WosDeferred::busy)ack.error="GI switch rejected: runtime busy";
        else if(WosDeferred::unsafeDeviceState)ack.error="GI switch rejected: unsafe device state";
        else if(!setIndirectEnabled)ack.error="GI switch is unavailable in this renderer";
        else if(!setIndirectEnabled(request.indirect,ack.error)&&ack.error.empty())ack.error="GI switch failed";
        if(!ack.error.empty()){ack.phase="error";WriteStatus();return;}
    }
    if(request.reload){
        if(WosDeferred::pending)ack.error="material reload rejected: pending batch";
        else if(WosDeferred::busy)ack.error="material reload rejected: runtime busy";
        else if(WosDeferred::unsafeDeviceState)ack.error="material reload rejected: unsafe device state";
        else if(!reloadMaterials)ack.error="material reload is unavailable in this renderer";
        else{
            ack.reloaded=reloadMaterials(d,ack.error);
            if(!ack.reloaded&&ack.error.empty())ack.error="material reload failed; old pack retained";
        }
        if(!ack.error.empty()||!ack.reloaded){ack.phase="error";WriteStatus();return;}
    }
    // Profiling only issues timestamp queries; it changes no pixels and is
    // applied after the reload and GI guards above.
    if(request.hasGpuProfiler)WosDeferredGpu::enabled=request.gpuProfiler;
    if(request.hasShadowGlobals)WosShadowFastGlobals::enabled=request.shadowGlobals;
    if(request.hasSky)WosSkyIBL::enabled=request.sky;
    if(request.hasShadowWork)WosShadowWork::enabled=request.shadowWork;
    if(request.hasLocalStatic)WosLocalShadow::staticEnabled=request.localStatic;if(request.hasHookProfile)WosHookProfile::enabled=request.hookProfile;if(request.hasStagger)WosCascadeShadow::stagger=request.stagger;if(request.hasCasterCull)WosShadowWork::cullEnabled=request.casterCull;if(request.hasCpuOpt)WosCpuOpt::mask=request.cpuOpt;if(request.hasConstCache)WosConstCache::enabled=request.constCache;if(request.hasGiHoldMrt)WosDeferred::giHoldMrt=request.giHoldMrt;if(request.hasLightReuse){WosDeferred::lightReuse=request.lightReuse;WosDeferred::lightCache.valid=false;}if(request.hasVerdictReuse){WosDeferred::verdictReuse=request.verdictReuse;WosDeferred::verdictCache.Clear();WosDeferredShadow::verifyReadOnly=request.verdictReuse;}
    if(request.gbufferDump){WosDeferred::gbufferDumpId=request.id;WosDeferred::gbufferDumpBatch=0;}
    if(request.gpuProfilerReset)WosDeferredGpu::ClearWindow();
    if(request.hasEligibilityCache)WosDeferredShadow::SetEnabled(request.eligibilityCache);
    if(request.hasEnabled)WosDeferred::enabled=request.enabled;
    if(request.hasView)WosDeferred::debugView=request.view;
    if(request.hasIntensity)WosDeferred::intensity=request.intensity;
    if(request.hasSpecular)WosDeferred::specularIntensity=request.specular;
    if(request.hasContactEnabled)WosDeferred::contactEnabled=request.contactEnabled;
    if(request.hasContactLength)WosDeferred::contactLength=request.contactLength;
    if(request.hasContactThickness)WosDeferred::contactThickness=request.contactThickness;
    if(request.hasContactSteps)WosDeferred::contactSteps=int(request.contactSteps);
    if(request.hasContactStrength)WosDeferred::contactStrength=request.contactStrength;
    if(request.hasContactBias)WosDeferred::contactBias=request.contactBias;
    if(request.hasSky||request.hasShadowWork||request.hasLocalStatic||request.hasShadowGlobals||request.hasEnabled||request.hasView||request.hasIntensity||request.hasSpecular||request.hasIndirect||request.hasGpuProfiler||request.hasEligibilityCache||request.hasContactEnabled||request.hasContactLength||request.hasContactThickness||request.hasContactSteps||request.hasContactStrength||request.hasContactBias)WosDeferred::SaveSettings();
    ack.applied=true;ack.capturePending=request.capture;
    if(request.trace){
        if(WosProbe::trace)ack.error="probe already capturing";
        else{WosProbe::Begin(d);ack.traceStarted=WosProbe::trace!=nullptr;if(ack.traceStarted)ack.traceFolder=WosProbe::folder;else ack.error="probe could not start";}
    }
    ack.phase=ack.capturePending?"capture_pending":ack.error.empty()?"complete":"error";WriteStatus();
}
static void Reset(){
    if(ack.capturePending){ack.capturePending=false;ack.phase="error";ack.error="device reset before screenshot";WriteStatus();}
    // Keep request deduplication across Reset; no D3D object survives a hook.
    polled=false;
}
}
