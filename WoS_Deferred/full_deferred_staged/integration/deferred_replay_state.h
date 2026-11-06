#pragma once
// Narrow state snapshot for material replay only, taken AFTER the native draw.
// BeginBatch and Flush change more state and must keep their full State object.
// SetRenderTarget resets viewport AND scissor; caller must set both for replay.
// Draw*UP may clear stream0 and indices, so those post-native bindings are saved.
#include <d3d9.h>
namespace WosDeferredReplay {
static const D3DRENDERSTATETYPE RenderStates[]={
    D3DRS_COLORWRITEENABLE,D3DRS_COLORWRITEENABLE1,D3DRS_COLORWRITEENABLE2,D3DRS_COLORWRITEENABLE3,
    D3DRS_ZWRITEENABLE,D3DRS_ZFUNC,D3DRS_ALPHABLENDENABLE,D3DRS_SEPARATEALPHABLENDENABLE
};
template<class Device> struct StateT {
    using SetPixelShaderFn=HRESULT(STDMETHODCALLTYPE*)(Device*,IDirect3DPixelShader9*);
    Device*device=nullptr;
    SetPixelShaderFn setPixelShader=nullptr;
    IDirect3DSurface9*rt[4]={},*depth=nullptr;
    IDirect3DPixelShader9*shader=nullptr;
    IDirect3DVertexBuffer9*stream=nullptr;
    IDirect3DIndexBuffer9*indices=nullptr;
    D3DVIEWPORT9 viewport={};
    RECT scissor={};
    DWORD states[8]={};
    float tag[4]={};
    UINT streamOffset=0,streamStride=0;
    bool saveTag=false,valid=false;
    StateT(const StateT&)=delete;
    StateT&operator=(const StateT&)=delete;
    StateT(Device*d,SetPixelShaderFn setter,bool changesTag):device(d),setPixelShader(setter),saveTag(changesTag){
        if(!device||!setPixelShader)return;
        for(unsigned i=0;i<4;++i){HRESULT hr=device->GetRenderTarget(i,&rt[i]);if(FAILED(hr)&&(i==0||hr!=D3DERR_NOTFOUND))return;}
        if(!rt[0])return;
        HRESULT hr=device->GetDepthStencilSurface(&depth);if(FAILED(hr)&&hr!=D3DERR_NOTFOUND)return;
        if(FAILED(device->GetViewport(&viewport))||FAILED(device->GetScissorRect(&scissor)))return;
        for(unsigned i=0;i<8;++i)if(FAILED(device->GetRenderState(RenderStates[i],&states[i])))return;
        if(FAILED(device->GetPixelShader(&shader)))return;
        if(saveTag&&FAILED(device->GetPixelShaderConstantF(221,tag,1)))return;
        if(FAILED(device->GetStreamSource(0,&stream,&streamOffset,&streamStride))||FAILED(device->GetIndices(&indices)))return;
        valid=true;
    }
    bool Restore(){
        if(!valid)return false;
        bool ok=true;auto check=[&](HRESULT hr){ok=SUCCEEDED(hr)&&ok;};
        // Clear auxiliary targets before restoring RT0 to avoid mixed-size or
        // mixed-format transient bindings; restore old indirect MRTs explicitly.
        check(device->SetDepthStencilSurface(nullptr));
        for(unsigned i=1;i<4;++i)check(device->SetRenderTarget(i,nullptr));
        for(unsigned i=0;i<4;++i)check(device->SetRenderTarget(i,rt[i]));
        check(device->SetDepthStencilSurface(depth));
        for(unsigned i=0;i<8;++i)check(device->SetRenderState(RenderStates[i],states[i]));
        if(saveTag)check(device->SetPixelShaderConstantF(221,tag,1));
        check(setPixelShader(device,shader));
        check(device->SetStreamSource(0,stream,streamOffset,streamStride));
        check(device->SetIndices(indices));
        // Both resets happen as a side effect of SetRenderTarget. Restore last.
        check(device->SetScissorRect(&scissor));
        check(device->SetViewport(&viewport));
        if(ok)valid=false;
        // A failed restore leaves the snapshot armed for the destructor's one
        // best-effort retry. The caller must still drop its batch on false.
        return ok;
    }
    ~StateT(){
        if(valid)Restore();
        for(auto*p:rt)if(p)p->Release();
        if(depth)depth->Release();if(shader)shader->Release();if(stream)stream->Release();if(indices)indices->Release();
    }
};
using State=StateT<IDirect3DDevice9>;
}
