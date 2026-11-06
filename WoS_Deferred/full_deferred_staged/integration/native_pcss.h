#pragma once
// Experimental directional PCSS for seven verified building and road shaders.
// Every substitution requires byte-for-byte original shader matching.
namespace WosPCSS {
static bool enabled=false;
static unsigned hits=0,lastHits=0;
static const char *status="F8: PCSS prototype OFF (seven material variants)";
struct Variant { std::vector<unsigned char> original, patched; IDirect3DPixelShader9 *shader=nullptr; };
static std::vector<Variant> variants;
static std::unordered_map<IDirect3DPixelShader9 *,int> cache;
struct Scale { float uv[4]; };
static std::unordered_map<IDirect3DBaseTexture9 *,Scale> scales;
static bool loaded=false;
static std::vector<unsigned char> Read(const std::string &path) {
    FILE *f=std::fopen(path.c_str(),"rb"); if(!f) return {};
    std::fseek(f,0,SEEK_END); long size=std::ftell(f); std::rewind(f);
    if(size<4 || size>1024*1024 || size%4) { std::fclose(f); return {}; }
    std::vector<unsigned char> bytes(size);
    if(std::fread(bytes.data(),1,size,f)!=static_cast<size_t>(size)) bytes.clear();
    std::fclose(f);return bytes;
}
static void ClearScales() { for(auto &e:scales)e.first->Release();scales.clear(); }
static void Reset() {
    ClearScales();
    for(auto &e:cache)e.first->Release();cache.clear();
    for(auto &v:variants)if(v.shader)v.shader->Release();variants.clear();
    loaded=false;hits=lastHits=0;
}
static void FrameEnd() {
    lastHits=hits;hits=0;ClearScales();
    if(GetForegroundWindow()==g_window && (GetAsyncKeyState(VK_F8)&1)) {
        enabled=!enabled;
        status=enabled?"F8: PCSS prototype ON (building and road shadows)":"F8: PCSS prototype OFF (original shadows)";
    }
}
static void Load(IDirect3DDevice9 *d) {
    if(loaded)return;loaded=true;
    for(int id:{9,11,12,13,17,95,97}) {
        char name[64];std::snprintf(name,sizeof(name),"\\pcss\\ps_%04d",id);
        std::string base=WosProbe::Root()+name;
        Variant v;v.original=Read(base+".original.cso");v.patched=Read(base+".pcss.cso");
        if(v.original.empty()||v.patched.empty())continue;
        if(FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD *>(v.patched.data()),&v.shader)))continue;
        variants.push_back(std::move(v));
    }
    if(variants.size()!=7) status="PCSS: missing or unsupported shaders; original fallback active";
}
static IDirect3DPixelShader9 *Replacement(IDirect3DDevice9 *d,IDirect3DPixelShader9 *ps) {
    Load(d);auto found=cache.find(ps);
    int index=-1;
    if(found!=cache.end())index=found->second;
    else {
        UINT size=0;
        if(SUCCEEDED(ps->GetFunction(nullptr,&size))) {
            bool candidate=false;for(auto &v:variants)if(v.original.size()==size)candidate=true;
            if(candidate) {
                std::vector<unsigned char> code(size);
                if(SUCCEEDED(ps->GetFunction(code.data(),&size)))
                    for(size_t i=0;i<variants.size();++i)if(code==variants[i].original){index=static_cast<int>(i);break;}
            }
        }
        ps->AddRef();cache.emplace(ps,index);
    }
    return index<0?nullptr:variants[index].shader;
}
static void CaptureLightProjection(IDirect3DDevice9 *d) {
    IDirect3DSurface9 *ds=nullptr;
    if(FAILED(d->GetDepthStencilSurface(&ds))||!ds)return;
    D3DSURFACE_DESC desc={};ds->GetDesc(&desc);
    if(desc.Format!=static_cast<D3DFORMAT>(MAKEFOURCC('I','N','T','Z')) || desc.Width!=2048 || desc.Height!=2048){ds->Release();return;}
    IDirect3DBaseTexture9 *tex=nullptr;
    ds->GetContainer(IID_IDirect3DBaseTexture9,reinterpret_cast<void **>(&tex));ds->Release();
    if(!tex)return;
    if(scales.find(tex)!=scales.end()){tex->Release();return;}
    IDirect3DVertexShader9 *vs=nullptr;d->GetVertexShader(&vs);
    int reg=FindCameraMatrixRegister(vs,"viewProj",4);
    if(vs)vs->Release();float m[16]={};
    if(reg<0||FAILED(d->GetVertexShaderConstantF(reg,m,4))){tex->Release();return;}
    for(float v:m)if(!std::isfinite(v)){tex->Release();return;}
    if(std::fabs(m[12])+std::fabs(m[13])+std::fabs(m[14])+std::fabs(m[15]-1)>0.0001f){tex->Release();return;}
    float x=std::sqrt(m[0]*m[0]+m[1]*m[1]+m[2]*m[2]);
    float y=std::sqrt(m[4]*m[4]+m[5]*m[5]+m[6]*m[6]);
    float z=std::sqrt(m[8]*m[8]+m[9]*m[9]+m[10]*m[10]);
    if(x<=0||y<=0||z<1e-8f){tex->Release();return;}
    // Orthographic clip XY -> texture UV contributes the factor 0.5.
    const float sunAngularRadius=0.00465f;
    Scale scale={{0.5f*std::tan(sunAngularRadius)*x/z,0.5f*std::tan(sunAngularRadius)*y/z,12.0f/desc.Width,12.0f/desc.Height}};
    scales.emplace(tex,scale); // Retain GetContainer reference until this frame ends.
}
struct DrawScope {
    IDirect3DDevice9 *device=nullptr;
    IDirect3DPixelShader9 *original=nullptr;
    float saved[4]={};
    DWORD filters[3]={};
    bool changed=false;
    DrawScope(IDirect3DDevice9 *d) {
        if(!enabled||WosProbe::suppress)return;
        CaptureLightProjection(d);
        if(FAILED(d->GetPixelShader(&original))||!original)return;
        auto replacement=Replacement(d,original);
        if(!replacement)return;
        IDirect3DBaseTexture9 *tex=nullptr;
        if(FAILED(d->GetTexture(2,&tex))||!tex)return;
        auto found=scales.find(tex);tex->Release();
        if(found==scales.end())return;
        if(FAILED(d->GetPixelShaderConstantF(210,saved,1)))return;
        const D3DSAMPLERSTATETYPE types[]={D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER,D3DSAMP_MIPFILTER};
        for(int i=0;i<3;++i)if(FAILED(d->GetSamplerState(2,types[i],&filters[i])))return;
        device=d;changed=true;
        if(FAILED(d->SetPixelShaderConstantF(210,found->second.uv,1))){Restore();return;}
        if(FAILED(d->SetSamplerState(2,D3DSAMP_MINFILTER,D3DTEXF_POINT))){Restore();return;}
        if(FAILED(d->SetSamplerState(2,D3DSAMP_MAGFILTER,D3DTEXF_POINT))){Restore();return;}
        if(FAILED(d->SetSamplerState(2,D3DSAMP_MIPFILTER,D3DTEXF_NONE))){Restore();return;}
        if(SUCCEEDED(g_original_set_pixel_shader(d,replacement)))++hits; else Restore();
    }
    void Restore() {
        if(changed) {
            g_original_set_pixel_shader(device,original);
            device->SetPixelShaderConstantF(210,saved,1);
            device->SetSamplerState(2,D3DSAMP_MINFILTER,filters[0]);
            device->SetSamplerState(2,D3DSAMP_MAGFILTER,filters[1]);
            device->SetSamplerState(2,D3DSAMP_MIPFILTER,filters[2]);
        }
        changed=false;
    }
    ~DrawScope() {
        Restore();
        if(original)original->Release();
    }
};
}