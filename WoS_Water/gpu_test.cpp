#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <vector>
#include <cstring>
#include "native_water.h"
static HRESULT STDMETHODCALLTYPE Create(IDirect3DDevice9*d,const DWORD*b,IDirect3DPixelShader9**o){return d->CreatePixelShader(b,o);}
static HRESULT STDMETHODCALLTYPE Fail(IDirect3DDevice9*,const DWORD*,IDirect3DPixelShader9**){return E_FAIL;}
static bool Equal(IDirect3DPixelShader9*p,const unsigned char*data,UINT length){
    UINT n=0;if(FAILED(p->GetFunction(nullptr,&n))||n!=length)return false;
    std::vector<unsigned char>b(n);return SUCCEEDED(p->GetFunction(b.data(),&n))&&!memcmp(b.data(),data,n);
}
int main(){
    HWND window=CreateWindowA("STATIC","Water GPU validation",WS_OVERLAPPED,0,0,64,64,nullptr,nullptr,GetModuleHandle(nullptr),nullptr);
    IDirect3D9*api=Direct3DCreate9(D3D_SDK_VERSION);if(!api)return 1;
    D3DPRESENT_PARAMETERS pp={};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=window;
    pp.BackBufferWidth=64;pp.BackBufferHeight=64;pp.BackBufferFormat=D3DFMT_X8R8G8B8;
    IDirect3DDevice9*d=nullptr;
    HRESULT hr=api->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&d);
    if(FAILED(hr)){printf("device creation failed %08lx\n",hr);return 2;}
    WosWater::initialized=true;WosWater::enabled=true;WosWater::logPath="WoS_Water/gpu_test_runtime.log";
    int errors=0;
    for(const auto&a:WosWaterAssets::assets){
        std::vector<DWORD> input(a.originalSize/4);memcpy(input.data(),a.original,a.originalSize);
        IDirect3DPixelShader9*p=nullptr;hr=d->CreatePixelShader(input.data(),&p);
        if(FAILED(hr)||!p){++errors;printf("original rejected %s %08lx\n",a.name,hr);continue;}
        WosWater::Replace(d,&p,Create,GetModuleHandle(nullptr));
        bool match=Equal(p,a.replacement,a.replacementSize);errors+=!match;
        hr=d->SetPixelShader(p);errors+=FAILED(hr);d->SetPixelShader(nullptr);p->Release();
        printf("%s replacement=%d bind_hr=%08lx\n",a.name,match,hr);
        d->CreatePixelShader(input.data(),&p);auto*before=p;
        WosWater::Replace(d,&p,Fail,GetModuleHandle(nullptr));
        bool fallback=p==before&&Equal(p,a.original,a.originalSize);errors+=!fallback;p->Release();
        WosWater::enabled=false;d->CreatePixelShader(input.data(),&p);before=p;
        WosWater::Replace(d,&p,Create,GetModuleHandle(nullptr));
        bool disabled=p==before&&Equal(p,a.original,a.originalSize);errors+=!disabled;p->Release();WosWater::enabled=true;
        // Change an internal float constant, retaining length and validity.
        size_t k=1;while(k<input.size()&&(input[k]&0xffff)==0xfffe)k+=1+((input[k]>>16)&0x7fff);
        if(k+2>=input.size()||(input[k]&0xffff)!=81){++errors;continue;}
        input[k+2]^=1;
        d->CreatePixelShader(input.data(),&p);if(!p){++errors;continue;}before=p;
        WosWater::Replace(d,&p,Create,GetModuleHandle(nullptr));
        bool unknown=p==before&&Equal(p,reinterpret_cast<unsigned char*>(input.data()),a.originalSize);errors+=!unknown;p->Release();
        printf("fallback=%d disabled=%d same_size_unknown_unchanged=%d\n",fallback,disabled,unknown);
    }
    d->Release();api->Release();DestroyWindow(window);
    printf("errors=%d\n",errors);return errors?3:0;
}
