#pragma once
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include "sky_lut.generated.h"
#include "sky_palette.generated.h"
#include "sky_shader.generated.h"
namespace WosSkyIBL {
static bool enabled=true;
static float diffuseStrength=.65f,specularStrength=1.f;
static float material[8]={.4f,.95f,.12f,.04f,0.f,.35f,.25f,.5f};
static IDirect3DTexture9* lookup[4]={};
static IDirect3DBaseTexture9* profileTexture=nullptr;
static IDirect3DDevice9* owner=nullptr;
static IDirect3DPixelShader9* shader=nullptr;
static int profile=-1;
static float colors[12]={};
static uint32_t skyScene=0;
static uint64_t serial=1,captured=0;
static unsigned captures=0,binds=0,failures=0;
static uint32_t resolveScene=0;
static bool valid=false;
static void Reset(){
    if(shader)shader->Release();shader=nullptr;
    for(auto& t:lookup){if(t)t->Release();t=nullptr;}
    if(profileTexture)profileTexture->Release();profileTexture=nullptr;
    owner=nullptr;valid=false;skyScene=0;captured=0;profile=-1;
}
static int Profile(IDirect3DTexture9*t){
    D3DSURFACE_DESC desc={};
    if(FAILED(t->GetLevelDesc(0,&desc))||desc.Format!=D3DFMT_DXT5||desc.Width!=4096||(desc.Height!=512&&desc.Height!=1024))return -1;
    D3DLOCKED_RECT r={};if(FAILED(t->LockRect(0,&r,nullptr,D3DLOCK_READONLY)))return -1;
    unsigned short tag[4]={};bool ok=r.Pitch>=64;
    if(ok)for(unsigned i=0;i<4;++i){
        auto p=static_cast<const unsigned char*>(r.pBits)+i*16+8;
        unsigned short a,b;unsigned indices;
        std::memcpy(&a,p,2);std::memcpy(&b,p+2,2);std::memcpy(&indices,p+4,4);
        tag[i]=a;ok=ok&&a==b&&indices==0;
    }
    t->UnlockRect(0);
    if(!ok||tag[0]!=0xf81f||tag[1]!=0x07e0||(tag[3]!=0x07e0&&tag[3]!=0xffe0))return -1;
    unsigned x=tag[2];
    unsigned id=((x&0xf800)?1:0)|((x&0x07e0)?2:0)|((x&31)?4:0)|((tag[3]&0xf800)?8:0);
    unsigned expected=((id&1)?0xf800:0)|((id&2)?0x07e0:0)|((id&4)?31:0);
    return x==expected&&id<10?int(id):-1;
}
// Call ONLY for the classified main sky pass, before the deferred resolve.
// Current scene identity and frame age prevent a previous level lighting interiors.
static void Capture(IDirect3DDevice9*d,uint32_t scene){
    if(!scene)return;
    D3DVIEWPORT9 vp={};if(FAILED(d->GetViewport(&vp))||vp.Width<640||vp.Height<360)return;
    if(owner&&owner!=d)Reset();owner=d;
    float native[12]={};
    if(FAILED(d->GetPixelShaderConstantF(5,native,3))){valid=false;return;}
    for(unsigned i=0;i<12;++i)if(!std::isfinite(native[i])){valid=false;return;}
    IDirect3DBaseTexture9* bound=nullptr;
    if(FAILED(d->GetTexture(0,&bound))||!bound){valid=false;return;}
    if(bound!=profileTexture){
        if(profileTexture)profileTexture->Release();profileTexture=bound;bound->AddRef();
        IDirect3DTexture9*t=nullptr;profile=-1;
        if(SUCCEEDED(bound->QueryInterface(IID_IDirect3DTexture9,reinterpret_cast<void**>(&t)))){profile=Profile(t);t->Release();}
    }
    bound->Release();
    if(profile>=0){for(int i=0;i<12;++i)colors[i]=WosSkyIBLData::palette[profile][i];}
    else {
        float peakH=(std::max)({native[4],native[5],native[6],.00001f});
        float peakZ=(std::max)({native[8],native[9],native[10],.00001f});
        for(unsigned c=0;c<3;++c){
            colors[c]=std::pow((std::max)(native[4+c],0.f)/peakH,2.4f)*peakH;
            colors[4+c]=(std::max)(native[c],0.f);
            colors[8+c]=std::pow((std::max)(native[8+c],0.f)/peakZ,4.f)*peakZ*.68f;
        }
    }
    valid=true;skyScene=scene;captured=serial;++captures;
}
static bool Upload(IDirect3DDevice9*d,unsigned slot,unsigned width,unsigned height,const unsigned short* data){
    if(lookup[slot])return true;
    if(FAILED(d->CreateTexture(width,height,1,0,D3DFMT_A16B16G16R16F,D3DPOOL_MANAGED,&lookup[slot],nullptr)))return false;
    D3DLOCKED_RECT rect={};
    if(FAILED(lookup[slot]->LockRect(0,&rect,nullptr,0))){lookup[slot]->Release();lookup[slot]=nullptr;return false;}
    for(unsigned y=0;y<height;++y)std::memcpy(static_cast<char*>(rect.pBits)+y*rect.Pitch,data+y*width*4,width*8);
    lookup[slot]->UnlockRect(0);return true;
}
// The caller's ALL state block must encompass Bind and the resolve draw.
static bool Bind(IDirect3DDevice9*d,uint32_t scene){
    float controls[4]={0,diffuseStrength,specularStrength,profile>=0?1.f:0.f};
    bool ready=enabled&&valid&&scene==skyScene&&serial>=captured&&serial-captured<=1&&owner==d;
    if(ready){
        using namespace WosSkyIBLData;
        ready=Upload(d,0,256,33,sky_weights_0)&&Upload(d,1,256,33,sky_weights_1)&&Upload(d,2,256,33,sky_weights_2)&&Upload(d,3,64,64,sky_brdf);
        if(!ready)++failures;
    }
    bool ok=true;auto check=[&](HRESULT hr){ok=SUCCEEDED(hr)&&ok;};
    if(ready){
        for(unsigned i=0;i<4;++i){
            check(d->SetTexture(11+i,lookup[i]));
            check(d->SetSamplerState(11+i,D3DSAMP_MINFILTER,D3DTEXF_LINEAR));
            check(d->SetSamplerState(11+i,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR));
            check(d->SetSamplerState(11+i,D3DSAMP_MIPFILTER,D3DTEXF_NONE));
            check(d->SetSamplerState(11+i,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));
            check(d->SetSamplerState(11+i,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));
            check(d->SetSamplerState(11+i,D3DSAMP_SRGBTEXTURE,0));
        }
        check(d->SetPixelShaderConstantF(45,colors,3));
        controls[0]=ok?1.f:0.f;if(ok)++binds;
    }
    check(d->SetPixelShaderConstantF(48,controls,1));return ok;
}
static void FrameEnd(){++serial;}
// Render inside the existing resolve's ALL-state scope, with its G-buffer and
// constants still bound. Caller restores all native state after the HDR copy.
template<class Draw> static bool Render(IDirect3DDevice9*d,uint32_t scene,int view,Draw draw,
    HRESULT(STDMETHODCALLTYPE*setter)(IDirect3DDevice9*,IDirect3DPixelShader9*)=nullptr){
    resolveScene=scene;
    if(view!=0||!enabled||!valid||skyScene!=scene||serial<captured||serial-captured>1)return true;
    if(!Bind(d,scene))return false;
    if(!shader&&FAILED(d->CreatePixelShader(WosSkyIBLData::shader,&shader))){++failures;return false;}
    bool ok=true;auto check=[&](HRESULT hr){ok=SUCCEEDED(hr)&&ok;};
    check(setter?setter(d,shader):d->SetPixelShader(shader));
    check(d->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE));
    check(d->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_ONE));
    check(d->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_ONE));
    check(d->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD));
    check(d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,FALSE));
    check(d->SetRenderState(D3DRS_COLORWRITEENABLE,7));
    if(ok)ok=draw();
    // Subsequent copy/adapter passes must not inherit additive blend.
    check(d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE));
    check(d->SetRenderState(D3DRS_COLORWRITEENABLE,15));
    for(unsigned i=11;i<=14;++i)check(d->SetTexture(i,nullptr));
    if(!ok)++failures;return ok;
}
}
