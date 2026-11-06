#pragma once
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "native_assets.h"

namespace WosWater {
using CreateFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,const DWORD*,IDirect3DPixelShader9**);
static bool initialized=false,enabled=false;
static std::string logPath;
// Optional host callback for each replacement created, e.g. water reflections.
static void(*onReplaced)(const char* name,IDirect3DPixelShader9* shader)=nullptr;
static void Initialize(HMODULE module) {
    if(initialized)return;
    initialized=true;
    char filename[MAX_PATH]={};
    if(!GetModuleFileNameA(module,filename,MAX_PATH))return;
    std::string base(filename);auto slash=base.find_last_of("\\/");
    if(slash==std::string::npos)return;
    base.resize(slash+1);base+="WoS_Water\\";
    enabled=GetPrivateProfileIntA("Water","Enabled",1,(base+"settings.ini").c_str())!=0;
    logPath=base+"runtime.log";
}
static void Log(const char* name,const char* result,HRESULT hr) {
    FILE* f=nullptr;
    if(fopen_s(&f,logPath.c_str(),"a")==0&&f){
        fprintf(f,"pid=%lu tick=%llu shader=%s result=%s hr=%08lx\n",GetCurrentProcessId(),GetTickCount64(),name,result,(unsigned long)hr);
        fclose(f);
    }
}
// Called only after the original shader was successfully created. Never reads
// past the caller's bytecode: GetFunction supplies the validated program length.
static void Replace(IDirect3DDevice9* device,IDirect3DPixelShader9** shader,CreateFn create,HMODULE module) {
    Initialize(module);
    if(!enabled||!shader||!*shader)return;
    UINT size=0;
    if(FAILED((*shader)->GetFunction(nullptr,&size)))return;
    for(const auto& asset:WosWaterAssets::assets){
        if(size!=asset.originalSize)continue;
        std::vector<unsigned char> actual(size);
        if(FAILED((*shader)->GetFunction(actual.data(),&size))||size!=asset.originalSize)return;
        if(memcmp(actual.data(),asset.original,size)!=0)continue;
        // DWORD-aligned storage for D3D9 on every supported compiler.
        std::vector<DWORD> aligned(asset.replacementSize/4);
        memcpy(aligned.data(),asset.replacement,asset.replacementSize);
        IDirect3DPixelShader9* replacement=nullptr;
        HRESULT hr=create(device,aligned.data(),&replacement);
        if(SUCCEEDED(hr)&&replacement){
            (*shader)->Release();*shader=replacement;
            Log(asset.name,"replaced",hr);
            if(onReplaced)onReplaced(asset.name,replacement);
        }else{
            if(replacement)replacement->Release();
            Log(asset.name,"native_fallback",hr);
        }
        return;
    }
}
}
