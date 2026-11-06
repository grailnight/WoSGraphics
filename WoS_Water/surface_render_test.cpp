#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cmath>
#include <vector>
#include <cstring>
#include "native_assets.h"
static void Check(HRESULT h){if(FAILED(h)){printf("HRESULT %08lx\n",h);ExitProcess(2);}}
static const int N=128;
static std::vector<float> Render(IDirect3DDevice9*d,IDirect3DSurface9*rt,IDirect3DSurface9*read,const unsigned char*bytes,unsigned size,float time){
    std::vector<DWORD>code(size/4);memcpy(code.data(),bytes,size);IDirect3DPixelShader9*ps=nullptr;
    Check(d->CreatePixelShader(code.data(),&ps));Check(d->SetPixelShader(ps));
    float clock[4]={time,0,0,0};Check(d->SetPixelShaderConstantF(8,clock,1));
    float vertices[]={-1,-1,0, 1,-1,0, -1,1,0, 1,1,0};
    Check(d->Clear(0,nullptr,D3DCLEAR_TARGET,0,1,0));Check(d->BeginScene());
    Check(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,vertices,3*sizeof(float)));Check(d->EndScene());
    Check(d->GetRenderTargetData(rt,read));D3DLOCKED_RECT lock{};Check(read->LockRect(&lock,nullptr,D3DLOCK_READONLY));
    std::vector<float>pixels(N*N*4);
    for(int y=0;y<N;++y)memcpy(pixels.data()+y*N*4,(char*)lock.pBits+y*lock.Pitch,N*16);
    read->UnlockRect();d->SetPixelShader(nullptr);ps->Release();return pixels;
}
int main(){
    HWND w=CreateWindowA("STATIC","Water offscreen render test",WS_OVERLAPPED,0,0,N,N,nullptr,nullptr,GetModuleHandle(nullptr),nullptr);
    auto*api=Direct3DCreate9(D3D_SDK_VERSION);if(!api)return 1;
    D3DPRESENT_PARAMETERS pp={};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=w;pp.BackBufferWidth=N;pp.BackBufferHeight=N;pp.BackBufferFormat=D3DFMT_X8R8G8B8;
    IDirect3DDevice9*d=nullptr;Check(api->CreateDevice(0,D3DDEVTYPE_HAL,w,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&d));
    IDirect3DSurface9*rt=nullptr,*read=nullptr;
    Check(d->CreateRenderTarget(N,N,D3DFMT_A32B32G32R32F,D3DMULTISAMPLE_NONE,0,FALSE,&rt,nullptr));
    Check(d->CreateOffscreenPlainSurface(N,N,D3DFMT_A32B32G32R32F,D3DPOOL_SYSTEMMEM,&read,nullptr));
    Check(d->SetRenderTarget(0,rt));Check(d->SetDepthStencilSurface(nullptr));
    const char*vsSource="struct O{float4 p:POSITION;float3 w:TEXCOORD2;float3 n:TEXCOORD3;float2 uv:TEXCOORD4;}; O main(float3 p:POSITION){O o;o.p=float4(p,1);o.w=float3(p.x*30,0,p.y*30);o.n=float3(0,1,0);o.uv=p.xy*.5+.5;return o;}";
    ID3DBlob*blob=nullptr,*error=nullptr;Check(D3DCompile(vsSource,strlen(vsSource),nullptr,nullptr,nullptr,"main","vs_3_0",0,0,&blob,&error));
    IDirect3DVertexShader9*vs=nullptr;Check(d->CreateVertexShader((DWORD*)blob->GetBufferPointer(),&vs));blob->Release();if(error)error->Release();
    Check(d->SetVertexShader(vs));Check(d->SetFVF(D3DFVF_XYZ));
    d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);d->SetRenderState(D3DRS_ZENABLE,FALSE);d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);
    float c[16][4]={};c[4][0]=.05f;c[4][1]=.1f;c[4][2]=.15f;c[5][2]=1;
    c[6][0]=0;c[6][1]=18;c[6][2]=-45;
    for(int j=0;j<4;++j)c[7][j]=1;
    c[9][0]=-.025f;c[9][1]=-.025f;c[10][0]=8.392f;
    c[11][0]=.025f;c[11][1]=-.025f;c[12][0]=.8f;
    c[13][1]=-.025f;c[14][0]=1.2f;c[15][0]=.3f;
    Check(d->SetPixelShaderConstantF(0,&c[0][0],16));
    IDirect3DTexture9*textures[2]={};
    for(int s=0;s<2;++s){
        Check(d->CreateTexture(N,N,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&textures[s],nullptr));
        D3DLOCKED_RECT lock{};Check(textures[s]->LockRect(0,&lock,nullptr,0));
        for(int y=0;y<N;++y)for(int x=0;x<N;++x){
            int r,g,b,a;
            if(s==0){r=70+x;g=90+y;b=110+(x+y)/4;a=64;}
            else{r=int(128+25*sin(x*.25)*cos(y*.17));g=int(128+25*cos(x*.19)*sin(y*.3));b=252;a=255;}
            ((DWORD*)((char*)lock.pBits+y*lock.Pitch))[x]=(a<<24)|(r<<16)|(g<<8)|b;
        }
        textures[s]->UnlockRect(0);Check(d->SetTexture(s,textures[s]));
        d->SetSamplerState(s,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);d->SetSamplerState(s,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
        d->SetSamplerState(s,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP);d->SetSamplerState(s,D3DSAMP_ADDRESSV,D3DTADDRESS_WRAP);
    }
    const auto&a=WosWaterAssets::assets[0];
    auto old=Render(d,rt,read,a.original,a.originalSize,3);
    auto now=Render(d,rt,read,a.replacement,a.replacementSize,3);
    auto later=Render(d,rt,read,a.replacement,a.replacementSize,5);
    double difference=0,motion=0,alpha=0;int nonfinite=0;
    for(size_t k=0;k<now.size();++k){
        nonfinite+=!std::isfinite(now[k])||!std::isfinite(later[k])||!std::isfinite(old[k]);
        if(k%4==3)alpha=fmax(alpha,fabs(now[k]-old[k]));
        else{difference+=fabs(now[k]-old[k]);motion+=fabs(now[k]-later[k]);}
    }
    difference/=N*N*3;motion/=N*N*3;
    printf("native_vs_new_mean_rgb_difference=%.8f\nanimation_mean_rgb_difference=%.8f\nmax_alpha_difference=%.8f\nnonfinite=%d\n",difference,motion,alpha,nonfinite);
    bool ok=difference>.003&&motion>.0001&&alpha<1e-6&&nonfinite==0;
    for(int s=0;s<2;++s){d->SetTexture(s,nullptr);textures[s]->Release();}d->SetVertexShader(nullptr);vs->Release();rt->Release();read->Release();d->Release();api->Release();DestroyWindow(w);
    return ok?0:3;
}
