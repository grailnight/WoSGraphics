#pragma once
// Temporal anti-aliasing for the native D3D9 renderer.
//
// 1. Jitter. Main-view draws (16F scene target with its INTZ depth) are drawn
//    through a twin of the bound vertex shader that offsets clip position by a
//    Halton(2,3) sub-pixel step, see taa_vs_patch.h. Jitter only runs while the
//    resolve succeeded on the previous frame, so a frame the resolve cannot
//    handle (menus, loading, a missing camera) never shows a shaking image for
//    more than one frame.
// 2. Resolve. Right after the bloom composite, before the HUD, the display
//    image is copied, reprojected history is fetched with Catmull-Rom, clipped
//    to the YCoCg variance box of the current 3x3 neighbourhood and blended.
//    Motion comes from depth and the engine camera of both frames, taken at the
//    nearest depth of the 3x3 so silhouettes carry the foreground motion.
// 3. Present. The resolved 16F history is written back with optional sharpening
//    that cannot overshoot its own neighbourhood. Destination alpha is kept.
//
// Everything that touches the device goes through Host so the game build can
// bypass its own hooks and the GPU test can call the device directly.
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "taa_vs_patch.h"

namespace WosTaa {

struct Host {
    HRESULT (STDMETHODCALLTYPE* setVS)(IDirect3DDevice9*, IDirect3DVertexShader9*) = nullptr;
    HRESULT (STDMETHODCALLTYPE* setPS)(IDirect3DDevice9*, IDirect3DPixelShader9*) = nullptr;
    HRESULT (STDMETHODCALLTYPE* createVS)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**) = nullptr;
    HRESULT (STDMETHODCALLTYPE* drawUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT) = nullptr;
};
static Host host;

// ------------------------------------------------------------------ settings
static bool enabled = true, faulted = false;
static float feedbackMin = 0.80f, feedbackMax = 0.94f, clipGamma = 1.25f, sharpen = 0.20f, jitterScale = 1.0f;
static int debugView = 0; // 0 normal, 1 motion, 2 history weight
static const char* status = "TAA waiting for the first composite";
static std::string lastReject;
static unsigned long frames = 0, failures = 0, skipped = 0, historyResets = 0;
static unsigned long jitteredDraws = 0, unjitteredDraws = 0, lastJitteredDraws = 0, lastUnjitteredDraws = 0;
static unsigned long patchedShaders = 0, rejectedShaders = 0;

template<class T> static void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// --------------------------------------------------------------------- jitter
struct FrameJitter { bool active = false; float pixel[2] = {}, ndc[2] = {}; };
static FrameJitter frame;
static unsigned sequence = 0, frameNumber = 1, lastResolvedFrame = 0;
static bool attemptedThisFrame = false, resolvedThisFrame = false;
static UINT lastWidth = 0, lastHeight = 0;

inline float Halton(unsigned index, unsigned base) {
    float f = 1.0f, r = 0.0f;
    while (index) { f /= float(base); r += f * float(index % base); index /= base; }
    return r;
}
// Called once per presented frame. Chooses the jitter the next frame draws with.
static void FrameEnd() {
    const bool resolved = resolvedThisFrame;
    attemptedThisFrame = resolvedThisFrame = false;
    lastJitteredDraws = jitteredDraws; lastUnjitteredDraws = unjitteredDraws;
    jitteredDraws = unjitteredDraws = 0;
    ++frameNumber;
    sequence = (sequence + 1) % 8;
    frame = FrameJitter{};
    if (enabled && !faulted && resolved && jitterScale > 0.0f && lastWidth && lastHeight) {
        frame.active = true;
        frame.pixel[0] = (Halton(sequence + 1, 2) - 0.5f) * jitterScale;
        frame.pixel[1] = (Halton(sequence + 1, 3) - 0.5f) * jitterScale;
        // Pixel +x is clip +x, pixel +y (down) is clip -y.
        frame.ndc[0] = 2.0f * frame.pixel[0] / float(lastWidth);
        frame.ndc[1] = -2.0f * frame.pixel[1] / float(lastHeight);
    }
}

struct Twin { IDirect3DVertexShader9* shader = nullptr; UINT reg = 0; };
static std::unordered_map<IDirect3DVertexShader9*, Twin> twins;
static UINT maxVertexConstants = 0;

static Twin* TwinFor(IDirect3DDevice9* d, IDirect3DVertexShader9* vs) {
    const auto found = twins.find(vs);
    if (found != twins.end()) return found->second.shader ? &found->second : nullptr;
    if (!maxVertexConstants) {
        D3DCAPS9 caps = {};
        maxVertexConstants = SUCCEEDED(d->GetDeviceCaps(&caps)) ? (std::min)(caps.MaxVertexShaderConst, DWORD(256)) : 0;
    }
    Twin twin;
    std::vector<DWORD> code;
    UINT bytes = 0;
    if (SUCCEEDED(vs->GetFunction(nullptr, &bytes)) && bytes >= 8 && bytes % 4 == 0 && bytes < (1u << 20)) {
        code.resize(bytes / 4);
        if (FAILED(vs->GetFunction(code.data(), &bytes))) code.clear();
    }
    WosTaaPatch::Result patched;
    std::string why = "bytecode unavailable";
    if (!code.empty() && host.createVS && WosTaaPatch::Patch(code.data(), code.size(), maxVertexConstants, patched, why)) {
        if (SUCCEEDED(host.createVS(d, patched.code.data(), &twin.shader))) { twin.reg = patched.jitterRegister; ++patchedShaders; }
        else { twin.shader = nullptr; why = "patched shader rejected by the device"; }
    }
    if (!twin.shader) { ++rejectedShaders; lastReject = why; }
    // Keep the original alive: a freed pointer reused by a new shader would
    // otherwise inherit this twin.
    vs->AddRef();
    Twin& slot = twins[vs] = twin;
    return slot.shader ? &slot : nullptr;
}
static void ReleaseTwins() {
    for (auto& entry : twins) { Release(entry.second.shader); entry.first->Release(); }
    twins.clear();
}

// Decides at the native draw whether it is a main-view draw; the jittered twin
// is bound only around the innermost device call, so every other scope still
// sees the engine's own shader.
struct JitterScope {
    IDirect3DDevice9* d = nullptr;
    IDirect3DVertexShader9* original = nullptr;
    Twin* twin = nullptr;
    JitterScope(IDirect3DDevice9* device, bool eligible) {
        if (!eligible || !frame.active || !enabled || faulted) return;
        // Read through the self-verifying state shadow: the same values as the
        // real Gets, without a device call per draw.
        DWORD z = 0;
        if (!WosDeferredShadow::RenderState(device, D3DRS_ZENABLE, z) || !z) return;
        IDirect3DSurface9* rt = nullptr, * ds = nullptr;
        D3DSURFACE_DESC color = {}, depth = {};
        bool main = WosDeferredShadow::RenderTarget(device, 0, rt) && rt && WosDeferredShadow::TargetDesc(rt, color) &&
                    color.Format == D3DFMT_A16B16G16R16F && color.Width == lastWidth && color.Height == lastHeight;
        main = main && WosDeferredShadow::DepthStencil(device, ds) && ds && WosDeferredShadow::DepthDesc(ds, depth) &&
               depth.Format == D3DFORMAT(MAKEFOURCC('I', 'N', 'T', 'Z')) && depth.Width == color.Width && depth.Height == color.Height;
        Release(rt); Release(ds);
        if (!main) return;
        if (FAILED(device->GetVertexShader(&original)) || !original) { Release(original); ++unjitteredDraws; return; }
        twin = TwinFor(device, original);
        if (!twin) { ++unjitteredDraws; return; }
        d = device;
    }
    template<class F> HRESULT Draw(F&& draw) {
        if (!twin || !host.setVS) return draw();
        float saved[4] = {};
        const float jitter[4] = { frame.ndc[0], frame.ndc[1], 0.0f, 0.0f };
        if (FAILED(d->GetVertexShaderConstantF(twin->reg, saved, 1))) return draw();
        if (FAILED(d->SetVertexShaderConstantF(twin->reg, jitter, 1)) || FAILED(host.setVS(d, twin->shader))) {
            d->SetVertexShaderConstantF(twin->reg, saved, 1);
            host.setVS(d, original);
            return draw();
        }
        const HRESULT hr = draw();
        host.setVS(d, original);
        d->SetVertexShaderConstantF(twin->reg, saved, 1);
        ++jitteredDraws;
        return hr;
    }
    ~JitterScope() { Release(original); }
};

// -------------------------------------------------------------------- resolve
struct Resources {
    IDirect3DDevice9* owner = nullptr;
    UINT width = 0, height = 0; D3DFORMAT format = D3DFMT_UNKNOWN;
    IDirect3DTexture9* color = nullptr, * history[2] = {};
    IDirect3DSurface9* colorRT = nullptr, * historyRT[2] = {};
    IDirect3DPixelShader9* resolvePS = nullptr, * presentPS = nullptr;
    void Clear() {
        Release(colorRT); Release(color);
        for (int i = 0; i < 2; ++i) { Release(historyRT[i]); Release(history[i]); }
        Release(resolvePS); Release(presentPS);
        owner = nullptr; width = height = 0; format = D3DFMT_UNKNOWN;
    }
};
static Resources r;
static std::vector<unsigned char> resolveCode, presentCode;
static unsigned historyIndex = 0;
static bool historyValid = false;
static float prevRelativeInverse[16] = {};
static double prevOrigin[3] = {};

static bool LoadShaders(const std::string& folder) {
    auto read = [](const std::string& path) {
        std::vector<unsigned char> bytes;
        FILE* f = std::fopen(path.c_str(), "rb"); if (!f) return bytes;
        std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::rewind(f);
        if (n > 0 && n < (1 << 20) && n % 4 == 0) { bytes.resize(size_t(n)); if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear(); }
        std::fclose(f); return bytes;
    };
    resolveCode = read(folder + "\\taa_resolve.cso");
    presentCode = read(folder + "\\taa_present.cso");
    r.Clear();
    return !resolveCode.empty() && !presentCode.empty();
}

// Device lost: default-pool targets go, twins (managed) stay.
static void Reset() { r.Clear(); historyValid = false; frame = FrameJitter{}; attemptedThisFrame = resolvedThisFrame = false; faulted = false; }

static bool Ensure(IDirect3DDevice9* d, const D3DSURFACE_DESC& desc) {
    if (r.owner == d && r.width == desc.Width && r.height == desc.Height && r.format == desc.Format) return true;
    r.Clear(); historyValid = false;
    if (resolveCode.empty() || presentCode.empty()) return false;
    auto target = [&](D3DFORMAT f, IDirect3DTexture9*& t, IDirect3DSurface9*& s) {
        return SUCCEEDED(d->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, f, D3DPOOL_DEFAULT, &t, nullptr)) && SUCCEEDED(t->GetSurfaceLevel(0, &s));
    };
    bool ok = target(desc.Format, r.color, r.colorRT) &&
              target(D3DFMT_A16B16G16R16F, r.history[0], r.historyRT[0]) &&
              target(D3DFMT_A16B16G16R16F, r.history[1], r.historyRT[1]) &&
              SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(resolveCode.data()), &r.resolvePS)) &&
              SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(presentCode.data()), &r.presentPS));
    if (!ok) { r.Clear(); return false; }
    r.owner = d; r.width = desc.Width; r.height = desc.Height; r.format = desc.Format;
    return true;
}

// D3DSBT_ALL omits render targets and depth surfaces. Keep those explicitly.
struct State {
    IDirect3DDevice9* d; IDirect3DStateBlock9* block = nullptr;
    IDirect3DSurface9* rt[4] = {}, * depth = nullptr;
    D3DVIEWPORT9 vp = {}; bool valid = false;
    explicit State(IDirect3DDevice9* dev) : d(dev) {
        for (UINT i = 0; i < 4; ++i) { HRESULT hr = d->GetRenderTarget(i, &rt[i]); if (FAILED(hr) && (i == 0 || hr != D3DERR_NOTFOUND)) return; }
        HRESULT hr = d->GetDepthStencilSurface(&depth); if (FAILED(hr) && hr != D3DERR_NOTFOUND) return;
        if (FAILED(d->GetViewport(&vp))) return;
        valid = SUCCEEDED(d->CreateStateBlock(D3DSBT_ALL, &block)) && SUCCEEDED(block->Capture());
    }
    bool Restore() {
        bool ok = true; auto check = [&](HRESULT h) { ok = SUCCEEDED(h) && ok; };
        for (UINT i = 0; i < 16; ++i) check(d->SetTexture(i, nullptr));
        check(d->SetDepthStencilSurface(nullptr));
        for (UINT i = 1; i < 4; ++i) check(d->SetRenderTarget(i, nullptr));
        for (UINT i = 0; i < 4; ++i) if (rt[i]) check(d->SetRenderTarget(i, rt[i]));
        check(d->SetDepthStencilSurface(depth)); check(block->Apply()); check(d->SetViewport(&vp));
        return ok;
    }
    ~State() { Release(block); for (auto*& p : rt) Release(p); Release(depth); }
};

inline bool Inverse(const double m[16], double out[16]) {
    double a[4][8] = {};
    for (int i = 0; i < 4; ++i) { for (int j = 0; j < 4; ++j) a[i][j] = m[i * 4 + j]; a[i][4 + i] = 1; }
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int i = c + 1; i < 4; ++i) if (std::fabs(a[i][c]) > std::fabs(a[pivot][c])) pivot = i;
        if (!(std::fabs(a[pivot][c]) > 1e-300)) return false;
        if (pivot != c) for (int j = 0; j < 8; ++j) std::swap(a[c][j], a[pivot][j]);
        const double inv = 1.0 / a[c][c];
        for (int j = 0; j < 8; ++j) a[c][j] *= inv;
        for (int i = 0; i < 4; ++i) if (i != c) { const double f = a[i][c]; if (f != 0) for (int j = 0; j < 8; ++j) a[i][j] -= f * a[c][j]; }
    }
    for (int i = 0; i < 16; ++i) { out[i] = a[i / 4][4 + i % 4]; if (!std::isfinite(out[i])) return false; }
    return true;
}

// Constants: c0-c3 current camera-relative inverse VP rows, c4-c7 previous VP
// rows accepting current camera-relative positions, c8 metrics, c9 jitter and
// history flag, c10 blend, c11 present.
static bool Constants(const float relativeInverse[16], const double origin[3], bool history, float c[48]) {
    std::memset(c, 0, 48 * sizeof(float));
    std::memcpy(c, relativeInverse, 64);
    double delta[3];
    for (int i = 0; i < 3; ++i) delta[i] = origin[i] - prevOrigin[i];
    // A cut or a teleport: nothing to reproject.
    if (history && !(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2] < 32.0 * 32.0)) history = false;
    if (history) {
        double m[16], prev[16];
        for (int i = 0; i < 16; ++i) m[i] = prevRelativeInverse[i];
        history = Inverse(m, prev);
        for (int row = 0; row < 4 && history; ++row)
            for (int col = 0; col < 4; ++col) {
                double v = prev[row * 4 + col];
                if (col == 3) v += prev[row * 4] * delta[0] + prev[row * 4 + 1] * delta[1] + prev[row * 4 + 2] * delta[2];
                if (!std::isfinite(v)) { history = false; break; }
                c[16 + row * 4 + col] = float(v);
            }
    }
    c[32] = 1.0f / float(r.width); c[33] = 1.0f / float(r.height); c[34] = float(r.width); c[35] = float(r.height);
    c[36] = frame.active ? frame.ndc[0] : 0.0f; c[37] = frame.active ? frame.ndc[1] : 0.0f;
    c[38] = history ? 1.0f : 0.0f;
    c[40] = (std::min)(feedbackMin, feedbackMax); c[41] = feedbackMax; c[42] = clipGamma; c[43] = float(debugView);
    c[44] = sharpen;
    return true;
}

static bool Quad(IDirect3DDevice9* d) {
    struct V { float x, y, z, rhw, u, v; };
    const float w = float(r.width), h = float(r.height);
    const V q[] = { {-.5f,-.5f,0,1,0,0}, {w - .5f,-.5f,0,1,1,0}, {-.5f,h - .5f,0,1,0,1}, {w - .5f,h - .5f,0,1,1,1} };
    D3DVIEWPORT9 vp = { 0, 0, r.width, r.height, 0, 1 };
    return SUCCEEDED(d->SetViewport(&vp)) && SUCCEEDED(host.drawUP(d, D3DPT_TRIANGLESTRIP, 2, q, sizeof(V)));
}

// Runs right after the composite that writes the display image. depth is this
// frame's scene INTZ; relativeInverse/origin describe the engine camera it was
// drawn with (without jitter).
static bool Resolve(IDirect3DDevice9* d, IDirect3DTexture9* depth, const float relativeInverse[16], const double origin[3]) {
    if (!enabled || faulted || attemptedThisFrame || !host.setPS || !host.drawUP) return false;
    attemptedThisFrame = true;
    State state(d);
    D3DSURFACE_DESC desc = {}, depthDesc = {};
    if (!state.valid || !state.rt[0] || FAILED(state.rt[0]->GetDesc(&desc)) || !depth || FAILED(depth->GetLevelDesc(0, &depthDesc))) {
        ++skipped; historyValid = false; status = "TAA: device state or depth unavailable"; return false;
    }
    if (desc.MultiSampleType != D3DMULTISAMPLE_NONE || state.vp.X || state.vp.Y || state.vp.Width != desc.Width || state.vp.Height != desc.Height ||
        desc.Width < 64 || desc.Height < 64 || depthDesc.Width != desc.Width || depthDesc.Height != desc.Height ||
        (desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A16B16G16R16F)) {
        ++skipped; historyValid = false; status = "TAA: composite target unsupported; native image preserved"; return false;
    }
    if (!Ensure(d, desc)) { ++failures; faulted = true; status = "TAA resource creation failed"; return false; }
    const bool useHistory = historyValid && lastResolvedFrame + 1 == frameNumber;
    float c[48];
    Constants(relativeInverse, origin, useHistory, c);
    const bool usedHistory = c[38] > 0.5f;
    const unsigned previous = historyIndex, current = historyIndex ^ 1u;

    if (FAILED(d->StretchRect(state.rt[0], nullptr, r.colorRT, nullptr, D3DTEXF_NONE))) {
        ++failures; faulted = true; status = "TAA scene copy failed"; return false;
    }
    bool ok = true; auto check = [&](HRESULT h) { ok = SUCCEEDED(h) && ok; };
    for (UINT i = 0; i < 16; ++i) check(d->SetTexture(i, nullptr));
    check(d->SetDepthStencilSurface(nullptr));
    for (UINT i = 1; i < 4; ++i) check(d->SetRenderTarget(i, nullptr));
    if (host.setVS) check(host.setVS(d, nullptr));
    check(d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1));
    check(d->SetStreamSourceFreq(0, 1));
    const D3DRENDERSTATETYPE off[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SEPARATEALPHABLENDENABLE,
                                       D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE, D3DRS_LIGHTING, D3DRS_CLIPPLANEENABLE };
    for (auto s : off) check(d->SetRenderState(s, 0));
    check(d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE)); check(d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID));
    check(d->SetRenderState(D3DRS_COLORWRITEENABLE, 15)); check(d->SetRenderState(D3DRS_MULTISAMPLEMASK, 0xffffffff));
    check(d->SetRenderState(D3DRS_WRAP0, 0));
    for (UINT s = 0; s < 3; ++s) {
        const DWORD filter = s == 2 ? D3DTEXF_LINEAR : D3DTEXF_POINT; // history is filtered, the rest point-sampled
        check(d->SetSamplerState(s, D3DSAMP_MINFILTER, filter)); check(d->SetSamplerState(s, D3DSAMP_MAGFILTER, filter));
        check(d->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
        check(d->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP)); check(d->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
        check(d->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, 0)); check(d->SetSamplerState(s, D3DSAMP_MAXMIPLEVEL, 0));
        check(d->SetSamplerState(s, D3DSAMP_MIPMAPLODBIAS, 0));
    }
    check(d->SetPixelShaderConstantF(0, c, 12));
    // 1. Resolve into this frame's history.
    check(d->SetRenderTarget(0, r.historyRT[current]));
    check(d->SetTexture(0, r.color)); check(d->SetTexture(1, depth)); check(d->SetTexture(2, r.history[previous]));
    check(host.setPS(d, r.resolvePS));
    if (ok) ok = Quad(d);
    // 2. Write it back to the display image.
    check(d->SetTexture(2, nullptr));
    check(d->SetRenderTarget(0, state.rt[0]));
    check(d->SetTexture(0, r.history[current])); check(d->SetTexture(1, r.color));
    check(host.setPS(d, r.presentPS));
    if (ok) ok = Quad(d);
    const bool restored = state.Restore();
    if (!ok || !restored) {
        ++failures; historyValid = false;
        if (!restored) { faulted = true; status = "TAA restore failed; disabled for this session"; }
        else status = "TAA pass failed; frame left partially resolved";
        return false;
    }
    historyIndex = current; historyValid = true; lastResolvedFrame = frameNumber;
    std::memcpy(prevRelativeInverse, relativeInverse, sizeof(prevRelativeInverse));
    for (int i = 0; i < 3; ++i) prevOrigin[i] = origin[i];
    lastWidth = desc.Width; lastHeight = desc.Height;
    resolvedThisFrame = true; ++frames;
    if (!usedHistory) ++historyResets;
    status = frame.active ? "TAA ON (jittered, reprojected)" : "TAA ON (history warming up, jitter starts next frame)";
    return true;
}

} // namespace WosTaa
