#pragma once
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/deferred_lights.h"
// Two complete point-light cubemaps in a 3 x 4 INTZ atlas. Light identities
// are world positions + ranges, never per-material indices. The previous
// frame discovers lights; current-frame native shadow geometry fills all faces.
#include "shadow_work_cache.h"
namespace WosLocalShadow {
using Matrix=WosShadowTransform::Matrix4;
static bool enabled=true;
static int resolution=512;
static float emitterRadius=.18f,bias=.035f,normalBias=.025f,strength=1.f;
struct Light {double p[3]={};float range=0,score=0;float colour[3]={};};
// The light shafts read this frame's observed list at the bloom draw, so
// discovery also runs when only they want it; Prepare still checks enabled.
static bool collectForShafts=false;
static std::vector<Light> observed,pending;
static Light lights[2];
static Matrix matrices[12];
static unsigned count=0,draws=0,skipped=0,failures=0;
static bool valid=false,attempted=false;
static IDirect3DDevice9* owner=nullptr;
static IDirect3DTexture9* atlas=nullptr;
static IDirect3DSurface9 *depth=nullptr,*colour=nullptr;
static unsigned tile=0;
static const char* status="Local shadow maps waiting for lights";

// Static caster layer.
//
// A cube face depends only on its light, and a street light does not move, so
// a rigid caster that has not moved puts exactly the same depth into it every
// frame. Those casters are drawn once into a second atlas with the same
// layout; the per-frame atlas then only receives what changed or animates, and
// the resolve reads min(live, static). A depth test that keeps the nearest
// value produces min() over all casters in any order, so the resolve sees the
// same depth it would if every caster had been drawn into one atlas this frame.
//
// Only vs_0003 casters qualify: their clip position is a function of the
// vertex buffer, the WVP this module writes, and the per-mesh decompression
// constants c7/c8, all of which are part of the draw key together with the
// index range, stream, declaration, pixel shader and raster states. Dynamic
// vertex or index buffers never qualify. Skinned and SpeedTree casters depend
// on bone and wind constants and are always drawn live.
//
// A slot's layer is rebuilt when its light changes, when a world matrix it
// holds is absent from this frame's shadow queues (the object moved or went
// away), and one frame after any key it holds was not drawn at all.
static bool staticEnabled=true;       // [Cache] LocalStatic
static bool staticShaderReady=false;  // the loaded resolve declares LocalStatic (s10)
static IDirect3DTexture9* staticAtlas=nullptr;
static IDirect3DSurface9* staticDepth=nullptr;
struct DrawArgs {uint32_t indexed,type,base,minIndex,vertices,start,count;};
static constexpr unsigned kKeyWords=46;
struct DrawKey {uint32_t w[kKeyWords];};
struct StaticSlot {
    bool active=false,dirty=true;Light light;
    WosShadowWork::WordIndex<kKeyWords> keys;  // value: index into seen
    std::vector<uint32_t> seen;                 // frame stamp per key
    WosShadowWork::WordIndex<16> worlds;        // world matrices the layer holds
    std::vector<Matrix> worldList;
    void Clear(){keys.Clear();seen.clear();worlds.Clear();worldList.clear();}
};
static StaticSlot slots[2];
// World matrices in this frame's and the previous frame's shadow queues. A
// caster enters the layer only once its world stood still for a frame, so
// moving traffic stays live instead of rebuilding a slot every frame.
static WosShadowWork::WordIndex<16> worldsA,worldsB;
static WosShadowWork::WordIndex<16>*frameWorlds=&worldsA,*previousWorlds=&worldsB;
static uint32_t staticFrame=1;
static constexpr size_t kStaticKeyLimit=65536;
static unsigned frameSkippedDraws=0,frameStaticDraws=0,frameLiveDraws=0;
static unsigned lastSkippedDraws=0,lastStaticDraws=0,lastLiveDraws=0;
static unsigned long long staticRebuilds=0,rebuildLight=0,rebuildMoved=0,rebuildUnseen=0,rebuildOverflow=0;
static const char* staticStatus="Static caster layer OFF";
static bool StaticUsable(){return staticEnabled&&staticShaderReady&&staticAtlas&&staticDepth;}
static bool Identical(const Light&a,const Light&b){return !std::memcmp(a.p,b.p,sizeof(a.p))&&a.range==b.range;}

static bool Same(const Light&a,const Light&b){
    double d=0;for(int j=0;j<3;++j)d+=(a.p[j]-b.p[j])*(a.p[j]-b.p[j]);
    return d<.0001&&std::abs(a.range-b.range)<.01f;
}
// Merging below is idempotent (max of score and colour, dedupe by position),
// so a set identical to one merged earlier this frame changes nothing. Most
// draws repeat one of the last few sets; the ring is cleared every frame.
struct RecentSet {unsigned n=0;double origin[3]={};float texels[64]={};};
static RecentSet recentSets[4];static unsigned recentCount=0,recentNext=0;
static unsigned long long observeCalls=0,observeRepeats=0;
static bool RepeatedSet(const float*texels,unsigned n,const double origin[3]){
    for(unsigned i=0;i<recentCount;++i){const RecentSet&r=recentSets[i];
        if(r.n==n&&!std::memcmp(r.origin,origin,sizeof(r.origin))&&!std::memcmp(r.texels,texels,n*8*sizeof(float)))return true;}
    return false;
}
static void RememberSet(const float*texels,unsigned n,const double origin[3]){
    RecentSet&r=recentSets[recentNext];recentNext=(recentNext+1)%4;if(recentCount<4)++recentCount;
    r.n=n;std::memcpy(r.origin,origin,sizeof(r.origin));std::memcpy(r.texels,texels,n*8*sizeof(float));
}
static void Observe(const WosDeferredLights::LightSet&set,const double origin[3]){
    if(!enabled&&!collectForShafts)return;
    unsigned n=unsigned(set.texels[3]);if(n>8)return;
    ++observeCalls;
    if(RepeatedSet(set.texels.data()+8,n,origin)){++observeRepeats;return;}
    RememberSet(set.texels.data()+8,n,origin);
    for(unsigned i=0;i<n;++i){
        const float*p=set.texels.data()+8+i*8;const float*c=p+4;
        if(!(p[3]>0))continue;
        Light l;l.range=1.f/p[3];double dist2=0;
        for(int j=0;j<3;++j){l.p[j]=origin[j]+p[j];dist2+=double(p[j])*p[j];}
        l.score=(std::max)(c[0],(std::max)(c[1],c[2]))/float(1+dist2);
        if(!WosFinite(l.range)||l.range<.2f||!WosFinite(l.score)||l.score<=0)continue;
        for(int j=0;j<3;++j)l.colour[j]=WosFinite(c[j])?(std::max)(c[j],0.f):0.f;
        bool found=false;
        for(auto&other:observed)if(Same(l,other)){
            other.score=(std::max)(other.score,l.score);
            for(int j=0;j<3;++j)other.colour[j]=(std::max)(other.colour[j],l.colour[j]);
            found=true;break;
        }
        if(!found&&observed.size()<256)observed.push_back(l);
    }
}
static void FrameEnd(){
    // A key that was not drawn this frame means the layer held a caster the
    // queues no longer submit; rebuild before the next frame reads it.
    if(valid&&StaticUsable())
        for(unsigned s=0;s<count;++s){
            if(!slots[s].active||slots[s].dirty)continue;
            for(uint32_t stamp:slots[s].seen)if(stamp!=staticFrame){slots[s].dirty=true;break;}
        }
    if(++staticFrame==0)staticFrame=1;
    lastSkippedDraws=frameSkippedDraws;lastStaticDraws=frameStaticDraws;lastLiveDraws=frameLiveDraws;
    frameSkippedDraws=frameStaticDraws=frameLiveDraws=0;
    WosShadowWork::local.Invalidate();
    pending.swap(observed);observed.clear();valid=false;attempted=false;draws=skipped=0;
    recentCount=recentNext=0;
    std::stable_sort(pending.begin(),pending.end(),[](const Light&a,const Light&b){return a.score>b.score;});
    if(pending.size()>2)pending.resize(2);
}
static void ReleaseStatic(){
    if(staticDepth)staticDepth->Release();if(staticAtlas)staticAtlas->Release();
    staticDepth=nullptr;staticAtlas=nullptr;
    for(auto&s:slots){s.Clear();s.active=false;s.dirty=true;}
}
static void Reset(){
    WosShadowWork::local.Reset();
    ReleaseStatic();
    if(depth)depth->Release();if(colour)colour->Release();if(atlas)atlas->Release();if(owner)owner->Release();
    depth=colour=nullptr;atlas=nullptr;owner=nullptr;tile=count=0;valid=false;
    observed.clear();pending.clear();recentCount=recentNext=0;
}
static const float axes[6][9]={
    {0,0,-1, 0,1,0, 1,0,0}, {0,0,1, 0,1,0, -1,0,0},
    {1,0,0, 0,0,1, 0,1,0}, {1,0,0, 0,0,-1, 0,-1,0},
    {1,0,0, 0,1,0, 0,0,1}, {-1,0,0, 0,1,0, 0,0,-1}
};
static Matrix FaceMatrix(const Light&l,unsigned face){
    Matrix m={};const float*a=axes[face];
    double nearZ=.05,farZ=(std::min)(double(l.range),500.0),q=farZ/(farZ-nearZ);
    for(int row=0;row<3;++row){m.v[row*4]=a[row];m.v[row*4+1]=a[3+row];m.v[row*4+2]=float(a[6+row]*q);m.v[row*4+3]=a[6+row];}
    for(int j=0;j<3;++j){m.v[12]-=float(l.p[j]*a[j]);m.v[13]-=float(l.p[j]*a[3+j]);m.v[14]-=float(l.p[j]*a[6+j]*q);m.v[15]-=float(l.p[j]*a[6+j]);}
    m.v[14]-=float(nearZ*q);return m;
}
struct Binding {
    IDirect3DDevice9*d=nullptr;IDirect3DSurface9*rt[4]={},*ds=nullptr;D3DVIEWPORT9 vp={};unsigned slots=0;
    bool Begin(IDirect3DDevice9*dev,IDirect3DSurface9*target=nullptr){
        if(!target)target=depth;
        if(!dev||!colour||!target||FAILED(dev->GetViewport(&vp)))return false;
        d=dev;slots=WosShadowWork::RenderTargetSlots(dev);
        bool ok=slots>0;
        for(unsigned i=0;i<slots;++i){HRESULT h=d->GetRenderTarget(i,&rt[i]);ok=(SUCCEEDED(h)||h==D3DERR_NOTFOUND)&&ok;}
        HRESULT h=d->GetDepthStencilSurface(&ds);ok=(SUCCEEDED(h)||h==D3DERR_NOTFOUND)&&ok;
        if(!ok){for(auto*s:rt)if(s)s->Release();if(ds)ds->Release();d=nullptr;return false;}
        for(unsigned i=1;i<slots;++i)if(rt[i])ok=SUCCEEDED(d->SetRenderTarget(i,nullptr))&&ok;
        ok=SUCCEEDED(d->SetRenderTarget(0,colour))&&ok;ok=SUCCEEDED(d->SetDepthStencilSurface(target))&&ok;
        if(!ok)End();return ok;
    }
    bool Select(unsigned face){D3DVIEWPORT9 v={(face%3)*tile,(face/3)*tile,tile,tile,0,1};return d&&SUCCEEDED(d->SetViewport(&v));}
    bool End(){
        if(!d)return true;bool ok=true;
        for(unsigned i=0;i<slots;++i)if(i==0||rt[i])ok=SUCCEEDED(d->SetRenderTarget(i,rt[i]))&&ok;
        ok=SUCCEEDED(d->SetDepthStencilSurface(ds))&&ok;ok=SUCCEEDED(d->SetViewport(&vp))&&ok;
        for(auto*s:rt)if(s)s->Release();if(ds)ds->Release();d=nullptr;return ok;
    }
    ~Binding(){End();}
};
// Empty one slot's two rows of the static atlas and forget what it held.
static bool Rebuild(IDirect3DDevice9*d,unsigned s,unsigned long long&reason){
    StaticSlot&slot=slots[s];slot.Clear();slot.light=lights[s];slot.active=false;slot.dirty=true;
    Binding b;bool ok=b.Begin(d,staticDepth);
    if(ok){D3DVIEWPORT9 vp={0,0,tile*3,tile*4,0,1};ok=SUCCEEDED(d->SetViewport(&vp));
        D3DRECT rect={0,LONG(s*2*tile),LONG(tile*3),LONG((s*2+2)*tile)};
        ok=SUCCEEDED(d->Clear(1,&rect,D3DCLEAR_ZBUFFER,0,1,0))&&ok;}
    ok=b.End()&&ok;
    if(ok){slot.active=true;slot.dirty=false;++staticRebuilds;++reason;}
    return ok;
}
// Keep a light in the slot whose static layer already holds it, and reuse the
// exact position that layer was rendered with, so the face matrices match it
// bit for bit. The light table re-derives the position from camera-relative
// floats every frame, so otherwise it would drift by rounding noise.
static void AssignLights(){
    count=unsigned(pending.size());
    Light out[2];bool filled[2]={},used[2]={};
    if(StaticUsable())
        for(unsigned s=0;s<count;++s){
            if(!slots[s].active)continue;
            for(unsigned j=0;j<count;++j)if(!used[j]&&Same(pending[j],slots[s].light)){
                out[s]=slots[s].light;out[s].score=pending[j].score;filled[s]=used[j]=true;break;
            }
        }
    for(unsigned j=0;j<count;++j){
        if(used[j])continue;
        for(unsigned s=0;s<count;++s)if(!filled[s]){out[s]=pending[j];filled[s]=used[j]=true;break;}
    }
    for(unsigned s=0;s<count;++s)lights[s]=out[s];
}
static void PrepareStatic(IDirect3DDevice9*d){
    if(!StaticUsable()){
        for(auto&s:slots)s.active=false;
        worldsA.Clear();worldsB.Clear();
        staticStatus=!staticEnabled?"Static caster layer OFF":(!staticShaderReady?"Static caster layer needs the resolve with LocalStatic":"Static caster layer unavailable");
        return;
    }
    std::swap(frameWorlds,previousWorlds);frameWorlds->Clear();
    auto collect=[&](const WosShadowPackets::Pass&pass){for(const auto&i:pass.instances)frameWorlds->Emplace(&i.world,0);};
    collect(WosCascadeProbe::source);
    if(WosCascadeProbe::nearReady)collect(WosCascadeProbe::nearSource);
    for(unsigned s=0;s<count;++s){
        StaticSlot&slot=slots[s];
        bool ok=true;
        if(!slot.active||!Identical(slot.light,lights[s]))ok=Rebuild(d,s,rebuildLight);
        else if(slot.dirty)ok=Rebuild(d,s,rebuildUnseen);
        else{
            bool moved=false;
            for(const Matrix&w:slot.worldList)if(!frameWorlds->Find(&w)){moved=true;break;}
            if(moved)ok=Rebuild(d,s,rebuildMoved);
        }
        if(!ok){slot.active=false;++failures;}
    }
    staticStatus="Static caster layer ON";
}
static bool Prepare(IDirect3DDevice9*d){
    if(attempted)return valid;
    attempted=true;valid=false;count=0;if(!enabled||pending.empty())return false;
    WosCascadeShadow::HostScope host;
    unsigned request=resolution>=1024?1024:512;
    if(owner!=d||tile!=request||!atlas){
        // Retain pending light discovery across resource recreation.
        auto keep=pending;Reset();pending=keep;
        bool ok=SUCCEEDED(d->CreateTexture(request*3,request*4,1,D3DUSAGE_DEPTHSTENCIL,
            static_cast<D3DFORMAT>(MAKEFOURCC('I','N','T','Z')),D3DPOOL_DEFAULT,&atlas,nullptr));
        if(ok)ok=SUCCEEDED(atlas->GetSurfaceLevel(0,&depth));
        if(ok)ok=SUCCEEDED(d->CreateRenderTarget(request*3,request*4,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&colour,nullptr));
        if(!ok){Reset();++failures;status="Local shadow atlas allocation failed";return false;}
        owner=d;owner->AddRef();tile=request;
    }
    // The static atlas is optional: without it every caster is drawn live,
    // exactly as before it existed.
    if(staticEnabled&&staticShaderReady&&!staticAtlas){
        bool ok=SUCCEEDED(d->CreateTexture(tile*3,tile*4,1,D3DUSAGE_DEPTHSTENCIL,
            static_cast<D3DFORMAT>(MAKEFOURCC('I','N','T','Z')),D3DPOOL_DEFAULT,&staticAtlas,nullptr));
        if(ok)ok=SUCCEEDED(staticAtlas->GetSurfaceLevel(0,&staticDepth));
        if(!ok){ReleaseStatic();staticStatus="Static caster atlas allocation failed";}
    }
    AssignLights();
    for(unsigned i=0;i<count;++i)for(unsigned f=0;f<6;++f)matrices[i*6+f]=FaceMatrix(lights[i],f);
    WosShadowWork::local.Begin(matrices,count*6);
    Binding b;bool ok=b.Begin(d);
    if(ok){D3DVIEWPORT9 vp={0,0,tile*3,tile*4,0,1};ok=SUCCEEDED(d->SetViewport(&vp));
        ok=SUCCEEDED(d->Clear(0,nullptr,D3DCLEAR_ZBUFFER,0,1,0))&&ok;}
    ok=b.End()&&ok;valid=ok;if(!ok)++failures;
    if(ok)PrepareStatic(d);
    status=ok?"Two nearest influential local lights: six shadow faces each":"Local shadow atlas clear failed";
    return ok;
}
// Identity of everything that decides where a vs_0003 caster writes depth.
// Returns false for anything that cannot be proven unchanged between frames.
static bool MakeKey(IDirect3DDevice9*d,const DrawArgs&a,const Matrix&world,DrawKey&k){
    std::memset(&k,0,sizeof(k));uint32_t*w=k.w;unsigned i=0;
    std::memcpy(w,&world,64);i=16;
    for(UINT stream=0;stream<2;++stream){
        IDirect3DVertexBuffer9*vb=nullptr;UINT offset=0,stride=0,frequency=0;
        if(FAILED(d->GetStreamSource(stream,&vb,&offset,&stride))||FAILED(d->GetStreamSourceFreq(stream,&frequency))){if(vb)vb->Release();return false;}
        if(vb){D3DVERTEXBUFFER_DESC desc={};bool dynamic=FAILED(vb->GetDesc(&desc))||(desc.Usage&D3DUSAGE_DYNAMIC);vb->Release();if(dynamic)return false;}
        w[i++]=uint32_t(uintptr_t(vb));w[i++]=offset;w[i++]=stride;w[i++]=frequency;
    }
    IDirect3DIndexBuffer9*ib=nullptr;if(FAILED(d->GetIndices(&ib)))return false;
    if(ib){D3DINDEXBUFFER_DESC desc={};bool dynamic=FAILED(ib->GetDesc(&desc))||(desc.Usage&D3DUSAGE_DYNAMIC);ib->Release();if(dynamic)return false;}
    w[i++]=uint32_t(uintptr_t(ib));
    IDirect3DVertexDeclaration9*decl=nullptr;if(FAILED(d->GetVertexDeclaration(&decl)))return false;
    w[i++]=uint32_t(uintptr_t(decl));if(decl)decl->Release();
    IDirect3DPixelShader9*ps=nullptr;if(FAILED(d->GetPixelShader(&ps)))return false;
    w[i++]=uint32_t(uintptr_t(ps));if(ps)ps->Release();
    w[i++]=(a.indexed<<16)|a.type;w[i++]=a.base;w[i++]=a.minIndex;w[i++]=a.vertices;w[i++]=a.start;w[i++]=a.count;
    for(auto state:{D3DRS_CULLMODE,D3DRS_FILLMODE,D3DRS_ALPHATESTENABLE,D3DRS_ALPHAFUNC,D3DRS_ALPHAREF}){
        DWORD value=0;if(FAILED(d->GetRenderState(state,&value)))return false;w[i++]=value;
    }
    float decompress[8]={};if(FAILED(d->GetVertexShaderConstantF(7,decompress,2)))return false;
    std::memcpy(w+i,decompress,sizeof(decompress));i+=8;
    return i==kKeyWords;
}
// True when the slot's layer already holds this draw; marks it drawn this frame.
static bool Held(StaticSlot&slot,const DrawKey&key){
    const size_t*index=slot.keys.Find(&key);if(!index||*index>=slot.seen.size())return false;
    slot.seen[*index]=staticFrame;return true;
}
static void Remember(StaticSlot&slot,const DrawKey&key,const Matrix&world){
    if(slot.seen.size()>=kStaticKeyLimit||!slot.keys.Emplace(&key,slot.seen.size())){slot.dirty=true;++rebuildOverflow;return;}
    slot.seen.push_back(staticFrame);
    if(!slot.worlds.Find(&world)){
        if(!slot.worlds.Emplace(&world,0)){slot.dirty=true;++rebuildOverflow;return;}
        slot.worldList.push_back(world);
    }
}
static void Bind(IDirect3DDevice9*d,const double origin[3]){
    const bool layered=valid&&StaticUsable();
    float params[4]={valid&&enabled?float(count):0.f,float(tile),emitterRadius,strength};
    float tune[4]={bias,normalBias,.05f,layered?1.f:0.f};float positions[8]={};
    for(unsigned i=0;i<count;++i){for(int j=0;j<3;++j)positions[i*4+j]=float(lights[i].p[j]-origin[j]);positions[i*4+3]=1.f/lights[i].range;}
    bool ok=SUCCEEDED(d->SetPixelShaderConstantF(36,params,1))&&SUCCEEDED(d->SetPixelShaderConstantF(37,tune,1))&&SUCCEEDED(d->SetPixelShaderConstantF(38,positions,2));
    ok=SUCCEEDED(d->SetTexture(9,valid?atlas:nullptr))&&ok;
    ok=SUCCEEDED(d->SetTexture(10,layered?staticAtlas:nullptr))&&ok;
    for(DWORD sampler=9;sampler<=10;++sampler){
        for(auto s:{D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER})ok=SUCCEEDED(d->SetSamplerState(sampler,s,D3DTEXF_POINT))&&ok;
        ok=SUCCEEDED(d->SetSamplerState(sampler,D3DSAMP_MIPFILTER,D3DTEXF_NONE))&&ok;
        for(auto s:{D3DSAMP_ADDRESSU,D3DSAMP_ADDRESSV})ok=SUCCEEDED(d->SetSamplerState(sampler,s,D3DTADDRESS_CLAMP))&&ok;
        ok=SUCCEEDED(d->SetSamplerState(sampler,D3DSAMP_SRGBTEXTURE,0))&&ok;
    }
    if(!ok){params[0]=0;d->SetPixelShaderConstantF(36,params,1);++failures;}
}
}
