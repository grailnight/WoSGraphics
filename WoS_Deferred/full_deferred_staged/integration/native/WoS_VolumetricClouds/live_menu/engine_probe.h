#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/engine_probe_signatures.h"
#pragma comment(lib,"bcrypt.lib")
namespace WosEngineProbe {
static FILE *log=nullptr,*raw=nullptr;
static uintptr_t imageBase=0;
static bool enabled=false,ioError=false,havePrevious=false;
static unsigned rows=0,snapshots=0;
static bool arenaAttempted=false;
static std::string captureFolder;
struct Globals { uint32_t current=0,root=0,nearScene=0,farScene=0,shadowActive=0,valid=0; };
static Globals previous;
// No unguarded dereferences of inferred engine pointers. Failed reads clear output.
static bool Read(uintptr_t address,void *out,size_t bytes) {
    SIZE_T done=0;
    if(address<0x10000 || !ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(address),out,bytes,&done) || done!=bytes) {
        std::memset(out,0,bytes); return false;
    }
    return true;
}
static bool FileSha(const wchar_t *path,std::string &sha) {
    sha.clear(); FILE *f=_wfopen(path,L"rb"); if(!f)return false;
    BCRYPT_ALG_HANDLE alg=nullptr; BCRYPT_HASH_HANDLE hash=nullptr;
    DWORD size=0,got=0; bool ok=false; unsigned char digest[32]={};
    std::vector<unsigned char> object,chunk(65536);
    if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0 &&
       BCryptGetProperty(alg,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&got,0)>=0 && size>0 && size<1048576) {
        object.resize(size);
        if(BCryptCreateHash(alg,&hash,object.data(),size,nullptr,0,0)>=0) {
            ok=true; size_t n;
            while((n=std::fread(chunk.data(),1,chunk.size(),f))>0) {
                if(BCryptHashData(hash,chunk.data(),static_cast<ULONG>(n),0)<0){ok=false;break;}
            }
            ok=ok&&!std::ferror(f);
            if(ok)ok=BCryptFinishHash(hash,digest,sizeof(digest),0)>=0;
        }
    }
    if(hash)BCryptDestroyHash(hash); if(alg)BCryptCloseAlgorithmProvider(alg,0);
    if(std::fclose(f)!=0)ok=false;
    if(ok){char text[65]={};for(unsigned i=0;i<32;++i)std::snprintf(text+i*2,3,"%02x",digest[i]);sha=text;}
    return ok;
}
// SHA256 of the game EXE with the two header fields a 4GB (Large Address Aware)
// patch touches cleared: the LAA bit in Characteristics and the PE CheckSum.
// The stock 1.1 EXE and any LAA-patched copy of it give the same value; the
// code itself is still checked byte for byte by the signatures below.
static bool ExeIdentitySha(const wchar_t *path,std::string &sha) {
    sha.clear(); FILE *f=_wfopen(path,L"rb"); if(!f)return false;
    std::vector<unsigned char> data; unsigned char chunk[65536]; size_t n;
    while((n=std::fread(chunk,1,sizeof(chunk),f))>0 && data.size()<64u*1024*1024)data.insert(data.end(),chunk,chunk+n);
    const bool readOk=!std::ferror(f); std::fclose(f);
    if(!readOk || data.size()<0x400)return false;
    uint32_t pe=0; std::memcpy(&pe,&data[60],4);
    if(pe+92>data.size() || std::memcmp(&data[pe],"PE\0\0",4))return false;
    data[pe+22]&=static_cast<unsigned char>(~0x20); std::memset(&data[pe+88],0,4);
    BCRYPT_ALG_HANDLE alg=nullptr; BCRYPT_HASH_HANDLE hash=nullptr; DWORD size=0,got=0; bool ok=false; unsigned char digest[32]={};
    std::vector<unsigned char> object;
    if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0 &&
       BCryptGetProperty(alg,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&got,0)>=0 && size>0 && size<1048576) {
        object.resize(size);
        ok=BCryptCreateHash(alg,&hash,object.data(),size,nullptr,0,0)>=0 &&
           BCryptHashData(hash,data.data(),static_cast<ULONG>(data.size()),0)>=0 &&
           BCryptFinishHash(hash,digest,sizeof(digest),0)>=0;
    }
    if(hash)BCryptDestroyHash(hash); if(alg)BCryptCloseAlgorithmProvider(alg,0);
    if(ok){char text[65]={};for(unsigned i=0;i<32;++i)std::snprintf(text+i*2,3,"%02x",digest[i]);sha=text;}
    return ok;
}
static bool Verify(std::string &reason) {
    imageBase=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    // Signature bytes include absolute operands. A relocated image needs a separate validated map.
    if(sizeof(void*)!=4 || imageBase!=0x400000){reason="unsupported image base or architecture";return false;}
    wchar_t path[32768]={}; DWORD n=GetModuleFileNameW(nullptr,path,32768);
    std::string sha;
    if(!n || n>=32768 || !ExeIdentitySha(path,sha)){reason="cannot hash running executable file";return false;}
    if(sha!=expectedSha){reason="executable SHA256 mismatch";return false;}
    for(const auto &s:signatures) {
        unsigned char found[32]={};
        if(!Read(imageBase+s.rva,found,sizeof(found)) || std::memcmp(found,s.bytes,32)) {
            char b[100];std::snprintf(b,sizeof(b),"code signature mismatch at RVA %08x",s.rva);reason=b;return false;
        }
    }
    reason="EXE identity (LAA-neutral) and all 12 loaded code signatures match";return true;
}
static void Finish(const char *reason) {
    if(!log)return;
    bool bad=ioError || (raw && std::ferror(raw));
    if(raw){if(std::fclose(raw))bad=true;raw=nullptr;}
    std::fprintf(log,"END\t%s\trows=%u\tsnapshots=%u\tio_error=%d\n",reason,rows,snapshots,bad||std::ferror(log));
    std::fclose(log);log=nullptr;enabled=false;havePrevious=false;
}
static void Begin(const std::string &folder) {
    Finish("RESTART");rows=snapshots=0;ioError=false;havePrevious=false;
    arenaAttempted=false;captureFolder=folder;
    log=std::fopen((folder+"\\engine.tsv").c_str(),"wb");if(!log)return;
    std::string reason;enabled=Verify(reason);
    std::fprintf(log,"WOS_ENGINE_PROBE\t1\nVERIFY\t%d\t%s\tbase=%08llx\n",enabled,reason.c_str(),static_cast<unsigned long long>(imageBase));
    std::fprintf(log,"NOTE\tRead-only inferred scene fields. Pointer readability does not establish object type or lifetime.\n");
    if(!enabled)return;
    raw=std::fopen((folder+"\\engine_scenes.bin").c_str(),"wb");
    if(!raw){enabled=false;std::fprintf(log,"DISABLED\tcannot open scene dump\n");}
}
static Globals ReadGlobals() {
    Globals g;uint32_t pair[2]={};unsigned char active=0;
    if(Read(imageBase+0xd15c54,pair,sizeof(pair))){g.current=pair[0];g.root=pair[1];g.valid|=1;}
    if(Read(imageBase+0xc36e98,pair,sizeof(pair))){g.nearScene=pair[0];g.farScene=pair[1];g.valid|=2;}
    if(Read(imageBase+0xc73dfa,&active,1)){g.shadowActive=active;g.valid|=4;}
    return g;
}

// Capture the render-list arena before the engine consumes/relinks its queues.
// This is a CPU-side snapshot, not a replayable copy of external GPU resources.
static bool ArenaBounds(const uint32_t (&a)[3],size_t &used) {
    used=0;
    if(a[0]<0x10000 || !a[1] || a[1]>64u*1024u*1024u || a[2]<a[0])return false;
    uint64_t end=static_cast<uint64_t>(a[0])+a[1];
    if(end>0x100000000ull || a[2]>end)return false;
    used=static_cast<size_t>(a[2]-a[0]);return used>0;
}
static void BeforeBeginScene(uintptr_t caller) {
    if(!enabled || !log || ioError || arenaAttempted)return;
    // Avoid dumping a partially consumed list if a proxy begins another scene later.
    if(rows!=0 || caller!=imageBase+0x5dcf3a)return;
    Globals g=ReadGlobals();
    if(g.valid!=7 || !g.root || g.current!=g.root)return;
    arenaAttempted=true;
    uint32_t a[3]={},after[3]={};size_t used=0;
    if(!Read(imageBase+0xd15c64,a,sizeof(a)) || !ArenaBounds(a,used)) {
        std::fprintf(log,"ARENA_REJECTED\tinvalid allocator bounds\n");return;
    }
    std::vector<unsigned char> bytes;
    try {bytes.resize(used);} catch(...) {std::fprintf(log,"ARENA_REJECTED\tallocation failed\n");return;}
    if(!Read(a[0],bytes.data(),used)) {std::fprintf(log,"ARENA_REJECTED\tunreadable arena\n");return;}
    Globals gAfter=ReadGlobals();
    if(!Read(imageBase+0xd15c64,after,sizeof(after)) || std::memcmp(a,after,sizeof(a)) || std::memcmp(&g,&gAfter,sizeof(g))) {
        std::fprintf(log,"ARENA_REJECTED\tmetadata changed during snapshot\n");return;
    }
    FILE *f=std::fopen((captureFolder+"\\engine_arena.bin").c_str(),"wb");
    if(!f){std::fprintf(log,"ARENA_REJECTED\tcannot open file\n");ioError=true;return;}
    bool ok=std::fwrite(bytes.data(),1,used,f)==used;if(std::fclose(f))ok=false;
    if(!ok){ioError=true;std::fprintf(log,"ARENA_REJECTED\twrite failed\n");return;}
    std::fprintf(log,"ARENA\tbefore_begin_scene\tbase=%08x\tcapacity=%u\tused=%u\troot=%08x\tnear=%08x\tfar=%08x\tcaller=%08llx\n",a[0],a[1],static_cast<unsigned>(used),g.root,g.nearScene,g.farScene,static_cast<unsigned long long>(caller));
}

static void Snapshot(unsigned draw,const char *role,uint32_t ptr) {
    if(!ptr || snapshots>=256)return;
    unsigned char bytes[0x4e0]={};
    if(!Read(ptr,bytes,sizeof(bytes))){std::fprintf(log,"UNREADABLE\t%u\t%s\t%08x\n",draw,role,ptr);return;}
    auto offset=_ftelli64(raw);
    if(offset<0 || std::fwrite(bytes,1,sizeof(bytes),raw)!=sizeof(bytes)){ioError=true;return;}
    std::fprintf(log,"SCENE\t%u\t%s\t%08x\toffset=%lld\tbytes=%u\n",draw,role,ptr,offset,static_cast<unsigned>(sizeof(bytes)));
    ++snapshots;
}
static void Draw(unsigned draw,uintptr_t caller) {
    if(!enabled||!log||ioError)return;
    Globals g=ReadGlobals();
    // Absolute caller is preserved: overlays/proxies may be outside the EXE.
    std::fprintf(log,"DRAW\t%u\tcaller=%08llx\tcurrent=%08x\troot=%08x\tnear=%08x\tfar=%08x\tshadow=%u\tvalid=%u\n",draw,static_cast<unsigned long long>(caller),g.current,g.root,g.nearScene,g.farScene,g.shadowActive,g.valid);
    ++rows;
    if(!havePrevious || std::memcmp(&previous,&g,sizeof(g))) {
        Snapshot(draw,"current",g.current);
        if(g.nearScene!=g.current)Snapshot(draw,"near",g.nearScene);
        if(g.farScene!=g.current && g.farScene!=g.nearScene)Snapshot(draw,"far",g.farScene);
        previous=g;havePrevious=true;
    }
    if(std::ferror(log)){ioError=true;enabled=false;}
}
}
