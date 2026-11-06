#pragma once
namespace WosRain {
static IDirect3DPixelShader9* shader=nullptr;
static bool faulted=false;
static void Reset(){if(shader){shader->Release();shader=nullptr;}faulted=false;}
static bool Render(IDirect3DDevice9*d,IDirect3DTexture9*hdr,IDirect3DTexture9*depth,
                   const float*inverse,const double*origin,const std::string&directory,float intensity){
    if(faulted||intensity<=0)return false;
    WosVolumetric::HostScope busy;
    if(!shader){auto code=WosIndirectMaterials::Read(directory+"\\rainfall.cso");
        if(code.empty()||FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(code.data()),&shader))){faulted=true;return false;}}
    D3DSURFACE_DESC desc{};if(FAILED(hdr->GetLevelDesc(0,&desc)))return false;
    IDirect3DSurface9*target=nullptr;if(FAILED(hdr->GetSurfaceLevel(0,&target)))return false;
    WosIndirect::State state(d);bool ok=state.valid;
    auto check=[&](HRESULT hr){ok=SUCCEEDED(hr)&&ok;};
    if(ok){
        for(unsigned i=0;i<16;++i)check(d->SetTexture(i,nullptr));
        check(d->SetDepthStencilSurface(nullptr));
        for(unsigned i=1;i<state.slots;++i)check(d->SetRenderTarget(i,nullptr));
        check(d->SetRenderTarget(0,target));
        check(d->SetVertexShader(nullptr));check(d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1));
        for(auto rs:{D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHATESTENABLE,D3DRS_STENCILENABLE,D3DRS_SCISSORTESTENABLE,
             D3DRS_SRGBWRITEENABLE,D3DRS_FOGENABLE,D3DRS_CLIPPLANEENABLE,D3DRS_SEPARATEALPHABLENDENABLE,D3DRS_WRAP0})check(d->SetRenderState(rs,0));
        check(d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));check(d->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID));
        check(d->SetRenderState(D3DRS_COLORWRITEENABLE,7));check(d->SetRenderState(D3DRS_MULTISAMPLEMASK,0xffffffff));
        check(d->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE));check(d->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_ONE));
        check(d->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_ONE));check(d->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD));
        check(d->SetTexture(4,depth));
        check(d->SetSamplerState(4,D3DSAMP_MINFILTER,D3DTEXF_POINT));check(d->SetSamplerState(4,D3DSAMP_MAGFILTER,D3DTEXF_POINT));
        check(d->SetSamplerState(4,D3DSAMP_MIPFILTER,D3DTEXF_NONE));check(d->SetSamplerState(4,D3DSAMP_SRGBTEXTURE,0));
        check(d->SetSamplerState(4,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));check(d->SetSamplerState(4,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));
        float eye[4]={float(origin[0]),float(origin[1]),float(origin[2]),float(GetTickCount64()%600000)*.001f};
        float rain[4]={intensity,45.f,.025f,14.f};
        check(d->SetPixelShaderConstantF(0,inverse,4));check(d->SetPixelShaderConstantF(4,eye,1));check(d->SetPixelShaderConstantF(5,rain,1));
        check(d->SetPixelShader(shader));
        if(ok)ok=WosIndirect::Quad(d,desc.Width,desc.Height);
    }
    bool restored=state.Restore();if(!restored)faulted=true;
    target->Release();return ok&&restored;
}
}
