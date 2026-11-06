#define NOMINMAX
#include <windows.h>
#include <vector>
#include <cstdio>
#include <stdexcept>
#include <fstream>
#include "smaa_runtime.h"
static unsigned checks=0;
void require(bool value,const char*why){++checks;if(!value){printf("FAIL: %s\n",why);throw std::runtime_error(why);}}
void hr(HRESULT h){require(SUCCEEDED(h),"D3D call");}
std::vector<DWORD> read(IDirect3DDevice9*d,IDirect3DSurface9*s,UINT w,UINT h){
    IDirect3DSurface9*cpu=nullptr;hr(d->CreateOffscreenPlainSurface(w,h,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&cpu,nullptr));hr(d->GetRenderTargetData(s,cpu));
    D3DLOCKED_RECT lock;hr(cpu->LockRect(&lock,nullptr,D3DLOCK_READONLY));std::vector<DWORD> out(w*h);
    for(UINT y=0;y<h;++y)memcpy(out.data()+y*w,static_cast<char*>(lock.pBits)+y*lock.Pitch,w*4);
    hr(cpu->UnlockRect());cpu->Release();return out;
}
void bmp(const char*name,const std::vector<DWORD>&p,UINT w,UINT h){
    BITMAPFILEHEADER f={};f.bfType=0x4d42;f.bfOffBits=54;f.bfSize=54+w*h*4;
    BITMAPINFOHEADER i={};i.biSize=40;i.biWidth=w;i.biHeight=-int(h);i.biPlanes=1;i.biBitCount=32;
    std::ofstream o(name,std::ios::binary);o.write((char*)&f,14);o.write((char*)&i,40);o.write((char*)p.data(),p.size()*4);
}
void scenario(IDirect3DDevice9*d,UINT w,UINT h,bool flat){
    IDirect3DSurface9 *saved=nullptr,*target=nullptr,*cpu=nullptr;hr(d->GetRenderTarget(0,&saved));
    hr(d->CreateRenderTarget(w,h,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&target,nullptr));
    hr(d->CreateOffscreenPlainSurface(w,h,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&cpu,nullptr));
    D3DLOCKED_RECT lock;hr(cpu->LockRect(&lock,nullptr,0));
    for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x){
        const bool white=(x>y*1.71f+19&&x<y*1.71f+85)||(y>h*.67f&&x>w*.7f);
        DWORD color=flat?0x005080a0:(white?0x00eeeeee:0x00151515);
        reinterpret_cast<DWORD*>(static_cast<char*>(lock.pBits)+y*lock.Pitch)[x]=color|(((x*7+y*11)&255)<<24);
    }
    hr(cpu->UnlockRect());hr(d->UpdateSurface(cpu,nullptr,target,nullptr));cpu->Release();
    hr(d->SetRenderTarget(0,target));auto before=read(d,target,w,h);
    // Hostile inherited state must not affect the postprocess or leak afterwards.
    hr(d->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE));hr(d->SetRenderState(D3DRS_ALPHATESTENABLE,TRUE));
    hr(d->SetRenderState(D3DRS_ALPHAFUNC,D3DCMP_NEVER));hr(d->SetRenderState(D3DRS_COLORWRITEENABLE,4));
    hr(d->SetRenderState(D3DRS_CULLMODE,D3DCULL_CW));hr(d->SetRenderState(D3DRS_FILLMODE,D3DFILL_WIREFRAME));
    hr(d->SetRenderState(D3DRS_SCISSORTESTENABLE,TRUE));RECT sc={7,9,17,19};hr(d->SetScissorRect(&sc));
    hr(d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP));hr(d->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT));
    float constants[4]={41,42,43,44};hr(d->SetPixelShaderConstantF(0,constants,1));
    IDirect3DVertexBuffer9*vb=nullptr;hr(d->CreateVertexBuffer(96,0,0,D3DPOOL_MANAGED,&vb,nullptr));hr(d->SetStreamSource(0,vb,4,24));
    WosSmaa::FrameEnd();WosSmaa::enabled=false;
    hr(d->BeginScene());require(!WosSmaa::Render(d),"disabled bypass");hr(d->EndScene());require(before==read(d,target,w,h),"OFF exact pixels");
    WosSmaa::enabled=true;hr(d->BeginScene());require(WosSmaa::Render(d),WosSmaa::status);require(!WosSmaa::Render(d),"once per frame");hr(d->EndScene());
    auto after=read(d,target,w,h);unsigned changed=0,intermediate=0;
    for(size_t j=0;j<before.size();++j){require((before[j]>>24)==(after[j]>>24),"alpha unchanged");if((before[j]&0xffffff)!=(after[j]&0xffffff))++changed;if((after[j]&255)>24&&(after[j]&255)<228)++intermediate;}
    require(flat?changed==0:changed>20,"flat preserved / diagonal smoothed");if(!flat)require(intermediate>20,"intermediate edge coverage");
    DWORD v=0;hr(d->GetRenderState(D3DRS_COLORWRITEENABLE,&v));require(v==4,"color mask restored");
    hr(d->GetRenderState(D3DRS_ALPHABLENDENABLE,&v));require(v==TRUE,"blend restored");
    hr(d->GetRenderState(D3DRS_FILLMODE,&v));require(v==D3DFILL_WIREFRAME,"fill restored");
    hr(d->GetRenderState(D3DRS_ALPHAFUNC,&v));require(v==D3DCMP_NEVER,"alpha func restored");
    hr(d->GetSamplerState(0,D3DSAMP_MINFILTER,&v));require(v==D3DTEXF_POINT,"sampler restored");
    float actual[4];hr(d->GetPixelShaderConstantF(0,actual,1));require(!memcmp(actual,constants,sizeof(actual)),"constants restored");
    IDirect3DVertexBuffer9*actualVB=nullptr;UINT offset=0,stride=0;hr(d->GetStreamSource(0,&actualVB,&offset,&stride));require(actualVB==vb&&offset==4&&stride==24,"stream restored after UP");actualVB->Release();
    RECT actualSC;hr(d->GetScissorRect(&actualSC));require(!memcmp(&sc,&actualSC,sizeof(sc)),"scissor restored");
    IDirect3DSurface9*actualRT=nullptr;hr(d->GetRenderTarget(0,&actualRT));require(actualRT==target,"target restored");actualRT->Release();
    if(w==640&&!flat){bmp("before.bmp",before,w,h);bmp("after.bmp",after,w,h);}
    WosSmaa::FrameEnd();D3DVIEWPORT9 partialViewport={1,0,w-1,h,0,1};hr(d->SetViewport(&partialViewport));
    hr(d->BeginScene());require(!WosSmaa::Render(d),"subviewport bypass");hr(d->EndScene());require(after==read(d,target,w,h),"skip exact pixels");
    hr(d->SetStreamSource(0,nullptr,0,0));vb->Release();hr(d->SetRenderTarget(0,saved));saved->Release();target->Release();
    printf("PASS %ux%u %s: changed=%u intermediate=%u\n",w,h,flat?"flat":"diagonal",changed,intermediate);
}
int main(){try{
    HWND wnd=CreateWindowA("STATIC","SMAA HAL test",WS_POPUP,0,0,640,360,nullptr,nullptr,GetModuleHandle(nullptr),nullptr);
    IDirect3D9*api=Direct3DCreate9(D3D_SDK_VERSION);require(api!=nullptr,"D3D9");
    D3DPRESENT_PARAMETERS pp={};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=wnd;pp.BackBufferWidth=640;pp.BackBufferHeight=360;pp.BackBufferFormat=D3DFMT_X8R8G8B8;
    IDirect3DDevice9*d=nullptr;hr(api->CreateDevice(0,D3DDEVTYPE_HAL,wnd,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&d));
    scenario(d,640,360,false);scenario(d,257,129,false);scenario(d,320,180,true);
    WosSmaa::Reset();hr(d->Reset(&pp));scenario(d,640,360,false);
    require(WosSmaa::failures==0,"no failures");WosSmaa::Reset();d->Release();api->Release();DestroyWindow(wnd);
    printf("PASS %u checks; native D3D9 HAL; resize, Reset, state, OFF, alpha, edge coverage\n",checks);return 0;
}catch(const std::exception&e){printf("ERROR %s (SMAA: %s)\n",e.what(),WosSmaa::status);return 1;}}

