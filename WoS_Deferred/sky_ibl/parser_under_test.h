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
    bool hasContactEnabled=false,contactEnabled=false,hasContactLength=false,hasContactThickness=false,hasContactSteps=false,hasContactStrength=false,hasContactBias=false;
    float contactLength=0,contactThickness=0,contactSteps=0,contactStrength=0,contactBias=0;
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
        const char*names[]={"RequestId","Enabled","View","Intensity","Specular","Capture","Trace","ReloadMaterials","IndirectEnabled","GpuProfiler","GpuProfilerReset","EligibilityCache","ContactEnabled","ContactLength","ContactThickness","ContactSteps","ContactStrength","ContactBias","ShadowGlobals","ShadowWork","SkyIBL"};
        unsigned index=0;while(index<21&&key!=names[index])++index;
        if(index==21){error="unknown control key";return false;}
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
        }
        if(!good){error="invalid value for "+key;return false;}
    }
    if(!(seen&1)){error="RequestId required";return false;}
    if(out.reload&&out.hasIndirect){error="GI switch and material reload require separate requests";return false;}return true;
}
}
