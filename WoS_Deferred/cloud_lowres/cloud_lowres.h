#pragma once
// Low-resolution, temporally amortized cloud march for the bloom composite.
//
// Until this module the bloom shader marched the cloud layer for every screen
// pixel every frame (64 steps, 4 light taps per dense step). Here the layer is
// marched into a buffer at 1/2 or 1/4 of the screen, and only one pixel of each
// 2x2 block of that buffer per frame; the rest is rebuilt from the previous
// frame, reprojected. A frame marches 1/16 (half) or 1/64 (quarter) of the
// pixels it used to. The scheme is the one Proper Shaders uses for its
// Cloudworks port; the shaders are ours and march our own layer.
//
// Both passes run at the bloom draw, before it, on the constants the hook has
// already uploaded for the bloom shader, and every state they touch is put
// back by WosIndirect::State. The resolved buffer is then bound to s4 for the
// bloom draw itself, with marker 11 in c36.w; the bloom shader reads it there
// instead of marching. BloomScope puts s4 and c36 back after the draw. If
// anything fails, nothing is bound and the bloom shader marches per pixel, as
// before.
//
// Shaders: WoS_Deferred\cloud_lowres\cloud_march.cso and cloud_resolve.cso.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
namespace WosCloudLowRes {

// User settings, [CloudLowRes] in clouds_menu.ini.
static int divisor = 2;            // 1 = per pixel in the bloom shader, 2 = half, 4 = quarter
static float marchedBlend = 0.5f;  // history a pixel marched this frame keeps
static float spatialMix = 0.15f;   // how far a waiting pixel leaks toward the fresh upsample per frame
static float clampExpand = 0.5f;   // slack on the history range, in units of the fresh range

static std::string directory;      // asset folder, set by the host at load
static const char* status = "Clouds: per-pixel march";
static bool faulted = false;
static unsigned long framesRendered = 0, failures = 0, historyResets = 0, skipped = 0;
static unsigned screenWidth = 0, screenHeight = 0, bufferWidth = 0, bufferHeight = 0, marchWidth = 0, marchHeight = 0;

static IDirect3DDevice9* owner = nullptr;
static IDirect3DPixelShader9* marchShader = nullptr;
static IDirect3DPixelShader9* resolveShader = nullptr;
static bool shadersTried = false;
static IDirect3DTexture9* marchTexture = nullptr;
static IDirect3DSurface9* marchSurface = nullptr;
static IDirect3DTexture9* history[2] = {};
static IDirect3DSurface9* historySurface[2] = {};
static int current = 0;            // history[current] is this frame's resolved buffer
static int renderedFrame = -1;     // g_frame_index of the last march
static bool historyReady = false;
static unsigned long sequence = 0; // marches since the buffers were made
static float previousInverse[16] = {};
static double previousOrigin[3] = {};
static int previousFrame = -1000;
static int previousDivisor = 0;

template<class T> static void ReleasePointer(T*& p) { if (p) { p->Release(); p = nullptr; } }
static void ReleaseTargets() {
    ReleasePointer(marchSurface); ReleasePointer(marchTexture);
    for (int i = 0; i < 2; ++i) { ReleasePointer(historySurface[i]); ReleasePointer(history[i]); }
    screenWidth = screenHeight = bufferWidth = bufferHeight = marchWidth = marchHeight = 0;
    historyReady = false; sequence = 0;
}
static void Reset() {
    ReleaseTargets(); ReleasePointer(marchShader); ReleasePointer(resolveShader);
    owner = nullptr; shadersTried = false; faulted = false; renderedFrame = -1; previousFrame = -1000;
}

static bool Shaders(IDirect3DDevice9* d) {
    if (owner && owner != d) Reset();
    if (marchShader && resolveShader) return true;
    if (shadersTried) return false;
    shadersTried = true;
    WosVolumetric::HostScope busy;
    auto create = [&](const char* name, IDirect3DPixelShader9*& out) {
        auto bytes = WosCascadeProbe::ReadFile(directory + "\\" + name);
        uint32_t first = 0, last = 0;
        if (bytes.size() < 8 || bytes.size() % 4) return false;
        std::memcpy(&first, bytes.data(), 4); std::memcpy(&last, bytes.data() + bytes.size() - 4, 4);
        return first == 0xffff0300u && last == 0x0000ffffu &&
               SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(bytes.data()), &out));
    };
    if (!create("cloud_march.cso", marchShader) || !create("cloud_resolve.cso", resolveShader)) {
        ReleasePointer(marchShader); ReleasePointer(resolveShader);
        faulted = true; status = "Cloud shaders missing in WoS_Deferred\\cloud_lowres; per-pixel march";
        return false;
    }
    owner = d;
    return true;
}

static bool Targets(IDirect3DDevice9* d, unsigned w, unsigned h) {
    const unsigned bw = (w + unsigned(divisor) - 1) / unsigned(divisor), bh = (h + unsigned(divisor) - 1) / unsigned(divisor);
    if (screenWidth == w && screenHeight == h && bufferWidth == bw && bufferHeight == bh && marchSurface) return true;
    ReleaseTargets();
    WosVolumetric::HostScope busy;
    const unsigned mw = (bw + 1) / 2, mh = (bh + 1) / 2;
    bool ok = SUCCEEDED(d->CreateTexture(mw, mh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &marchTexture, nullptr)) &&
              SUCCEEDED(marchTexture->GetSurfaceLevel(0, &marchSurface));
    for (int i = 0; i < 2 && ok; ++i)
        ok = SUCCEEDED(d->CreateTexture(bw, bh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &history[i], nullptr)) &&
             SUCCEEDED(history[i]->GetSurfaceLevel(0, &historySurface[i]));
    if (!ok) { ReleaseTargets(); status = "Cloud buffer allocation failed; per-pixel march"; return false; }
    screenWidth = w; screenHeight = h; bufferWidth = bw; bufferHeight = bh; marchWidth = mw; marchHeight = mh;
    return true;
}

static bool Invert(const double m[16], double out[16]) {
    double a[4][8];
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? m[r * 4 + c] : double(c - 4 == r);
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r) if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (std::fabs(a[p][c]) < 1e-30) return false;
        for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[p][k]);
        const double s = a[c][c];
        for (int k = 0; k < 8; ++k) a[c][k] /= s;
        for (int r = 0; r < 4; ++r) if (r != c) { const double f = a[r][c]; for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k]; }
    }
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) { out[r * 4 + c] = a[r][c + 4]; if (!std::isfinite(out[r * 4 + c])) return false; }
    return true;
}

// Last frame's world->clip rows, rebased so that they take a position relative
// to this frame's camera. Done in double: the camera sits thousands of units
// from the origin and float would lose the frame-to-frame translation.
static bool PreviousViewProjection(const double origin[3], float out[16]) {
    double inverse[16], forward[16];
    for (int i = 0; i < 16; ++i) inverse[i] = previousInverse[i];
    if (!Invert(inverse, forward)) return false;
    for (int r = 0; r < 4; ++r) {
        double shift = forward[r * 4 + 3];
        for (int c = 0; c < 3; ++c) shift += forward[r * 4 + c] * origin[c];
        for (int c = 0; c < 3; ++c) out[r * 4 + c] = float(forward[r * 4 + c]);
        out[r * 4 + 3] = float(shift);
        for (int c = 0; c < 4; ++c) if (!std::isfinite(out[r * 4 + c])) return false;
    }
    return true;
}

static void WriteStatus();

// One march and resolve per frame. origin and inverse are the camera position
// and inverse viewProj the bloom shader receives in c22..c24.w and c32..c35.
static bool Render(IDirect3DDevice9* d, unsigned w, unsigned h, const float origin[3], const float inverse[16]) {
    if (!Shaders(d) || !Targets(d, w, h)) return false;

    const double cameraOrigin[3] = { origin[0], origin[1], origin[2] };
    double moved = 0;
    for (int i = 0; i < 3; ++i) moved += (cameraOrigin[i] - previousOrigin[i]) * (cameraOrigin[i] - previousOrigin[i]);
    // A cut, a load or a pause shows up as a jump or a gap; history from before
    // it would be the wrong sky.
    bool useHistory = historyReady && previousDivisor == divisor &&
                      g_frame_index - previousFrame > 0 && g_frame_index - previousFrame <= 8 && moved < 400.0 * 400.0;
    float previous[16] = {};
    if (useHistory && !PreviousViewProjection(cameraOrigin, previous)) useHistory = false;
    if (!useHistory && historyReady) ++historyResets;

    static const float slots[4][2] = { { 0, 0 }, { 1, 1 }, { 1, 0 }, { 0, 1 } };
    const float* slot = slots[sequence % 4];
    const float seed = float(std::fmod(0.6180339887 * double(sequence / 4), 1.0));
    const int target = useHistory ? 1 - current : current;

    WosVolumetric::HostScope busy;
    WosIndirect::State state(d);
    bool ok = state.valid;
    auto check = [&](HRESULT hr) { ok = SUCCEEDED(hr) && ok; };
    if (ok) {
        check(d->SetDepthStencilSurface(nullptr));
        for (unsigned i = 1; i < state.slots; ++i) check(d->SetRenderTarget(i, nullptr));
        check(d->SetVertexShader(nullptr)); check(d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1));
        check(d->SetRenderState(D3DRS_WRAP0, 0));
        for (auto rs : { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE,
                         D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE,
                         D3DRS_CLIPPLANEENABLE, D3DRS_SEPARATEALPHABLENDENABLE })
            check(d->SetRenderState(rs, 0));
        check(d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE)); check(d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID));
        check(d->SetRenderState(D3DRS_COLORWRITEENABLE, 15)); check(d->SetRenderState(D3DRS_MULTISAMPLEMASK, 0xffffffff));
        check(d->SetRenderState(D3DRS_DITHERENABLE, FALSE));

        // Pass 1: one pixel of every 2x2 block.
        const float grid[4] = { float(bufferWidth), float(bufferHeight), slot[0], slot[1] };
        const float march[4] = { seed, 0, 0, 0 };
        check(d->SetPixelShaderConstantF(36, grid, 1));
        check(d->SetPixelShaderConstantF(37, march, 1));
        check(d->SetRenderTarget(0, marchSurface));
        check(g_original_set_pixel_shader(d, marchShader));
        if (ok) ok = WosIndirect::Quad(d, marchWidth, marchHeight);

        // Pass 2: the whole buffer from the sparse march and last frame.
        const float resolve[4] = { marchedBlend, spatialMix, clampExpand, useHistory ? 1.0f : 0.0f };
        const float size[4] = { float(marchWidth), float(marchHeight), 1.0f / float(marchWidth), 1.0f / float(marchHeight) };
        check(d->SetPixelShaderConstantF(37, resolve, 1));
        check(d->SetPixelShaderConstantF(38, size, 1));
        check(d->SetPixelShaderConstantF(40, previous, 4));
        check(d->SetTexture(0, marchTexture));
        check(d->SetTexture(1, useHistory ? history[current] : nullptr));
        for (DWORD stage = 0; stage < 2; ++stage) {
            const DWORD filter = stage ? D3DTEXF_LINEAR : D3DTEXF_POINT;
            check(d->SetSamplerState(stage, D3DSAMP_MINFILTER, filter));
            check(d->SetSamplerState(stage, D3DSAMP_MAGFILTER, filter));
            check(d->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
            check(d->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
            check(d->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
            check(d->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, 0));
        }
        check(d->SetRenderTarget(0, historySurface[target]));
        check(g_original_set_pixel_shader(d, resolveShader));
        if (ok) ok = WosIndirect::Quad(d, bufferWidth, bufferHeight);
        check(d->SetTexture(0, nullptr)); check(d->SetTexture(1, nullptr));
    }
    const bool restored = state.valid && state.Restore();
    if (!restored) {
        faulted = true; historyReady = false;
        status = "Cloud pass state restore failed; per-pixel march for this session";
        return false;
    }
    if (!ok) { ++failures; historyReady = false; status = "Cloud pass failed this frame; per-pixel march"; return false; }

    current = target;
    historyReady = true;
    ++sequence; ++framesRendered;
    std::memcpy(previousInverse, inverse, sizeof(previousInverse));
    for (int i = 0; i < 3; ++i) previousOrigin[i] = cameraOrigin[i];
    previousFrame = g_frame_index;
    previousDivisor = divisor;
    status = divisor >= 4 ? "Clouds: 1/4 resolution, temporal" : "Clouds: 1/2 resolution, temporal";
    return true;
}

// Constructed at the bloom draw next to the other bloom passes; destroyed after
// the draw, which is when s4 and c36 go back to what the game had there.
struct BloomScope {
    IDirect3DDevice9* d = nullptr;
    IDirect3DBaseTexture9* savedTexture = nullptr;
    DWORD savedSampler[6] = {};
    float savedConstant[4] = {};
    bool bound = false;
    static constexpr D3DSAMPLERSTATETYPE kStates[6] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER,
                                                       D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE };

    // ready: the bloom constants hold this frame's camera and inverse viewProj.
    BloomScope(IDirect3DDevice9* device, bool bloom, bool ready, const float origin[3], const float inverse[16]) {
        if (!bloom || WosProbe::suppress || WosDeferred::busy) return;
        WriteStatus();
        if (divisor < 2 || faulted) { if (divisor < 2) status = "Clouds: per-pixel march"; return; }
        if (!ready) { ++skipped; status = "Clouds waiting for camera and depth; per-pixel march"; return; }
        D3DVIEWPORT9 viewport = {};
        if (FAILED(device->GetViewport(&viewport)) || viewport.Width < 320 || viewport.Height < 200 ||
            viewport.Width > 4096 || viewport.Height > 4096) { ++skipped; return; }
        // A second bloom draw in the same frame reuses the buffer instead of
        // advancing the history twice.
        if (renderedFrame != g_frame_index) {
            if (!Render(device, viewport.Width, viewport.Height, origin, inverse)) return;
            renderedFrame = g_frame_index;
        } else if (!historyReady || screenWidth != viewport.Width || screenHeight != viewport.Height) {
            return;
        }
        d = device;
        if (FAILED(d->GetTexture(4, &savedTexture))) savedTexture = nullptr;
        for (int i = 0; i < 6; ++i) d->GetSamplerState(4, kStates[i], &savedSampler[i]);
        d->GetPixelShaderConstantF(36, savedConstant, 1);
        const DWORD values[6] = { D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, 0 };
        bool ok = SUCCEEDED(d->SetTexture(4, history[current]));
        for (int i = 0; i < 6; ++i) ok = SUCCEEDED(d->SetSamplerState(4, kStates[i], values[i])) && ok;
        const float info[4] = { 1.0f / float(bufferWidth), 1.0f / float(bufferHeight), float(divisor), ok ? 11.0f : 0.0f };
        ok = SUCCEEDED(d->SetPixelShaderConstantF(36, info, 1)) && ok;
        bound = true;
        if (!ok) { ++failures; status = "Cloud buffer could not be bound; per-pixel march"; }
    }
    ~BloomScope() {
        if (!bound) return;
        d->SetTexture(4, savedTexture);
        for (int i = 0; i < 6; ++i) d->SetSamplerState(4, kStates[i], savedSampler[i]);
        d->SetPixelShaderConstantF(36, savedConstant, 1);
        if (savedTexture) savedTexture->Release();
    }
    BloomScope(const BloomScope&) = delete;
    BloomScope& operator=(const BloomScope&) = delete;
};

// Once a second, for checking the mod without a screenshot.
static ULONGLONG lastStatusWrite = 0;
static void WriteStatus() {
    const ULONGLONG tick = GetTickCount64();
    if (tick - lastStatusWrite < 1000 || directory.empty()) return;
    lastStatusWrite = tick;
    FILE* f = std::fopen((directory + "\\cloud_lowres_status.json").c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "{\"divisor\":%d,\"faulted\":%s,\"status\":\"%s\",\"frames_rendered\":%lu,\"failures\":%lu,\"history_resets\":%lu,"
                    "\"skipped\":%lu,\"screen\":[%u,%u],\"buffer\":[%u,%u],\"march\":[%u,%u],\"marched_blend\":%.3f,"
                    "\"spatial_mix\":%.3f,\"clamp_expand\":%.3f}\n",
                 divisor, faulted ? "true" : "false", status, framesRendered, failures, historyResets, skipped,
                 screenWidth, screenHeight, bufferWidth, bufferHeight, marchWidth, marchHeight, marchedBlend, spatialMix, clampExpand);
    std::fclose(f);
}

} // namespace WosCloudLowRes
