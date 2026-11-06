#pragma once
// Shadow copy of the device state the eligibility check reads on every draw.
//
// Every read goes through here. A known entry is returned without touching
// D3D; an unknown entry is fetched with the real Get and remembered. Entries
// become known only through the integration hooks on the setters, so a state
// that can change on a path this file does not see is never known: state
// block Apply, EndStateBlock and device Reset invalidate everything, and a
// recording state block suspends updates because recorded setters do not
// apply. RT0 changes also forget the viewport, which D3D9 resets with it.
//
// The shadow verifies itself: every 64th check compares each known entry with
// the real Get before it is used. One mismatch disables the shadow for the
// rest of the session and is counted in the status file. Disabled or unknown
// means the real Get runs, so correctness never depends on the shadow.
//
// Nothing is remembered until the integration reports that every setter hook
// is installed: without the hooks a remembered value could go stale unseen,
// so an unhooked build (and every isolated test) reads the real device only.
#include <windows.h>
#include <d3d9.h>
#include <cstring>
#include <cstdio>
namespace WosDeferredShadow {
static const unsigned kStates=256;
static bool requested=true;         // [Cache] Eligibility as the user set it
static bool enabled=true;           // runtime state; cleared by a mismatch without touching requested
static bool recording=false;        // BeginStateBlock..EndStateBlock
static bool hooked=false;           // set by the integration once all setter hooks are patched
// Set by the Reset hook around the whole reset: module resets still call the
// setters on their way out and the device returns to its defaults inside
// Reset, so nothing seen meanwhile may be remembered.
static bool suspended=false;
static const char* status="Eligibility cache armed";
static DWORD renderState[kStates]={};
static unsigned char stateKnown[kStates]={};
static IDirect3DSurface9* target[4]={};
static bool targetKnown[4]={};
static D3DSURFACE_DESC targetDesc={};
static bool targetDescOk=false;
static IDirect3DSurface9* depth=nullptr;
static bool depthKnown=false;
static D3DSURFACE_DESC depthDesc={};
static bool depthDescOk=false;
static IDirect3DTexture9* depthTexture=nullptr;   // owned reference to the bound depth container
static D3DVIEWPORT9 viewport={};
static bool viewportKnown=false;
static IDirect3DPixelShader9* shader=nullptr;
static bool shaderKnown=false;
static unsigned hits=0,fills=0,lastHits=0,lastFills=0;
static unsigned verifications=0,mismatches=0,invalidations=0,checks=0,resets=0,resetFailures=0;
// The mismatch that disabled the shadow, in words, for the status file.
static char mismatchWhat[192]="";
static HRESULT lastResetResult=S_OK;
// HRESULT of every Reset attempt, oldest first, while it fits.
static char resetLog[96]="";
// Device vtable slots d3d9 had written its own functions back into and the
// integration patched again (RehookDevice); foreign = owned by neither.
static unsigned rehooks=0;
static char rehookSlots[64]="",foreignSlots[64]="";
// With verdict reuse on, the self-check reads back only the render states
// something has read; the others never decide anything.
static bool verifyReadOnly=false;
static unsigned char stateRead[kStates]={};

static void ReleaseHeld(){if(depthTexture){depthTexture->Release();depthTexture=nullptr;}}
static void Invalidate(){
    std::memset(stateKnown,0,sizeof(stateKnown));
    for(auto&k:targetKnown)k=false;targetDescOk=false;
    depthKnown=false;depthDescOk=false;viewportKnown=false;shaderKnown=false;
    ReleaseHeld();++invalidations;
}
static void Reset(){Invalidate();recording=false;}
// Around IDirect3DDevice9::Reset: learn nothing until the call is done, then
// start clean. The clear on entry also drops the depth container reference,
// which would make Reset fail. A failed Reset leaves the device lost (the
// secure desktop is still up): the game keeps drawing, but its Set calls do
// not stick, so tracking resumes only after a Reset that succeeded.
static void BeginDeviceReset(){suspended=true;Invalidate();}
static void EndDeviceReset(HRESULT hr){
    ++resets;lastResetResult=hr;
    const size_t used=std::strlen(resetLog);
    if(used+12<sizeof(resetLog))std::snprintf(resetLog+used,sizeof(resetLog)-used,"%s%08lX",used?" ":"",(unsigned long)hr);
    if(FAILED(hr)){++resetFailures;Invalidate();return;}
    suspended=false;Reset();
}
static void SetHooked(bool on){hooked=on;suspended=false;Invalidate();}   // a new device is not lost
static void Disable(const char*why){enabled=false;status=why;Invalidate();}
static void SetEnabled(bool on){
    requested=on;if(on==enabled)return;
    enabled=on;Invalidate();if(on)mismatchWhat[0]=0;status=on?"Eligibility cache armed":"Eligibility cache OFF";
}
static void FrameEnd(){lastHits=hits;lastFills=fills;hits=fills=0;}

// Setter hooks. Callers invoke these only after the real call succeeded.
static bool Tracking(){return enabled&&hooked&&!recording&&!suspended;}
static void OnSetRenderState(D3DRENDERSTATETYPE type,DWORD value){
    if(!Tracking()||unsigned(type)>=kStates)return;
    renderState[type]=value;stateKnown[type]=1;
}
static void OnSetRenderTarget(DWORD index,IDirect3DSurface9*surface){
    if(!Tracking()||index>=4)return;
    target[index]=surface;targetKnown[index]=true;
    if(index==0){viewportKnown=false;targetDescOk=surface&&SUCCEEDED(surface->GetDesc(&targetDesc));}
}
static void OnSetDepthStencil(IDirect3DSurface9*surface){
    if(!Tracking())return;
    ReleaseHeld();depth=surface;depthKnown=true;
    depthDescOk=surface&&SUCCEEDED(surface->GetDesc(&depthDesc));
    if(surface){IDirect3DTexture9*t=nullptr;if(SUCCEEDED(surface->GetContainer(IID_IDirect3DTexture9,reinterpret_cast<void**>(&t))))depthTexture=t;}
}
static void OnSetViewport(const D3DVIEWPORT9*value){
    if(!Tracking()||!value)return;
    viewport=*value;viewportKnown=true;
}
static void OnSetPixelShader(IDirect3DPixelShader9*value){
    if(!Tracking())return;
    shader=value;shaderKnown=true;
}
static void OnBeginStateBlock(){recording=true;}
static void OnEndStateBlock(){recording=false;Invalidate();}
static void OnApply(){Invalidate();}

// Reads. Each returns the same value the real Get would, filling the shadow
// on a miss. Surface and shader outputs carry a reference like the real Get.
static bool RenderState(IDirect3DDevice9*d,D3DRENDERSTATETYPE type,DWORD&out){
    if(unsigned(type)<kStates)stateRead[type]=1;
    if(enabled&&unsigned(type)<kStates&&stateKnown[type]){out=renderState[type];++hits;return true;}
    DWORD value=0;if(FAILED(d->GetRenderState(type,&value)))return false;
    out=value;if(Tracking()&&unsigned(type)<kStates){renderState[type]=value;stateKnown[type]=1;++fills;}
    return true;
}
static bool RenderTarget(IDirect3DDevice9*d,DWORD index,IDirect3DSurface9*&out){
    out=nullptr;if(index>=4)return false;
    if(enabled&&targetKnown[index]){out=target[index];if(out)out->AddRef();++hits;return true;}
    IDirect3DSurface9*surface=nullptr;HRESULT hr=d->GetRenderTarget(index,&surface);
    if(FAILED(hr)&&hr!=D3DERR_NOTFOUND)return false;
    out=surface;
    if(Tracking()){
        target[index]=surface;targetKnown[index]=true;++fills;
        if(index==0)targetDescOk=surface&&SUCCEEDED(surface->GetDesc(&targetDesc));
    }
    return true;
}
static bool TargetDesc(IDirect3DSurface9*surface,D3DSURFACE_DESC&out){
    if(enabled&&targetKnown[0]&&surface==target[0]&&targetDescOk){out=targetDesc;++hits;return true;}
    return surface&&SUCCEEDED(surface->GetDesc(&out));
}
static bool DepthStencil(IDirect3DDevice9*d,IDirect3DSurface9*&out){
    out=nullptr;
    if(enabled&&depthKnown){out=depth;if(out)out->AddRef();++hits;return true;}
    IDirect3DSurface9*surface=nullptr;HRESULT hr=d->GetDepthStencilSurface(&surface);
    if(FAILED(hr)&&hr!=D3DERR_NOTFOUND)return false;
    out=surface;
    if(Tracking()){
        ReleaseHeld();depth=surface;depthKnown=true;++fills;
        depthDescOk=surface&&SUCCEEDED(surface->GetDesc(&depthDesc));
        if(surface){IDirect3DTexture9*t=nullptr;if(SUCCEEDED(surface->GetContainer(IID_IDirect3DTexture9,reinterpret_cast<void**>(&t))))depthTexture=t;}
    }
    return true;
}
static bool DepthDesc(IDirect3DSurface9*surface,D3DSURFACE_DESC&out){
    if(enabled&&depthKnown&&surface==depth&&depthDescOk){out=depthDesc;++hits;return true;}
    return surface&&SUCCEEDED(surface->GetDesc(&out));
}
static bool DepthContainer(IDirect3DSurface9*surface,IDirect3DTexture9*&out){
    out=nullptr;
    if(enabled&&depthKnown&&surface==depth&&depthTexture){out=depthTexture;out->AddRef();++hits;return true;}
    return surface&&SUCCEEDED(surface->GetContainer(IID_IDirect3DTexture9,reinterpret_cast<void**>(&out)))&&out;
}
static bool Viewport(IDirect3DDevice9*d,D3DVIEWPORT9&out){
    if(enabled&&viewportKnown){out=viewport;++hits;return true;}
    D3DVIEWPORT9 value={};if(FAILED(d->GetViewport(&value)))return false;
    out=value;if(Tracking()){viewport=value;viewportKnown=true;++fills;}
    return true;
}
static bool PixelShader(IDirect3DDevice9*d,IDirect3DPixelShader9*&out){
    out=nullptr;
    if(enabled&&shaderKnown){out=shader;if(out)out->AddRef();++hits;return true;}
    IDirect3DPixelShader9*value=nullptr;if(FAILED(d->GetPixelShader(&value)))return false;
    out=value;if(Tracking()){shader=value;shaderKnown=true;++fills;}
    return true;
}

// Self-check: compare every known entry with the real device. Returns false
// and disables the shadow on the first mismatch.
static bool Verify(IDirect3DDevice9*d){
    if(!enabled)return true;
    ++verifications;bool ok=true;
    for(unsigned i=0;i<kStates&&ok;++i){
        if(!stateKnown[i]||(verifyReadOnly&&!stateRead[i]))continue;
        DWORD value=0;if(FAILED(d->GetRenderState(D3DRENDERSTATETYPE(i),&value)))continue;
        ok=value==renderState[i];
        if(!ok)std::snprintf(mismatchWhat,sizeof(mismatchWhat),"render state %u: shadow %lu, device %lu (after %u resets)",i,(unsigned long)renderState[i],(unsigned long)value,resets);
    }
    for(unsigned i=0;i<4&&ok;++i){
        if(!targetKnown[i])continue;
        IDirect3DSurface9*s=nullptr;HRESULT hr=d->GetRenderTarget(i,&s);
        if(FAILED(hr)&&hr!=D3DERR_NOTFOUND)continue;
        ok=s==target[i];if(s)s->Release();
        if(!ok)std::snprintf(mismatchWhat,sizeof(mismatchWhat),"render target %u: shadow %p, device %p (after %u resets)",i,(void*)target[i],(void*)s,resets);
    }
    if(ok&&depthKnown){
        IDirect3DSurface9*s=nullptr;HRESULT hr=d->GetDepthStencilSurface(&s);
        if(SUCCEEDED(hr)||hr==D3DERR_NOTFOUND){ok=s==depth;if(s)s->Release();
            if(!ok)std::snprintf(mismatchWhat,sizeof(mismatchWhat),"depth: shadow %p, device %p (after %u resets)",(void*)depth,(void*)s,resets);}
    }
    if(ok&&viewportKnown){
        D3DVIEWPORT9 v={};if(SUCCEEDED(d->GetViewport(&v)))ok=std::memcmp(&v,&viewport,sizeof(v))==0;
        if(!ok)std::snprintf(mismatchWhat,sizeof(mismatchWhat),"viewport: shadow %lu,%lu %lux%lu z %g-%g, device %lu,%lu %lux%lu z %g-%g (after %u resets)",
            viewport.X,viewport.Y,viewport.Width,viewport.Height,(double)viewport.MinZ,(double)viewport.MaxZ,
            v.X,v.Y,v.Width,v.Height,(double)v.MinZ,(double)v.MaxZ,resets);
    }
    if(ok&&shaderKnown){
        IDirect3DPixelShader9*p=nullptr;if(SUCCEEDED(d->GetPixelShader(&p))){ok=p==shader;if(p)p->Release();
            if(!ok)std::snprintf(mismatchWhat,sizeof(mismatchWhat),"pixel shader: shadow %p, device %p (after %u resets)",(void*)shader,(void*)p,resets);}
    }
    if(!ok){++mismatches;Disable("Eligibility cache disabled: shadow state mismatch");}
    return ok;
}
// Called once per eligibility check; runs the self-check on every 64th one.
static void Tick(IDirect3DDevice9*d){if(enabled&&((++checks&63)==0))Verify(d);}

// Verdict reuse (WosDeferred::DrawScope): everything the eligibility check
// reads here before the pixel shader, as plain values. Equal keys mean the
// check reads equal values, including which render states are still unknown:
// an unknown entry changes only through a setter, which makes it known, or
// through an invalidation, which the count records.
static const D3DRENDERSTATETYPE kVerdictStates[14]={D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ZFUNC,D3DRS_ALPHABLENDENABLE,
    D3DRS_SRCBLEND,D3DRS_DESTBLEND,D3DRS_BLENDOP,D3DRS_SEPARATEALPHABLENDENABLE,D3DRS_SRCBLENDALPHA,D3DRS_DESTBLENDALPHA,
    D3DRS_BLENDOPALPHA,D3DRS_COLORWRITEENABLE,D3DRS_SRGBWRITEENABLE,D3DRS_STENCILENABLE};
struct VerdictKey {
    DWORD state[14];DWORD stateKnownBits;
    IDirect3DSurface9*target[4];IDirect3DSurface9*depth;IDirect3DTexture9*depthTexture;
    D3DVIEWPORT9 viewport;
    DWORD targetFormat,targetWidth,targetHeight,targetSamples,depthFormat,depthWidth,depthHeight,depthSamples;
    unsigned invalidations;
};
// False unless the targets, depth, viewport and pixel shader are all known.
static bool Key(VerdictKey&k){
    std::memset(&k,0,sizeof(k));
    if(!enabled||!hooked||!depthKnown||!viewportKnown||!shaderKnown)return false;
    for(unsigned i=0;i<4;++i){if(!targetKnown[i])return false;k.target[i]=target[i];}
    for(unsigned i=0;i<14;++i){
        const unsigned t=unsigned(kVerdictStates[i]);stateRead[t]=1;
        if(stateKnown[t]){k.state[i]=renderState[t];k.stateKnownBits|=1u<<i;}
    }
    k.depth=depth;k.depthTexture=depthTexture;k.viewport=viewport;
    if(targetDescOk){k.targetFormat=DWORD(targetDesc.Format);k.targetWidth=targetDesc.Width;k.targetHeight=targetDesc.Height;k.targetSamples=DWORD(targetDesc.MultiSampleType);}
    if(depthDescOk){k.depthFormat=DWORD(depthDesc.Format);k.depthWidth=depthDesc.Width;k.depthHeight=depthDesc.Height;k.depthSamples=DWORD(depthDesc.MultiSampleType);}
    k.invalidations=invalidations;
    return true;
}
static bool SameKey(const VerdictKey&a,const VerdictKey&b){return std::memcmp(&a,&b,sizeof(a))==0;}
// Borrowed reads for pointer comparisons: no reference is taken, so a value is
// only good while it stays bound. False when an entry is unknown.
static bool PeekPixelShader(IDirect3DPixelShader9*&out){
    out=nullptr;if(!enabled||!shaderKnown)return false;
    out=shader;++hits;return true;
}
static bool PeekBindings(IDirect3DSurface9*(&rt)[4],IDirect3DSurface9*&ds,IDirect3DPixelShader9*&ps){
    if(!enabled||!depthKnown||!shaderKnown)return false;
    for(unsigned i=0;i<4;++i){if(!targetKnown[i])return false;rt[i]=target[i];}
    ds=depth;ps=shader;++hits;return true;
}

static void WriteJson(FILE*f){
    std::fprintf(f,"\"eligibility_cache\":{\"requested\":%s,\"enabled\":%s,\"status\":\"%s\",\"hits\":%u,\"fills\":%u,\"verifications\":%u,\"mismatches\":%u,\"invalidations\":%u,\"resets\":%u,\"reset_failures\":%u,\"suspended\":%s,\"last_reset_hr\":\"0x%08lX\",\"reset_log\":\"%s\",\"rehooks\":%u,\"rehook_slots\":\"%s\",\"foreign_slots\":\"%s\",\"mismatch_detail\":\"%s\"}",
        requested?"true":"false",enabled?"true":"false",status,lastHits,lastFills,verifications,mismatches,invalidations,resets,
        resetFailures,suspended?"true":"false",(unsigned long)lastResetResult,resetLog,rehooks,rehookSlots,foreignSlots,mismatchWhat);
}
}
