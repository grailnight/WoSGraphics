#pragma once
// Snapshot BEFORE a real primary draw. The draw owns normal D3D9 side effects
// (notably Draw*UP clears stream/index bindings), so never restore those here.
// Viewport and scissor are not captured or restored: D3D9 resets them only when
// render target index 0 changes, and this state never touches index 0.
#include <d3d9.h>
namespace WosDeferredPrimary {
template<class Device> struct StateT {
    using Setter=HRESULT(STDMETHODCALLTYPE*)(Device*,IDirect3DPixelShader9*);
    Device*d=nullptr;Setter setShader=nullptr;
    IDirect3DSurface9*aux[3]={};IDirect3DPixelShader9*shader=nullptr;
    DWORD masks[3]={};float tag[4]={};
    bool saveTag=false,valid=false;
    StateT(const StateT&)=delete;StateT&operator=(const StateT&)=delete;
    StateT(Device*device,Setter setter,bool changesTag):d(device),setShader(setter),saveTag(changesTag){
        if(!d||!setter)return;
        for(unsigned i=0;i<3;++i){HRESULT hr=d->GetRenderTarget(i+1,&aux[i]);if(FAILED(hr)&&hr!=D3DERR_NOTFOUND)return;}
        if(FAILED(d->GetPixelShader(&shader)))return;
        const D3DRENDERSTATETYPE states[]={D3DRS_COLORWRITEENABLE1,D3DRS_COLORWRITEENABLE2,D3DRS_COLORWRITEENABLE3};
        for(unsigned i=0;i<3;++i)if(FAILED(d->GetRenderState(states[i],&masks[i])))return;
        if(saveTag&&FAILED(d->GetPixelShaderConstantF(221,tag,1)))return;
        valid=true;
    }
    bool Restore(){
        if(!valid)return false;bool ok=true;auto check=[&](HRESULT hr){ok=SUCCEEDED(hr)&&ok;};
        // RT0, depth, native depth/blend/alpha state and native input side effects
        // are untouched. Only our three auxiliary targets and masks are restored.
        for(unsigned i=0;i<3;++i)check(d->SetRenderTarget(i+1,aux[i]));
        const D3DRENDERSTATETYPE states[]={D3DRS_COLORWRITEENABLE1,D3DRS_COLORWRITEENABLE2,D3DRS_COLORWRITEENABLE3};
        for(unsigned i=0;i<3;++i)check(d->SetRenderState(states[i],masks[i]));
        if(saveTag)check(d->SetPixelShaderConstantF(221,tag,1));
        check(setShader(d,shader));
        if(ok)valid=false;return ok;
    }
    ~StateT(){if(valid)Restore();for(auto*p:aux)if(p)p->Release();if(shader)shader->Release();}
};
using State=StateT<IDirect3DDevice9>;
}
