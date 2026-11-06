#pragma once
// Top-down height map for world-space sky visibility.
//
// The sky visibility pass integrates horizons from the depth buffer, so it
// cannot see a wall that is outside the frame: the far side of a street, the
// building behind the camera. A path tracer would. The cheapest structure that
// holds those occluders is the one open-world engines use for sky occlusion:
// the highest surface above each point of the ground, rendered orthographically
// from above around the camera. A ray that dips below it has hit something.
//
// It is filled by the same caster replay as the sun cascades (WosCascadeProbe,
// fifth target), so coverage is exactly what the native shadow queues submit.
// Every `interval` frames, or sooner when the camera leaves the middle of the
// map, is enough: sky occlusion changes slowly, and between renders the map is
// rebased to the moving camera from its absolute world placement.
//
// Nothing below the highest surface is known, so a receiver standing under an
// overhang (a bridge deck, an awning) is left to the screen-space estimate;
// the shader tests that and the consumer takes min() of the two.
#include <d3d9.h>
#include <cmath>
// WosShadowTransform::Matrix4 comes from the cascade probe, included first.
namespace WosSkyHeight {
static bool enabled=true;
static float extent=256.f;      // world units covered by the map, per side
static float rayLength=48.f;    // world units each visibility ray marches
static int interval=4;          // frames between renders while the camera stays central
static const unsigned kResolution=1024;
static const double kAbove=400.0,kRange=700.0; // map top above the eye, depth span

static bool* hostBusy=nullptr;
static bool* hostActive=nullptr;

static IDirect3DDevice9* device=nullptr;
static IDirect3DTexture9* texture=nullptr;
static IDirect3DSurface9* depth=nullptr;
static IDirect3DSurface9* color=nullptr;

struct Placement { double minX=0,minZ=0,top=0,extent=0; };
static Placement pending,current;
static bool pendingValid=false,currentValid=false;
static unsigned frame=0,lastRender=0,renders=0,frameRenders=0,lastRenders=0,binds=0,failures=0;
static const char* status="Sky height map: waiting for the first replay";

static void Release(){
    if(depth){depth->Release();depth=nullptr;}
    if(color){color->Release();color=nullptr;}
    if(texture){texture->Release();texture=nullptr;}
    if(device){device->Release();device=nullptr;}
    pendingValid=currentValid=false;
}
static void Reset(){Release();renders=frameRenders=lastRenders=binds=failures=0;}

static bool Create(IDirect3DDevice9* d){
    if(device==d&&texture)return true;
    bool previous=hostBusy?*hostBusy:false;if(hostBusy)*hostBusy=true;
    IDirect3DTexture9* t=nullptr;IDirect3DSurface9* s=nullptr;IDirect3DSurface9* c=nullptr;
    bool ok=SUCCEEDED(d->CreateTexture(kResolution,kResolution,1,D3DUSAGE_DEPTHSTENCIL,
        static_cast<D3DFORMAT>(MAKEFOURCC('I','N','T','Z')),D3DPOOL_DEFAULT,&t,nullptr));
    if(ok)ok=SUCCEEDED(t->GetSurfaceLevel(0,&s));
    if(ok)ok=SUCCEEDED(d->CreateRenderTarget(kResolution,kResolution,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&c,nullptr));
    if(hostBusy)*hostBusy=previous;
    if(!ok){if(c)c->Release();if(s)s->Release();if(t)t->Release();++failures;return false;}
    Release();device=d;d->AddRef();texture=t;depth=s;color=c;return true;
}

// Binds the map as the only target: color, depth and a full viewport. The
// caller restores what it saved; the probe's sink binding already does.
static bool Target(IDirect3DDevice9* d){
    D3DVIEWPORT9 vp={0,0,kResolution,kResolution,0,1};
    return SUCCEEDED(d->SetRenderTarget(0,color))&&SUCCEEDED(d->SetDepthStencilSurface(depth))
        &&SUCCEEDED(d->SetViewport(&vp));
}

// Engine row-vector convention, like the cascade plan: clip = [x y z 1] * M.
//   clip.x = 2 (x - minX) / E - 1, clip.y = 1 - 2 (z - minZ) / E,
//   clip.z = (top - y) / range, so the texture's u follows x, v follows z and
//   the nearest depth kept by the z test is the highest surface.
static void Matrix(const Placement& p,WosShadowTransform::Matrix4& m){
    const double e=p.extent;
    float v[16]={float(2/e),0,0,0,
                 0,0,float(-1/kRange),0,
                 0,float(-2/e),0,0,
                 float(-2*p.minX/e-1),float(1+2*p.minZ/e),float(p.top/kRange),1};
    for(unsigned i=0;i<16;++i)m.v[i]=v[i];
}

// Probe seam, called once per frame after the cascade atlas was prepared.
// Returns true when the map should be rendered this frame; it has then been
// cleared and `vp` holds the matrix to replay casters with.
static bool Prepare(IDirect3DDevice9* d,const double eye[3],WosShadowTransform::Matrix4& vp){
    if(!enabled||!hostActive||!*hostActive)return false;
    for(unsigned i=0;i<3;++i)if(!std::isfinite(eye[i]))return false;
    double e=extent<64.f?64.0:(extent>1024.f?1024.0:double(extent));
    bool due=!currentValid||current.extent!=e||frame-lastRender>=unsigned(interval<1?1:interval);
    if(!due){
        // Recentre early once the camera has used up an eighth of the map.
        double cx=current.minX+current.extent*.5,cz=current.minZ+current.extent*.5;
        due=std::fabs(eye[0]-cx)>e*.125||std::fabs(eye[2]-cz)>e*.125;
    }
    if(!due)return false;
    if(!Create(d))return false;
    // Snapped to whole texels so a static scene rasterises identically after
    // a recentre and the rays do not shimmer.
    const double texel=e/kResolution;
    Placement p;p.extent=e;
    p.minX=std::floor((eye[0]-e*.5)/texel)*texel;
    p.minZ=std::floor((eye[2]-e*.5)/texel)*texel;
    p.top=eye[1]+kAbove;
    IDirect3DSurface9 *savedColor=nullptr,*savedDepth=nullptr;D3DVIEWPORT9 savedVp={};
    bool ok=SUCCEEDED(d->GetViewport(&savedVp));
    HRESULT hc=d->GetRenderTarget(0,&savedColor),hd=d->GetDepthStencilSurface(&savedDepth);
    ok=ok&&SUCCEEDED(hc)&&(SUCCEEDED(hd)||hd==D3DERR_NOTFOUND);
    if(ok)ok=Target(d)&&SUCCEEDED(d->Clear(0,nullptr,D3DCLEAR_ZBUFFER,0,1.f,0));
    if(savedColor){ok=SUCCEEDED(d->SetRenderTarget(0,savedColor))&&ok;savedColor->Release();}
    ok=SUCCEEDED(d->SetDepthStencilSurface(savedDepth))&&ok;if(savedDepth)savedDepth->Release();
    ok=SUCCEEDED(d->SetViewport(&savedVp))&&ok;
    if(!ok){++failures;return false;}
    // The map is empty now; it describes the new placement only once a caster
    // has actually reached it.
    currentValid=false;pending=p;pendingValid=true;lastRender=frame;
    Matrix(p,vp);
    for(float x:vp.v)if(!std::isfinite(x)){pendingValid=false;return false;}
    return true;
}
static bool Select(IDirect3DDevice9* d){
    if(!texture||!Target(d))return false;
    if(pendingValid){current=pending;currentValid=true;pendingValid=false;++renders;}
    ++frameRenders;return true;
}

// Constants for the sky visibility pass, camera-relative like everything the
// deferred passes reconstruct:
//   c50 u = x * c50.x + c50.y, v = z * c50.z + c50.w
//   c51 surface height = c51.x - depth * c51.y, c51.z enabled, c51.w ray length
//   c52 x world units per texel
static bool Bind(IDirect3DDevice9* d,const double origin[3],unsigned sampler,float shiftA=0,float shiftB=0){
    float off[4]={0,0,0,0};
    // c52.yz carry the per-frame pattern shift even when the map is unused.
    float shift[4]={0,shiftA,shiftB,0};d->SetPixelShaderConstantF(52,shift,1);
    if(!enabled||!currentValid||!texture||d!=device){d->SetPixelShaderConstantF(51,off,1);d->SetTexture(sampler,nullptr);return false;}
    const double e=current.extent;
    float map[4]={float(1/e),float((origin[0]-current.minX)/e),float(1/e),float((origin[2]-current.minZ)/e)};
    float height[4]={float(current.top-origin[1]),float(kRange),1.f,rayLength};
    float texel[4]={float(e/kResolution),shiftA,shiftB,0};
    for(float x:map)if(!std::isfinite(x)){d->SetPixelShaderConstantF(51,off,1);return false;}
    bool ok=SUCCEEDED(d->SetPixelShaderConstantF(50,map,1))&&SUCCEEDED(d->SetPixelShaderConstantF(51,height,1))
          &&SUCCEEDED(d->SetPixelShaderConstantF(52,texel,1))&&SUCCEEDED(d->SetTexture(sampler,texture));
    const D3DSAMPLERSTATETYPE types[6]={D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER,D3DSAMP_MIPFILTER,D3DSAMP_ADDRESSU,D3DSAMP_ADDRESSV,D3DSAMP_SRGBTEXTURE};
    const DWORD values[6]={D3DTEXF_POINT,D3DTEXF_POINT,D3DTEXF_NONE,D3DTADDRESS_CLAMP,D3DTADDRESS_CLAMP,0};
    for(unsigned i=0;i<6;++i)ok=SUCCEEDED(d->SetSamplerState(sampler,types[i],values[i]))&&ok;
    if(!ok){++failures;d->SetPixelShaderConstantF(51,off,1);d->SetTexture(sampler,nullptr);return false;}
    ++binds;return true;
}
static void FrameEnd(){
    ++frame;lastRenders=frameRenders;frameRenders=0;
    if(!enabled)status="Sky height map OFF";
    else if(!texture)status="Sky height map: needs the cascade shadows (Cascades enabled)";
    else if(!currentValid)status="Sky height map: no casters reached it yet";
    else status="Sky height map active";
}
}
