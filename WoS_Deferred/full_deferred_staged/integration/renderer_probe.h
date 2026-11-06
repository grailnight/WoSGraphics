#pragma once
// Read-only, one-frame D3D9 inventory. No render state is changed by the probe.
#include <d3dcompiler.h>
#include <unordered_map>
#include "engine_probe.h"
namespace WosProbe {
static FILE *trace = nullptr, *constants = nullptr;
static std::string folder;
static std::unordered_map<IUnknown *, unsigned> resources;
static std::unordered_map<IUnknown *, unsigned> shaders;
static unsigned draws = 0;
static bool suppress = false, failed = false;
static const char *status = "F7: capture one frame for deferred renderer research";
static std::string Root() {
    char p[MAX_PATH] = {};
    GetModuleFileNameA(g_module, p, MAX_PATH);
    std::string r(p); r.resize(r.find_last_of("\\/") + 1);
    return r + "WoS_Deferred";
}
static unsigned Id(IUnknown *p) {
    if (!p) return 0;
    auto it = resources.find(p);
    if (it != resources.end()) return it->second;
    unsigned id = static_cast<unsigned>(resources.size()) + 1;
    p->AddRef(); resources.emplace(p, id); return id;
}
static void Finish(const char *reason) {
    if (!trace) return;
    WosEngineProbe::Finish(reason);
    std::fprintf(trace, "END\t%s\tdraws=%u\tio_error=%d\n", reason, draws, failed || std::ferror(constants) || std::ferror(trace));
    bool ok = !failed && !std::ferror(trace) && !std::ferror(constants);
    ok = (std::fclose(trace) == 0) && ok;
    ok = (std::fclose(constants) == 0) && ok;
    trace = constants = nullptr;
    for (auto &e : resources) e.first->Release();
    for (auto &e : shaders) e.first->Release();
    resources.clear(); shaders.clear();
    status = ok ? "Capture saved in WoS_Deferred/captures" : "Capture write failed; check disk space";
}
template<class T> static unsigned Shader(T *shader, const char *stage) {
    if (!shader) return 0;
    auto it = shaders.find(shader);
    if (it != shaders.end()) return it->second;
    unsigned id = static_cast<unsigned>(shaders.size()) + 1;
    UINT size = 0;
    if (FAILED(shader->GetFunction(nullptr, &size)) || size == 0 || size > 1024 * 1024) return 0;
    std::vector<unsigned char> bytes(size);
    if (FAILED(shader->GetFunction(bytes.data(), &size))) return 0;
    shader->AddRef(); shaders.emplace(shader, id);
    char name[64]; std::snprintf(name, sizeof(name), "\\%s_%04u", stage, id);
    std::string path = folder + name;
    FILE *out = std::fopen((path + ".cso").c_str(), "wb");
    if (out) { if (std::fwrite(bytes.data(), 1, size, out) != size) failed = true; if (std::fclose(out)) failed = true; }
    else failed = true;
    using DisassembleFn = HRESULT(WINAPI *)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob **);
    static HMODULE compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static auto disassemble = compiler ? reinterpret_cast<DisassembleFn>(GetProcAddress(compiler, "D3DDisassemble")) : nullptr;
    ID3DBlob *asmText = nullptr;
    if (disassemble && SUCCEEDED(disassemble(bytes.data(), size, 0, nullptr, &asmText))) {
        out = std::fopen((path + ".asm").c_str(), "wb");
        if (out) { if (std::fwrite(asmText->GetBufferPointer(), 1, asmText->GetBufferSize(), out) != asmText->GetBufferSize()) failed = true; if (std::fclose(out)) failed = true; }
        else failed = true;
        asmText->Release();
    }
    std::fprintf(trace, "SHADER\t%s\t%u\tbytes=%u\n", stage, id, size);
    return id;
}
static void Begin(IDirect3DDevice9 *d) {
    if (trace) return;
    std::string root = Root(); CreateDirectoryA(root.c_str(), nullptr);
    root += "\\captures"; CreateDirectoryA(root.c_str(), nullptr);
    SYSTEMTIME t; GetLocalTime(&t);
    char suffix[100]; std::snprintf(suffix, sizeof(suffix), "\\%04u%02u%02u_%02u%02u%02u_%03u_%llu", t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,GetTickCount64());
    folder = root + suffix;
    if (!CreateDirectoryA(folder.c_str(), nullptr)) { status = "Cannot create capture directory"; return; }
    trace = std::fopen((folder + "\\frame.tsv").c_str(), "wb");
    constants = std::fopen((folder + "\\constants.bin").c_str(), "wb");
    if (!trace || !constants) {
        if (trace) std::fclose(trace); if (constants) std::fclose(constants);
        trace = constants = nullptr; status = "Cannot write capture files"; return;
    }
    draws = 0; failed = false;
    WosEngineProbe::Begin(folder);
    std::fprintf(trace, "WOS_RENDER_PROBE\t1\nCONSTANTS\tper_draw=8320 bytes\tps_float4[224] then vs_float4[256] then ps_int4[16] then vs_int4[16] then ps_bool[16] then vs_bool[16]\n");
    D3DCAPS9 caps = {}; d->GetDeviceCaps(&caps);
    std::fprintf(trace, "CAPS\tMRT=%lu\tPS=%08lx\tVS=%08lx\tmax_texture=%lux%lu\tmax_vs_constants=%lu\n", caps.NumSimultaneousRTs,caps.PixelShaderVersion,caps.VertexShaderVersion,caps.MaxTextureWidth,caps.MaxTextureHeight,caps.MaxVertexShaderConst);
    D3DDEVICE_CREATION_PARAMETERS cp = {}; IDirect3D9 *api = nullptr;
    if (SUCCEEDED(d->GetCreationParameters(&cp)) && SUCCEEDED(d->GetDirect3D(&api))) {
        D3DADAPTER_IDENTIFIER9 adapter = {}; api->GetAdapterIdentifier(cp.AdapterOrdinal, 0, &adapter);
        std::fprintf(trace,"ADAPTER\t%s\tvendor=%08lx\tdevice=%08lx\n",adapter.Description,adapter.VendorId,adapter.DeviceId);
        api->Release();
    }
    status = "Capturing one frame...";
}
static void Surface(const char *kind, unsigned slot, IDirect3DSurface9 *s) {
    if (!s) return;
    D3DSURFACE_DESC desc = {}; HRESULT hr = s->GetDesc(&desc);
    IDirect3DBaseTexture9 *texture = nullptr;
    s->GetContainer(IID_IDirect3DBaseTexture9,reinterpret_cast<void **>(&texture));
    std::fprintf(trace,"%s\t%u\tid=%u\ttexture=%u\t%lux%lu\tfmt=%lu\tmsaa=%u\thr=%08lx\n",kind,slot,Id(s),Id(texture),desc.Width,desc.Height,desc.Format,desc.MultiSampleType,hr);
    if (texture) texture->Release();
}
static void Draw(IDirect3DDevice9 *d, const char *kind, D3DPRIMITIVETYPE type, UINT primitives, uintptr_t caller = 0) {
    if (!trace || suppress) return;
    if (draws >= 10000) { Finish("TRUNCATED_DRAW_LIMIT"); return; }
    WosEngineProbe::Draw(draws, caller);
    IDirect3DPixelShader9 *ps = nullptr; IDirect3DVertexShader9 *vs = nullptr;
    d->GetPixelShader(&ps); d->GetVertexShader(&vs);
    unsigned pid = Shader(ps,"ps"), vid = Shader(vs,"vs");
    if (ps) ps->Release(); if (vs) vs->Release();
    D3DVIEWPORT9 vp = {}; d->GetViewport(&vp);
    std::fprintf(trace,"DRAW\t%u\t%s\ttype=%u\tprimitives=%u\tps=%u\tvs=%u\tviewport=%lu,%lu,%lu,%lu,%.9g,%.9g\tconstants_offset=%llu\n",draws++,kind,type,primitives,pid,vid,vp.X,vp.Y,vp.Width,vp.Height,vp.MinZ,vp.MaxZ,static_cast<unsigned long long>(_ftelli64(constants)));
    for (unsigned i=0;i<4;++i) { IDirect3DSurface9 *s=nullptr; if(SUCCEEDED(d->GetRenderTarget(i,&s)) && s) { Surface("RT",i,s); s->Release(); } }
    IDirect3DSurface9 *depth=nullptr; if(SUCCEEDED(d->GetDepthStencilSurface(&depth)) && depth) { Surface("DS",0,depth); depth->Release(); }
    for(unsigned i=0;i<20;++i) {
        DWORD stage = i<16 ? i : D3DVERTEXTEXTURESAMPLER0+i-16;
        IDirect3DBaseTexture9 *tex=nullptr;
        if (FAILED(d->GetTexture(stage,&tex)) || !tex) continue;
        D3DSURFACE_DESC desc={};
        if(tex->GetType()==D3DRTYPE_TEXTURE) static_cast<IDirect3DTexture9 *>(tex)->GetLevelDesc(0,&desc);
        if(tex->GetType()==D3DRTYPE_CUBETEXTURE) static_cast<IDirect3DCubeTexture9 *>(tex)->GetLevelDesc(0,&desc);
        std::fprintf(trace,"TEX\t%lu\tid=%u\ttype=%u\t%lux%lu\tfmt=%lu\n",stage,Id(tex),tex->GetType(),desc.Width,desc.Height,desc.Format);
        tex->Release();
        DWORD value=0;
        for (unsigned state=1;state<=13;++state) if(SUCCEEDED(d->GetSamplerState(stage,static_cast<D3DSAMPLERSTATETYPE>(state),&value))) std::fprintf(trace,"SAMPLER\t%lu\t%u\t%lu\n",stage,state,value);
    }
    const D3DRENDERSTATETYPE states[]={D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ZFUNC,D3DRS_ALPHATESTENABLE,D3DRS_ALPHAREF,D3DRS_ALPHAFUNC,D3DRS_ALPHABLENDENABLE,D3DRS_SRCBLEND,D3DRS_DESTBLEND,D3DRS_BLENDOP,D3DRS_SEPARATEALPHABLENDENABLE,D3DRS_SRCBLENDALPHA,D3DRS_DESTBLENDALPHA,D3DRS_BLENDOPALPHA,D3DRS_CULLMODE,D3DRS_COLORWRITEENABLE,D3DRS_COLORWRITEENABLE1,D3DRS_COLORWRITEENABLE2,D3DRS_COLORWRITEENABLE3,D3DRS_SRGBWRITEENABLE,D3DRS_STENCILENABLE,D3DRS_STENCILFUNC,D3DRS_STENCILREF,D3DRS_STENCILMASK,D3DRS_STENCILWRITEMASK,D3DRS_STENCILPASS,D3DRS_STENCILFAIL,D3DRS_STENCILZFAIL,D3DRS_DEPTHBIAS,D3DRS_SLOPESCALEDEPTHBIAS,D3DRS_SCISSORTESTENABLE};
    for(auto state:states) { DWORD v=0; if(SUCCEEDED(d->GetRenderState(state,&v))) std::fprintf(trace,"STATE\t%u\t%lu\n",state,v); }
    RECT scissor={}; if(SUCCEEDED(d->GetScissorRect(&scissor))) std::fprintf(trace,"SCISSOR\t%ld,%ld,%ld,%ld\n",scissor.left,scissor.top,scissor.right,scissor.bottom);
    IDirect3DVertexDeclaration9 *decl=nullptr;
    if(SUCCEEDED(d->GetVertexDeclaration(&decl)) && decl) {
        D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH+1]={}; UINT n=MAXD3DDECLLENGTH+1;
        if(SUCCEEDED(decl->GetDeclaration(elements,&n))) for(UINT i=0;i<n;++i) std::fprintf(trace,"DECL\t%u\t%u,%u,%u,%u,%u,%u\n",i,elements[i].Stream,elements[i].Offset,elements[i].Type,elements[i].Method,elements[i].Usage,elements[i].UsageIndex);
        decl->Release();
    }
    float pf[224*4]={},vf[256*4]={}; int pi[16*4]={},vi[16*4]={}; BOOL pb[16]={},vb[16]={};
    HRESULT hrs[]={d->GetPixelShaderConstantF(0,pf,224),d->GetVertexShaderConstantF(0,vf,256),d->GetPixelShaderConstantI(0,pi,16),d->GetVertexShaderConstantI(0,vi,16),d->GetPixelShaderConstantB(0,pb,16),d->GetVertexShaderConstantB(0,vb,16)};
    std::fprintf(trace,"CONSTANT_STATUS\t%08lx,%08lx,%08lx,%08lx,%08lx,%08lx\n",hrs[0],hrs[1],hrs[2],hrs[3],hrs[4],hrs[5]);
    auto write=[&](const void *data,size_t size) { if(std::fwrite(data,1,size,constants)!=size) failed=true; };
    write(pf,sizeof(pf));write(vf,sizeof(vf));write(pi,sizeof(pi));write(vi,sizeof(vi));write(pb,sizeof(pb));write(vb,sizeof(vb));
    if(failed) Finish("IO_ERROR");
}
using PresentFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
static PresentFn originalPresent=nullptr;
static void (*frameEnd)()=nullptr;
static void (*afterPresent)(IDirect3DDevice9*,HRESULT)=nullptr;
static HRESULT STDMETHODCALLTYPE Present(IDirect3DDevice9 *d,const RECT *src,const RECT *dst,HWND wnd,const RGNDATA *dirty) {
    if(frameEnd)frameEnd();
    Finish("PRESENT");
    HRESULT hr=originalPresent(d,src,dst,wnd,dirty);
    if(afterPresent)afterPresent(d,hr);
    if(SUCCEEDED(hr) && GetForegroundWindow()==g_window && (GetAsyncKeyState(VK_F7)&1)) Begin(d);
    return hr;
}
}