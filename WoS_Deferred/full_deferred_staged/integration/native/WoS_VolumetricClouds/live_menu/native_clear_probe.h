#pragma once
// F7-only, read-only native Clear inventory. No clear generations are inferred.
// Include after renderer_probe.h; the caller owns the original slot-43 hook.
#include <d3d9.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
namespace WosNativeClear {
using ClearFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,DWORD,const D3DRECT*,DWORD,D3DCOLOR,float,DWORD);
static ClearFn originalClear=nullptr;
constexpr unsigned MaxEvents=512,MaxRects=64;
constexpr size_t MaxBytes=4u*1024u*1024u;
static FILE* log=nullptr;
static std::string captureFolder;
static unsigned attempted=0,written=0,dropped=0,suppressed=0,observationFailures=0,internalErrors=0;
static size_t bytesWritten=0;
static bool truncated=false,incomplete=false,ioError=false,openAttempted=false;
static void String(std::ostream&o,const char*s){o<<'"';if(s)for(unsigned n=0;s[n]&&n<160;++n){unsigned char c=static_cast<unsigned char>(s[n]);if(c=='"'||c=='\\')o<<'\\'<<char(c);else if(c<32){char b[7];std::snprintf(b,sizeof(b),"\\u%04x",unsigned(c));o<<b;}else o<<char(c);}o<<'"';}
static void Float(std::ostream&o,float v){if(std::isfinite(v))o<<v;else o<<"null";}
static uint32_t FloatBits(float v){uint32_t b=0;std::memcpy(&b,&v,4);return b;}
static void Address(std::ostream&o,uintptr_t p){char b[32];std::snprintf(b,sizeof(b),"0x%08llx",static_cast<unsigned long long>(p));String(o,b);}
static bool Emit(const std::string&line,bool terminal=false){
 if(!log)return false;
 if(!terminal&&(written>=MaxEvents||line.size()>MaxBytes||bytesWritten>MaxBytes-line.size())){truncated=incomplete=true;++dropped;return false;}
 if(std::fwrite(line.data(),1,line.size(),log)!=line.size()){ioError=incomplete=true;return false;}bytesWritten+=line.size();return true;
}
static void Finish(const char*reason) noexcept {
 try {
 if(!log)return;
 if(!WosProbe::trace||WosProbe::failed||std::strcmp(reason,"PRESENT"))incomplete=true;
 std::ostringstream o;o.imbue(std::locale::classic());o<<"{\"type\":\"end\",\"version\":1,\"reason\":";String(o,reason);
 o<<",\"attempted\":"<<attempted<<",\"events\":"<<written<<",\"dropped\":"<<dropped<<",\"suppressed\":"<<suppressed<<",\"observation_failures\":"<<observationFailures<<",\"internal_errors\":"<<internalErrors<<",\"draws_at_end\":"<<WosProbe::draws<<",\"truncated\":"<<(truncated?"true":"false")<<",\"incomplete\":"<<(incomplete?"true":"false")<<",\"io_error\":"<<((ioError||std::ferror(log))?"true":"false")<<"}\n";
 Emit(o.str(),true);if(std::fclose(log))ioError=true;log=nullptr;
 }catch(...){++internalErrors;incomplete=true;if(log)std::fclose(log);log=nullptr;}
}
static void BeginIfActive() noexcept {
 try {
 if(!WosProbe::trace||WosProbe::suppress)return;
 if(captureFolder!=WosProbe::folder){Finish("RESTART");captureFolder=WosProbe::folder;attempted=written=dropped=suppressed=observationFailures=internalErrors=0;bytesWritten=0;truncated=incomplete=ioError=openAttempted=false;}
 if(openAttempted)return;openAttempted=true;
 log=std::fopen((captureFolder+"\\rsm_native_clears.jsonl").c_str(),"wb");
 if(!log){ioError=incomplete=true;std::fprintf(WosProbe::trace,"NATIVE_CLEAR_PROBE_ERROR\tcannot open rsm_native_clears.jsonl\n");return;}
 incomplete=WosProbe::draws!=0;
 std::ostringstream o;o.imbue(std::locale::classic());o<<"{\"type\":\"begin\",\"version\":1,\"read_only\":true,\"generation_tracking\":false,\"max_events\":"<<MaxEvents<<",\"max_rects\":"<<MaxRects<<",\"max_bytes\":"<<MaxBytes<<",\"draws_at_begin\":"<<WosProbe::draws<<",\"engine_probe_enabled\":"<<(WosEngineProbe::enabled?"true":"false")<<"}\n";Emit(o.str());
 }catch(...){++internalErrors;incomplete=true;}
}
struct Site {uintptr_t caller;const char*name;unsigned char bytes[32];};
static const Site sites[]={
 {0x009e9601,"native_scene_clear",{0x96,0x90,0x04,0x00,0x00,0xd9,0x1c,0x24,0x52,0xe8,0xc1,0xab,0xe9,0xff,0x83,0xc4,0x04,0x50,0x8b,0x83,0xac,0x00,0x00,0x00,0x57,0x6a,0x00,0x6a,0x00,0x55,0xff,0xd0}},
 {0x009e88d9,"native_other_clear",{0x00,0x00,0xff,0xd0,0xd9,0xe8,0xa1,0xbc,0x70,0x11,0x01,0x8b,0x08,0x8b,0x91,0xac,0x00,0x00,0x00,0x53,0x51,0xd9,0x1c,0x24,0x53,0x6a,0x07,0x53,0x53,0x50,0xff,0xd2}}
};
struct Surface {
 IDirect3DSurface9* value=nullptr;IUnknown* identity=nullptr;
 HRESULT query=E_PENDING,identityQuery=E_PENDING,descQuery=E_PENDING;
 unsigned id=0,canonicalId=0;D3DSURFACE_DESC desc={};
 void Inspect(){if(FAILED(query)||!value)return;id=WosProbe::Id(value);identityQuery=value->QueryInterface(IID_IUnknown,reinterpret_cast<void**>(&identity));if(SUCCEEDED(identityQuery)&&identity)canonicalId=WosProbe::Id(identity);descQuery=value->GetDesc(&desc);}
 bool Complete(bool allowAbsent)const{return allowAbsent&&query==D3DERR_NOTFOUND&&!value?true:SUCCEEDED(query)&&value&&SUCCEEDED(identityQuery)&&identity&&SUCCEEDED(descQuery);}
 ~Surface(){if(identity)identity->Release();if(value)value->Release();}
};
struct Event {
 unsigned sequence=0,drawsBefore=0;uintptr_t caller=0,imageBase=0;const char*candidate="none";
 bool engineEnabled=false,engineRead=false,globalsStable=false,callerCodeChecked=false,callerCodeRead=false,callerCodeMatch=false;
 WosEngineProbe::Globals globals{},globalsAfter{};
 bool arenaRead=false,arenaBounds=false,sceneInArena=false,vpRead=false,vpFinite=false;
 uint32_t arena[3]={};float sceneVP[16]={};
 Surface rt,ds;IUnknown* deviceIdentity=nullptr;HRESULT deviceIdentityQuery=E_PENDING;unsigned deviceId=0;
 D3DVIEWPORT9 viewport={};HRESULT viewportQuery=E_PENDING,scissorStateQuery=E_PENDING,scissorRectQuery=E_PENDING;DWORD scissorEnabled=0;RECT scissor={};
 bool rectsConsistent=false,rectReadAttempted=false,rectsCopied=false,rectsUnknown=true;std::array<D3DRECT,MaxRects>rects{};
 ~Event(){if(deviceIdentity)deviceIdentity->Release();}
};
static bool Observe(IDirect3DDevice9*d,DWORD count,const D3DRECT*rects,uintptr_t caller,Event&e){
 BeginIfActive();if(!log||ioError)return false;
 ++attempted;if(truncated||written>=MaxEvents){truncated=incomplete=true;++dropped;return false;}
 e.sequence=attempted-1;e.drawsBefore=WosProbe::draws;e.caller=caller;
 e.engineEnabled=WosEngineProbe::enabled;e.imageBase=WosEngineProbe::imageBase;
 const Site*site=nullptr;for(const auto&s:sites)if(s.caller==caller){site=&s;e.candidate=s.name;break;}
 // The existing gate covers the exact EXE and its own loaded signatures. Clear
 // callsite bytes are separately checked; a candidate address alone is no proof.
 e.engineRead=e.engineEnabled&&WosEngineProbe::imageBase==0x400000;
 if(e.engineRead){
  e.globals=WosEngineProbe::ReadGlobals();
  if(site){unsigned char bytes[32]={};e.callerCodeChecked=true;e.callerCodeRead=WosEngineProbe::Read(caller-sizeof(bytes),bytes,sizeof(bytes));e.callerCodeMatch=e.callerCodeRead&&!std::memcmp(bytes,site->bytes,sizeof(bytes));}
  size_t used=0;e.arenaRead=WosEngineProbe::Read(WosEngineProbe::imageBase+0xd15c64,e.arena,sizeof(e.arena));e.arenaBounds=e.arenaRead&&WosEngineProbe::ArenaBounds(e.arena,used);
  e.sceneInArena=e.arenaBounds&&(e.globals.valid&1)&&e.globals.current>=e.arena[0]&&uint64_t(e.globals.current)+0x4e0<=e.arena[2];
  if(e.sceneInArena){e.vpRead=WosEngineProbe::Read(uintptr_t(e.globals.current)+0x190,e.sceneVP,sizeof(e.sceneVP));e.vpFinite=e.vpRead;for(float v:e.sceneVP)if(!std::isfinite(v))e.vpFinite=false;}
  e.globalsAfter=WosEngineProbe::ReadGlobals();e.globalsStable=!std::memcmp(&e.globals,&e.globalsAfter,sizeof(e.globals));
 }
 e.deviceIdentityQuery=d->QueryInterface(IID_IUnknown,reinterpret_cast<void**>(&e.deviceIdentity));if(SUCCEEDED(e.deviceIdentityQuery)&&e.deviceIdentity)e.deviceId=WosProbe::Id(e.deviceIdentity);
 e.rt.query=d->GetRenderTarget(0,&e.rt.value);e.rt.Inspect();e.ds.query=d->GetDepthStencilSurface(&e.ds.value);e.ds.Inspect();
 e.viewportQuery=d->GetViewport(&e.viewport);e.scissorStateQuery=d->GetRenderState(D3DRS_SCISSORTESTENABLE,&e.scissorEnabled);
 // Read the rectangle even when disabled so the raw state is available. Its
 // failed query is immaterial to effective Clear area only when disabled.
 e.scissorRectQuery=d->GetScissorRect(&e.scissor);
 e.rectsConsistent=(count==0)==(rects==nullptr);
 if(!count&&!rects)e.rectsCopied=true;
 else if(count&&count<=MaxRects&&rects){e.rectReadAttempted=true;e.rectsCopied=WosEngineProbe::Read(reinterpret_cast<uintptr_t>(rects),e.rects.data(),size_t(count)*sizeof(D3DRECT));}
 e.rectsUnknown=!e.rectsConsistent||count>MaxRects||!e.rectsCopied;
 bool queries=SUCCEEDED(e.deviceIdentityQuery)&&e.deviceIdentity&&e.rt.Complete(false)&&e.ds.Complete(true)&&SUCCEEDED(e.viewportQuery)&&SUCCEEDED(e.scissorStateQuery)&&(!e.scissorEnabled||SUCCEEDED(e.scissorRectQuery));
 bool engineComplete=e.engineRead&&e.globals.valid==7&&e.globalsStable&&e.arenaBounds&&e.sceneInArena&&e.vpRead&&e.vpFinite&&(!site||e.callerCodeMatch);
 if(!queries||e.rectsUnknown||!engineComplete){++observationFailures;incomplete=true;}
 return true;
}
static void Globals(std::ostream&o,const WosEngineProbe::Globals&g){o<<"{\"current\":"<<g.current<<",\"root\":"<<g.root<<",\"near\":"<<g.nearScene<<",\"far\":"<<g.farScene<<",\"shadow_active\":"<<g.shadowActive<<",\"valid\":"<<g.valid<<'}';}
static void SurfaceJson(std::ostream&o,const Surface&s){
 o<<"{\"query_hr\":"<<uint32_t(s.query)<<",\"pointer\":";Address(o,reinterpret_cast<uintptr_t>(s.value));o<<",\"surface_id\":"<<s.id<<",\"identity_hr\":"<<uint32_t(s.identityQuery)<<",\"canonical_id\":"<<s.canonicalId<<",\"desc_hr\":"<<uint32_t(s.descQuery)<<",\"width\":"<<s.desc.Width<<",\"height\":"<<s.desc.Height<<",\"format\":"<<unsigned(s.desc.Format)<<",\"multisample_type\":"<<unsigned(s.desc.MultiSampleType)<<",\"multisample_quality\":"<<s.desc.MultiSampleQuality<<'}';
}
static void Record(const Event&e,DWORD count,const D3DRECT*rects,DWORD flags,D3DCOLOR color,float z,DWORD stencil,HRESULT originalResult){
 std::ostringstream o;o.imbue(std::locale::classic());o<<std::setprecision(9);
 o<<"{\"type\":\"clear\",\"sequence\":"<<e.sequence<<",\"draws_before\":"<<e.drawsBefore<<",\"caller\":";Address(o,e.caller);o<<",\"candidate\":";String(o,e.candidate);
 o<<",\"engine_probe_enabled\":"<<(e.engineEnabled?"true":"false")<<",\"engine_read_attempted\":"<<(e.engineRead?"true":"false")<<",\"image_base\":";Address(o,e.imageBase);
 o<<",\"caller_code_checked\":"<<(e.callerCodeChecked?"true":"false")<<",\"caller_code_read\":"<<(e.callerCodeRead?"true":"false")<<",\"caller_code_matches\":"<<(e.callerCodeMatch?"true":"false")<<",\"globals\":";Globals(o,e.globals);o<<",\"globals_after\":";Globals(o,e.globalsAfter);o<<",\"globals_stable\":"<<(e.globalsStable?"true":"false");
 o<<",\"scene_snapshot\":{\"arena_read\":"<<(e.arenaRead?"true":"false")<<",\"arena_bounds\":"<<(e.arenaBounds?"true":"false")<<",\"arena\":["<<e.arena[0]<<','<<e.arena[1]<<','<<e.arena[2]<<"],\"scene_in_arena\":"<<(e.sceneInArena?"true":"false")<<",\"vp_read\":"<<(e.vpRead?"true":"false")<<",\"vp_finite\":"<<(e.vpFinite?"true":"false")<<",\"vp\":[";
 for(unsigned i=0;i<16;++i){if(i)o<<',';Float(o,e.sceneVP[i]);}o<<"],\"vp_bits\":[";for(unsigned i=0;i<16;++i)o<<(i?",":"")<<FloatBits(e.sceneVP[i]);o<<"]}";
 o<<",\"device_identity_hr\":"<<uint32_t(e.deviceIdentityQuery)<<",\"device_canonical_id\":"<<e.deviceId<<",\"rt0\":";SurfaceJson(o,e.rt);o<<",\"ds\":";SurfaceJson(o,e.ds);
 o<<",\"viewport\":{\"hr\":"<<uint32_t(e.viewportQuery)<<",\"value\":["<<e.viewport.X<<','<<e.viewport.Y<<','<<e.viewport.Width<<','<<e.viewport.Height<<',';Float(o,e.viewport.MinZ);o<<',';Float(o,e.viewport.MaxZ);o<<"]},\"scissor\":{\"state_hr\":"<<uint32_t(e.scissorStateQuery)<<",\"enabled\":"<<e.scissorEnabled<<",\"rect_hr\":"<<uint32_t(e.scissorRectQuery)<<",\"rect\":["<<e.scissor.left<<','<<e.scissor.top<<','<<e.scissor.right<<','<<e.scissor.bottom<<"]}";
 o<<",\"rects\":{\"count\":"<<count<<",\"pointer\":";Address(o,reinterpret_cast<uintptr_t>(rects));o<<",\"consistent\":"<<(e.rectsConsistent?"true":"false")<<",\"read_attempted\":"<<(e.rectReadAttempted?"true":"false")<<",\"copied\":"<<(e.rectsCopied?"true":"false")<<",\"unknown\":"<<(e.rectsUnknown?"true":"false")<<",\"values\":[";
 if(e.rectsCopied&&!e.rectsUnknown)for(unsigned i=0;i<count;++i){const auto&r=e.rects[i];o<<(i?",":"")<<'['<<r.x1<<','<<r.y1<<','<<r.x2<<','<<r.y2<<']';}o<<"]}";
 o<<",\"flags\":"<<flags<<",\"color\":"<<color<<",\"z\":";Float(o,z);o<<",\"z_bits\":"<<FloatBits(z)<<",\"stencil\":"<<stencil<<",\"original_hr\":"<<uint32_t(originalResult)<<"}\n";
 if(Emit(o.str()))++written;
}
static HRESULT Invoke(IDirect3DDevice9*d,DWORD count,const D3DRECT*rects,DWORD flags,D3DCOLOR color,float z,DWORD stencil,uintptr_t caller){
 // Inactive/suppressed path does no D3D queries, process reads or I/O.
 if(!WosProbe::trace||WosProbe::suppress){if(WosProbe::trace&&WosProbe::suppress&&log){++suppressed;incomplete=true;}return originalClear(d,count,rects,flags,color,z,stencil);}
 Event e;bool observed=false;
 try {if(d)observed=Observe(d,count,rects,caller,e);else {++internalErrors;incomplete=true;}}catch(...){++internalErrors;incomplete=true;}
 // Keep this outside both probe try blocks. A probe failure must never retry,
 // skip or replace the native call, including an original failing HRESULT.
 const HRESULT hr=originalClear(d,count,rects,flags,color,z,stencil);
 try {if(observed)Record(e,count,rects,flags,color,z,stencil,hr);}catch(...){++internalErrors;incomplete=true;}
 return hr;
}
}
