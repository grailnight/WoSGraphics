#pragma once
// Material class per primary draw, named by its diffuse texture.
//
// Legacy WoS assets carry no metalness or roughness, but their texture names
// say what a surface is (s_mat_metal_rusty_03_dif, s_brick4_nor ...).
// WoS_Deferred/pbr_materials/build_texture_classes.py reads every texture in
// the packs, classifies it by name and keys it by its pixels, because a draw
// only exposes an IDirect3DTexture9. The key is FNV-1a 64 over the fourcc, the
// size and the bytes of the first mip whose larger side is <= 64.
//
// The class travels in texel 31.y of the draw's light set, beside the rain
// class in .x, so neither the G-buffer nor any material shader changes. The
// resolve and the sky pass read it and use the class's roughness window,
// metalness and reflectance floor (pbr_legacy_bridge.hlsl, c53 and c56..c69).
#include <d3d9.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace WosMaterialClasses {
constexpr unsigned ClassCount=14;
// Ids are shared with texture_classes.bin and the shaders; append, never renumber.
// 12 is the facade marker below, never a table row, a setting or a menu line.
static const char* const Names[ClassCount]={"unknown","metal","concrete","brick","asphalt","glass",
                                            "wood","roof","rubber","tile","foliage","painted_metal",
                                            "facade","symbiote"};
// Per class: roughness floor, roughness ceiling, metalness, minimum reflectance
// coverage. The texture's own glossiness is spread across floor..ceiling.
static float table[ClassCount][4]={
    {0,0,0,0},
    {.22f,.55f,1,.6f},    // metal
    {.72f,.95f,0,0},      // concrete
    {.78f,.97f,0,0},      // brick
    {.80f,.97f,0,0},      // asphalt
    {.12f,.25f,0,1},      // glass: F0 .04, F90 1; a flat pane at .04 flashes the sun at ~176x
    {.55f,.85f,0,0},      // wood
    {.75f,.97f,0,0},      // roof
    {.65f,.90f,0,0},      // rubber
    {.25f,.55f,0,.2f},    // tile
    {.55f,.85f,0,0},      // foliage
    {.32f,.62f,0,.15f},   // painted metal: paint is a dielectric
    {0,0,0,0},            // facade: never read
    // symbiote: wet goo; F0 is its violet specular colour, the floor keeps the hue.
    // Metal 1 halves the bark's diffuse at strength .5: Intensity 2.585 lights it twice as bright as native.
    {.15f,.40f,1,.25f},
};
static const float defaults[ClassCount][4]={
    {0,0,0,0},{.22f,.55f,1,.6f},{.72f,.95f,0,0},{.78f,.97f,0,0},{.80f,.97f,0,0},{.12f,.25f,0,1},
    {.55f,.85f,0,0},{.75f,.97f,0,0},{.65f,.90f,0,0},{.25f,.55f,0,.2f},{.55f,.85f,0,0},{.32f,.62f,0,.15f},
    {0,0,0,0},{.15f,.40f,1,.25f}};
// 0 leaves every pixel on the specular-mask estimate, bit for bit.
static float strength=1.f;
// How far full wetness lowers a class's roughness window (the shader caps it at .4 of the key).
static float rainPull=.6f;
static std::string tablePath;
static std::unordered_map<uint64_t,uint8_t> keys;
static bool loaded=false,loadFailed=false;
// Per-frame counters for the menu: primary draws seen and those given a class.
static unsigned drawsSeen=0,drawsClassified=0,lastSeen=0,lastClassified=0;
static unsigned classDraws[ClassCount]={},lastClassDraws[ClassCount]={};
// Facade draws share one atlas and name no class; the resolve finds their
// windows per pixel from the G-buffer (WoSFacadeClass). Never a table row.
constexpr uint8_t Facade=12;
// Rows the table file, the settings and the menu may hold.
static bool IsRow(unsigned c){return c&&c<ClassCount&&c!=Facade;}
static unsigned facadeDraws=0,lastFacadeDraws=0;

static bool Load(){
    if(loaded)return true;if(loadFailed||tablePath.empty())return false;
    loadFailed=true;
    FILE*f=nullptr;if(fopen_s(&f,tablePath.c_str(),"rb")||!f)return false;
    char magic[4]={};uint32_t version=0,count=0;
    bool ok=fread(magic,4,1,f)==1&&fread(&version,4,1,f)==1&&fread(&count,4,1,f)==1&&
            !std::memcmp(magic,"WPMC",4)&&version==1&&count<(1u<<20);
    try{
        keys.reserve(count);
        for(uint32_t i=0;ok&&i<count;++i){
            uint64_t key=0;uint8_t cls=0,role=0;uint16_t pad=0;
            ok=fread(&key,8,1,f)==1&&fread(&cls,1,1,f)==1&&fread(&role,1,1,f)==1&&fread(&pad,2,1,f)==1;
            if(ok&&IsRow(cls))keys[key]=cls;
        }
    }catch(...){ok=false;}
    fclose(f);
    if(!ok||keys.empty()){keys.clear();return false;}
    loaded=true;loadFailed=false;return true;
}
static bool Active(){return strength>0&&Load();}

// Samplers whose texture is the surface's own colour. Blend masks, atlases,
// property maps, grime and decals are left out on purpose: they name no surface.
static const char* const DiffuseNames[]={"gDiffuseMap0Sampler","gDiffuseMap1Sampler","diffuse_Sampler",
    "cleanDiffuseSampler","diffuseCleanSampler","diffuseSampler","diffuseSampler1","diffuseSampler2",
    "colorTexture0Sampler","colorTexture1Sampler"};

// The metallic goo shader (metallicTint, edgeTint, thin film) names its own
// surface: most city goo binds a 4x4 placeholder as its diffuse map
// (bDiffuseMap off), so its texture says nothing and the draw stayed class 0.
constexpr unsigned SymbioteShader=254;
constexpr uint8_t Symbiote=13;

// Sampler registers of the diffuse layers, from the native shader's CTAB.
// A shader that samples the building property map is a facade: returns 255;
// the metallic goo shader returns SymbioteShader.
static unsigned DiffuseSamplers(const std::vector<unsigned char>&code,uint8_t regs[4]){
    unsigned found=0;size_t pos=4;
    while(pos+8<=code.size()){
        uint32_t token=0;std::memcpy(&token,&code[pos],4);
        if((token&0xffff)!=0xfffe){pos+=4;continue;}
        size_t size=(token>>16)*4ull;
        if(pos+8+size>code.size())break;
        if(std::memcmp(&code[pos+4],"CTAB",4)){pos+=4+size;continue;}
        const size_t base=pos+8;uint32_t header[7]={};
        if(size<28)break;std::memcpy(header,&code[base],28);
        const uint32_t count=header[3],info=header[4];
        for(uint32_t i=0;i<count&&found<4;++i){
            size_t at=base+info+i*20ull;if(at+20>pos+4+size)break;
            uint32_t nameOffset=0;uint16_t regset=0,index=0;
            std::memcpy(&nameOffset,&code[at],4);std::memcpy(&regset,&code[at+4],2);std::memcpy(&index,&code[at+6],2);
            if(base+nameOffset>=pos+4+size)continue;
            const char*name=reinterpret_cast<const char*>(&code[base+nameOffset]);
            size_t limit=pos+4+size-(base+nameOffset);
            if(strnlen(name,limit)==12&&!std::strncmp(name,"metallicTint",limit))return SymbioteShader;
            if(regset!=3||index>15)continue;
            if(strnlen(name,limit)==15&&!std::strncmp(name,"propertySampler",limit))return 255;
            for(const char*wanted:DiffuseNames)
                if(strnlen(name,limit)==std::strlen(wanted)&&!std::strncmp(name,wanted,limit)){regs[found++]=uint8_t(index);break;}
        }
        break;
    }
    return found;
}

// {5d1b7c7e-6a0e-4a8e-9b1f-57c1a55e7a11}: the class is stored on the texture
// object itself. A streamed-out texture takes it along, so a new texture that
// the allocator puts at the same address is keyed afresh, not mistaken for it.
static const GUID ClassTag={0x5d1b7c7e,0x6a0e,0x4a8e,{0x9b,0x1f,0x57,0xc1,0xa5,0x5e,0x7a,0x11}};

static uint64_t Fnv(const void*data,size_t size,uint64_t h){
    const unsigned char*p=static_cast<const unsigned char*>(data);
    for(size_t i=0;i<size;++i){h^=p[i];h*=0x100000001B3ull;}
    return h;
}
static uint8_t Key(IDirect3DTexture9*t){
    D3DSURFACE_DESC desc={};if(FAILED(t->GetLevelDesc(0,&desc)))return 0;
    const UINT bytes=desc.Format==D3DFMT_DXT1?8u:(desc.Format==D3DFMT_DXT3||desc.Format==D3DFMT_DXT5)?16u:0u;
    if(!bytes||desc.Pool!=D3DPOOL_MANAGED)return 0;
    const UINT levels=t->GetLevelCount();if(!levels)return 0;
    UINT level=levels-1;
    for(UINT i=0;i<levels;++i){
        UINT w=desc.Width>>i,h=desc.Height>>i;if(!w)w=1;if(!h)h=1;
        if((w>h?w:h)<=64){level=i;break;}
    }
    UINT dims[2]={desc.Width>>level,desc.Height>>level};if(!dims[0])dims[0]=1;if(!dims[1])dims[1]=1;
    const UINT bw=(dims[0]+3)/4?(dims[0]+3)/4:1,bh=(dims[1]+3)/4?(dims[1]+3)/4:1,row=bw*bytes;
    D3DLOCKED_RECT lock={};
    if(FAILED(t->LockRect(level,&lock,nullptr,D3DLOCK_READONLY|D3DLOCK_NOSYSLOCK)))return 0;
    uint64_t h=0xCBF29CE484222325ull;DWORD fourcc=desc.Format;
    h=Fnv(&fourcc,4,h);h=Fnv(dims,8,h);
    const unsigned char*bits=static_cast<const unsigned char*>(lock.pBits);
    for(UINT y=0;y<bh;++y)h=Fnv(bits+size_t(y)*lock.Pitch,row,h);
    t->UnlockRect(level);
    auto it=keys.find(h);return it==keys.end()?0:it->second;
}
static uint8_t ClassOfTexture(IDirect3DBaseTexture9*base){
    if(base->GetType()!=D3DRTYPE_TEXTURE)return 0;
    uint8_t tag[2]={};DWORD size=sizeof(tag);
    if(SUCCEEDED(base->GetPrivateData(ClassTag,tag,&size))&&size==sizeof(tag))return tag[0];
    tag[0]=Key(static_cast<IDirect3DTexture9*>(base));tag[1]=1;
    base->SetPrivateData(ClassTag,tag,sizeof(tag),0);
    return tag[0];
}

// One class for the draw only when every bound diffuse layer names the same
// one; a blend of brick and concrete, or brick and an unknown layer, stays 0.
static uint8_t ClassOf(IDirect3DDevice9*d,const uint8_t*regs,unsigned count){
    ++drawsSeen;
    if(count==255){++facadeDraws;return Facade;}
    if(count==SymbioteShader){++drawsClassified;++classDraws[Symbiote];return Symbiote;}
    uint8_t cls=0;bool any=false;
    for(unsigned i=0;i<count;++i){
        IDirect3DBaseTexture9*t=nullptr;
        if(FAILED(d->GetTexture(regs[i],&t))||!t)continue;
        uint8_t c=ClassOfTexture(t);t->Release();
        if(!c)return 0;
        if(any&&c!=cls)return 0;
        cls=c;any=true;
    }
    if(cls){++drawsClassified;++classDraws[cls];}
    return cls;
}
static void EndFrame(){
    lastSeen=drawsSeen;lastClassified=drawsClassified;drawsSeen=drawsClassified=0;
    lastFacadeDraws=facadeDraws;facadeDraws=0;
    for(unsigned i=0;i<ClassCount;++i){lastClassDraws[i]=classDraws[i];classDraws[i]=0;}
}
}
