#pragma once
// Shaderpack-style water for the native renderer.
//
// 1. Mask. Right after the engine draws its (v3-replaced) sm_water_mesh, the
//    same draw is replayed with ZFUNC EQUAL into a full-resolution mask through
//    water_mask.cso: rippled world normal and surface distance. The replay sits
//    inside the TAA jitter scope, so the vertex positions and depth match.
// 2. Reflections. At the native bloom composite, before god rays and SSGI read
//    the scene, water_ssr.cso marches reflected rays through the scene depth and
//    writes reflections and a sun glint back into the game's HDR texture.
//
// Include after native_indirect.h, deferred_runtime.h and taa_runtime.h. The
// hook sets Host once the device vtable is patched; native_water.h registers
// the replacement shaders it creates.
namespace WosWaterSSR {

struct Host {
    HRESULT (STDMETHODCALLTYPE* setPS)(IDirect3DDevice9*, IDirect3DPixelShader9*) = nullptr;
};
static Host host;

static bool enabled = true, faulted = false;
static float strength = 1.0f, rippleStrength = 0.6f, f0 = 0.02f, roughness = 0.08f, glint = 0.5f, maxDistance = 300.0f;
static int debugView = 0; // 0 normal, 1 ripple normal, 2 reflection alone, 3 confidence/hit/sky
static const char* status = "Water reflections waiting for water on screen";
static unsigned long frames = 0, failures = 0, skipped = 0, maskDraws = 0, lastMaskDraws = 0, replayFailures = 0;
static std::string directory;

template<class T> static void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// --------------------------------------------------------------- water shaders
// Kind 0 is sm_water_mesh, the only one the mask supports; the pond kinds are
// registered so the diagnostics can tell which water a scene actually draws.
static const char* kindNames[3] = { "sm_water_mesh", "sm_water_pond_mesh", "sm_water_pond_uv1" };
static IDirect3DPixelShader9* waterShaders[16] = {};
static int waterKinds[16] = {};
static unsigned waterShaderCount = 0;
// Held for the process lifetime: a freed address reused by another shader must
// never be mistaken for water.
static void RegisterWater(IDirect3DPixelShader9* shader, int kind = 0) {
    if (!shader || waterShaderCount >= 16 || kind < 0 || kind > 2) return;
    shader->AddRef();
    waterKinds[waterShaderCount] = kind;
    waterShaders[waterShaderCount++] = shader;
}
static int WaterKind(IDirect3DPixelShader9* shader) {
    if (!shader) return -1;
    for (unsigned i = 0; i < waterShaderCount; ++i) if (waterShaders[i] == shader) return waterKinds[i];
    return -1;
}
static bool IsWater(IDirect3DPixelShader9* shader) { return WaterKind(shader) == 0; }

// ---------------------------------------------------------------- diagnostics
// Cumulative counters, written once per second to WoS_Deferred\water_ssr_status.json.
struct Diagnostics {
    unsigned long seen[3] = {}, blended[3] = {}, noDepthWrite[3] = {}, gated = 0;
    unsigned long notMainTarget = 0, noCamera = 0, noResources = 0, stateQuery = 0, drawFailed = 0;
    unsigned long bloomCalls = 0, bloomIdle = 0, bloomTexture = 0, bloomCamera = 0, bloomScene = 0, bloomDepth = 0, bloomConstants = 0;
    unsigned long lastFrameSeen[3] = {};
};
static Diagnostics diag;
static ULONGLONG lastStatusWrite = 0;
static void WriteStatus() {
    ULONGLONG now = GetTickCount64();
    if (now - lastStatusWrite < 1000 || directory.empty()) return;
    lastStatusWrite = now;
    std::string path = directory.substr(0, directory.find_last_of("\\/")) + "\\water_ssr_status.json";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "{\"enabled\":%s,\"faulted\":%s,\"status\":\"%s\",\"frames\":%lu,\"mask_draws\":%lu,\"registered_shaders\":%u,\"kinds\":[",
                 enabled ? "true" : "false", faulted ? "true" : "false", status, frames, maskDraws, waterShaderCount);
    for (int k = 0; k < 3; ++k)
        std::fprintf(f, "%s{\"name\":\"%s\",\"draws\":%lu,\"blended\":%lu,\"no_depth_write\":%lu}", k ? "," : "", kindNames[k], diag.seen[k], diag.blended[k], diag.noDepthWrite[k]);
    std::fprintf(f, "],\"replay\":{\"gated\":%lu,\"not_main_target\":%lu,\"no_camera\":%lu,\"no_resources\":%lu,\"state_query\":%lu,\"draw_failed\":%lu},",
                 diag.gated, diag.notMainTarget, diag.noCamera, diag.noResources, diag.stateQuery, diag.drawFailed);
    std::fprintf(f, "\"bloom\":{\"calls\":%lu,\"idle_no_mask\":%lu,\"texture\":%lu,\"camera\":%lu,\"scene\":%lu,\"depth\":%lu,\"constants\":%lu,\"skipped\":%lu,\"failures\":%lu},\"sun_valid\":%s}\n",
                 diag.bloomCalls, diag.bloomIdle, diag.bloomTexture, diag.bloomCamera, diag.bloomScene, diag.bloomDepth, diag.bloomConstants, skipped, failures,
                 WosVolumetric::sunValid ? "true" : "false");
    std::fclose(f);
}

// ------------------------------------------------------------------ resources
static IDirect3DDevice9* owner = nullptr;
static IDirect3DTexture9* mask = nullptr, * copy = nullptr;
static IDirect3DSurface9* maskRT = nullptr, * copyRT = nullptr;
static IDirect3DPixelShader9* maskPS = nullptr, * ssrPS = nullptr;
static UINT width = 0, height = 0;
static bool shadersTried = false;
// What the mask currently describes.
static int maskFrame = -1, renderedFrame = -1;
static IUnknown* maskDepth = nullptr;
static uint32_t maskScene = 0;

static void ReleaseTargets() {
    Release(maskRT); Release(mask); Release(copyRT); Release(copy);
    Release(maskDepth); maskFrame = -1; maskScene = 0; width = height = 0;
}
static void Reset() {
    ReleaseTargets();
    Release(maskPS); Release(ssrPS);
    owner = nullptr; shadersTried = false; faulted = false;
}

static bool Shaders(IDirect3DDevice9* d) {
    if (owner && owner != d) Reset();
    if (maskPS && ssrPS) return true;
    if (shadersTried) return false;
    shadersTried = true;
    auto create = [&](const char* name, IDirect3DPixelShader9*& out) {
        auto bytes = WosCascadeProbe::ReadFile(directory + "\\" + name);
        if (bytes.size() < 8 || bytes.size() % 4) return false;
        uint32_t first = 0; std::memcpy(&first, bytes.data(), 4);
        return first == 0xffff0300u && SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(bytes.data()), &out));
    };
    if (!create("water_mask.cso", maskPS) || !create("water_ssr.cso", ssrPS)) {
        Release(maskPS); Release(ssrPS);
        faulted = true; status = "Water reflection shaders missing in WoS_Deferred\\water_ssr";
        return false;
    }
    owner = d;
    return true;
}
static bool Targets(IDirect3DDevice9* d, UINT w, UINT h) {
    if (mask && copy && width == w && height == h) return true;
    ReleaseTargets();
    auto target = [&](IDirect3DTexture9*& t, IDirect3DSurface9*& s) {
        return SUCCEEDED(d->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &t, nullptr)) &&
               SUCCEEDED(t->GetSurfaceLevel(0, &s));
    };
    if (!target(mask, maskRT) || !target(copy, copyRT)) {
        ReleaseTargets(); faulted = true; status = "Water reflection target allocation failed";
        return false;
    }
    width = w; height = h;
    return true;
}

// ----------------------------------------------------------------------- mask
// Replays one native water draw into the mask. Every state it touches is read
// first and written back; a failure only disables the mask for this frame.
template<class F> static void Replay(IDirect3DDevice9* d, F&& draw) {
    IDirect3DSurface9* rt[4] = {}, * ds = nullptr;
    IDirect3DTexture9* depthTexture = nullptr;
    IUnknown* identity = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    D3DSURFACE_DESC color = {}, depth = {};
    D3DVIEWPORT9 viewport = {};
    RECT scissor = {};
    DWORD zwrite = 0, zfunc = 0, blend = 0, write[4] = {}, srgb = 0;
    WosIndirectCamera::Camera camera;
    const uint32_t scene = WosIndirectCamera::CurrentScene();
    bool main = scene && SUCCEEDED(d->GetRenderTarget(0, &rt[0])) && rt[0] && SUCCEEDED(rt[0]->GetDesc(&color)) &&
                color.Format == D3DFMT_A16B16G16R16F && color.MultiSampleType == D3DMULTISAMPLE_NONE &&
                color.Width >= 640 && color.Height >= 200 && color.Width <= 4096 && color.Height <= 4096 &&
                SUCCEEDED(d->GetDepthStencilSurface(&ds)) && ds && SUCCEEDED(ds->GetDesc(&depth)) &&
                depth.Format == D3DFORMAT(MAKEFOURCC('I', 'N', 'T', 'Z')) && depth.Width == color.Width && depth.Height == color.Height &&
                SUCCEEDED(d->GetViewport(&viewport)) && !viewport.X && !viewport.Y && viewport.Width == color.Width && viewport.Height == color.Height &&
                SUCCEEDED(ds->GetContainer(IID_IDirect3DTexture9, reinterpret_cast<void**>(&depthTexture))) &&
                SUCCEEDED(depthTexture->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&identity)));
    if (!main) ++diag.notMainTarget;
    const bool matched = main && WosIndirectCamera::Find(depthTexture, color.Width, color.Height, camera, scene);
    if (main && !matched) ++diag.noCamera;
    bool ok = matched && Shaders(d) && Targets(d, color.Width, color.Height);
    if (matched && !ok) ++diag.noResources;
    bool resources = ok;
    for (unsigned i = 1; i < 4 && ok; ++i) { HRESULT hr = d->GetRenderTarget(i, &rt[i]); if (FAILED(hr) && hr != D3DERR_NOTFOUND) ok = false; }
    ok = ok && SUCCEEDED(d->GetPixelShader(&ps)) && SUCCEEDED(d->GetScissorRect(&scissor)) &&
         SUCCEEDED(d->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite)) && SUCCEEDED(d->GetRenderState(D3DRS_ZFUNC, &zfunc)) &&
         SUCCEEDED(d->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend)) && SUCCEEDED(d->GetRenderState(D3DRS_SRGBWRITEENABLE, &srgb)) &&
         SUCCEEDED(d->GetRenderState(D3DRS_COLORWRITEENABLE, &write[0])) && SUCCEEDED(d->GetRenderState(D3DRS_COLORWRITEENABLE1, &write[1])) &&
         SUCCEEDED(d->GetRenderState(D3DRS_COLORWRITEENABLE2, &write[2])) && SUCCEEDED(d->GetRenderState(D3DRS_COLORWRITEENABLE3, &write[3]));
    if (resources && !ok) ++diag.stateQuery;
    if (ok) {
        bool changed = true;
        auto check = [&](HRESULT h) { changed = SUCCEEDED(h) && changed; };
        for (unsigned i = 1; i < 4; ++i) check(d->SetRenderTarget(i, nullptr));
        check(d->SetRenderTarget(0, maskRT));
        // A new frame, scene or depth surface starts a fresh mask.
        if (maskFrame != g_frame_index || maskScene != scene || maskDepth != identity) {
            check(d->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0));
            Release(maskDepth); maskDepth = identity; maskDepth->AddRef();
            maskFrame = g_frame_index; maskScene = scene; lastMaskDraws = 0;
        }
        check(d->SetViewport(&viewport));
        check(d->SetScissorRect(&scissor));
        check(d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE));
        check(d->SetRenderState(D3DRS_ZFUNC, D3DCMP_EQUAL));
        check(d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
        check(d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE));
        check(d->SetRenderState(D3DRS_COLORWRITEENABLE, 15));
        check(host.setPS(d, maskPS));
        if (changed) changed = SUCCEEDED(draw());
        // Restore in reverse; RT0 first resets viewport and scissor, so those follow.
        bool restored = SUCCEEDED(host.setPS(d, ps));
        restored = SUCCEEDED(d->SetRenderTarget(0, rt[0])) && restored;
        for (unsigned i = 1; i < 4; ++i) restored = SUCCEEDED(d->SetRenderTarget(i, rt[i])) && restored;
        restored = SUCCEEDED(d->SetViewport(&viewport)) && restored;
        restored = SUCCEEDED(d->SetScissorRect(&scissor)) && restored;
        restored = SUCCEEDED(d->SetRenderState(D3DRS_ZWRITEENABLE, zwrite)) && restored;
        restored = SUCCEEDED(d->SetRenderState(D3DRS_ZFUNC, zfunc)) && restored;
        restored = SUCCEEDED(d->SetRenderState(D3DRS_ALPHABLENDENABLE, blend)) && restored;
        restored = SUCCEEDED(d->SetRenderState(D3DRS_SRGBWRITEENABLE, srgb)) && restored;
        restored = SUCCEEDED(d->SetRenderState(D3DRS_COLORWRITEENABLE, write[0])) && restored;
        if (!restored) { faulted = true; status = "Water mask state restore failed; reflections disabled for this session"; }
        if (changed) { ++maskDraws; ++lastMaskDraws; }
        else { ++replayFailures; ++diag.drawFailed; maskFrame = -1; }
    }
    for (auto*& p : rt) Release(p);
    Release(ds); Release(depthTexture); Release(identity); Release(ps);
}
// Wraps the innermost native call. The engine's own draw always runs first and
// its result is what the game receives.
template<class F> static HRESULT Draw(IDirect3DDevice9* d, F&& draw) {
    const HRESULT hr = draw();
    const int kind = WaterKind(g_current_shader);
    if (kind < 0 || FAILED(hr)) return hr;
    ++diag.seen[kind];
    DWORD value = 0;
    if (SUCCEEDED(d->GetRenderState(D3DRS_ALPHABLENDENABLE, &value)) && value) ++diag.blended[kind];
    if (SUCCEEDED(d->GetRenderState(D3DRS_ZWRITEENABLE, &value)) && !value) ++diag.noDepthWrite[kind];
    if (kind != 0) return hr;
    // No WosDeferred::busy gate: the primary deferred path raises it around the
    // native draw it adopts (water included). This wrapper only ever sees native
    // draws, and the water is identified by the engine's own shader.
    if (enabled && !faulted && host.setPS && !WosProbe::suppress && WosIndirectCamera::frameReady)
        Replay(d, draw);
    else ++diag.gated;
    return hr;
}

// ---------------------------------------------------------------- reflections
static bool Constants(const WosIndirectCamera::Camera& camera, float c[52]) {
    std::memset(c, 0, 52 * sizeof(float));
    std::memcpy(c, camera.relativeInverse, 64);
    WosShadowTransform::Matrix4 m; std::memcpy(m.v, camera.relativeInverse, 64);
    double vp[16];
    if (!WosCascades::Inverse(m, vp)) return false;
    for (unsigned i = 0; i < 16; ++i) { c[32 + i] = float(vp[i]); if (!std::isfinite(c[32 + i])) return false; }
    c[20] = 1.0f / float(width); c[21] = 1.0f / float(height); c[22] = maxDistance; c[23] = float(g_frame_index % 1024);
    c[24] = strength; c[25] = rippleStrength; c[26] = f0; c[27] = roughness;
    const bool sun = WosVolumetric::sunValid && glint > 0.0f;
    for (unsigned i = 0; i < 3; ++i) { c[28 + i] = sun ? WosVolumetric::sunDirection[i] : 0.0f; c[48 + i] = sun ? WosVolumetric::sunColour[i] : 0.0f; }
    c[31] = sun ? glint : 0.0f;
    c[51] = float(debugView);
    return true;
}
// At the native bloom composite. Texture 0 is the finished linear scene.
static void BloomDraw(IDirect3DDevice9* d, bool bloom) {
    if (!bloom || WosProbe::suppress || WosDeferred::busy || renderedFrame == g_frame_index) return;
    ++diag.bloomCalls;
    WriteStatus();
    if (!enabled || faulted) return;
    if (maskFrame != g_frame_index || !lastMaskDraws) { ++diag.bloomIdle; status = "Water reflections idle: no water drawn this frame"; return; }
    renderedFrame = g_frame_index;
    IDirect3DBaseTexture9* bound = nullptr;
    IDirect3DTexture9* hdr = nullptr, * depth = nullptr;
    IDirect3DSurface9* hdrRT = nullptr;
    IUnknown* identity = nullptr;
    D3DSURFACE_DESC desc = {};
    WosIndirectCamera::Camera camera;
    bool ok = SUCCEEDED(d->GetTexture(0, &bound)) && bound &&
              SUCCEEDED(bound->QueryInterface(IID_IDirect3DTexture9, reinterpret_cast<void**>(&hdr))) &&
              SUCCEEDED(hdr->GetLevelDesc(0, &desc)) && desc.Format == D3DFMT_A16B16G16R16F &&
              desc.Width == width && desc.Height == height;
    if (!ok) ++diag.bloomTexture;
    else if (!(ok = WosIndirectCamera::Acquire(desc.Width, desc.Height, camera, depth, WosIndirectCamera::CurrentScene()) && depth)) ++diag.bloomCamera;
    else if (!(ok = camera.scene == maskScene)) ++diag.bloomScene;
    else if (!(ok = SUCCEEDED(depth->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&identity))) && identity == maskDepth)) ++diag.bloomDepth;
    float c[52];
    if (ok && !Constants(camera, c)) { ok = false; ++diag.bloomConstants; }
    if (!ok || FAILED(hdr->GetSurfaceLevel(0, &hdrRT))) {
        ++skipped; status = "Water reflections skipped: scene, camera or depth does not match the mask";
    } else {
        WosIndirect::State state(d);
        bool drawn = state.valid && SUCCEEDED(d->StretchRect(hdrRT, nullptr, copyRT, nullptr, D3DTEXF_NONE));
        auto check = [&](HRESULT h) { drawn = SUCCEEDED(h) && drawn; };
        if (drawn) {
            check(d->SetDepthStencilSurface(nullptr));
            for (unsigned i = 1; i < state.slots; ++i) check(d->SetRenderTarget(i, nullptr));
            check(d->SetRenderTarget(0, hdrRT));
            check(d->SetVertexShader(nullptr)); check(d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1));
            const D3DRENDERSTATETYPE zeros[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE,
                                                 D3DRS_SCISSORTESTENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE, D3DRS_CLIPPLANEENABLE, D3DRS_SEPARATEALPHABLENDENABLE };
            for (auto s : zeros) check(d->SetRenderState(s, 0));
            check(d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE)); check(d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID));
            check(d->SetRenderState(D3DRS_COLORWRITEENABLE, 15)); check(d->SetRenderState(D3DRS_MULTISAMPLEMASK, 0xffffffff));
            for (DWORD i = 0; i < 3; ++i) {
                check(d->SetSamplerState(i, D3DSAMP_MINFILTER, D3DTEXF_POINT)); check(d->SetSamplerState(i, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
                check(d->SetSamplerState(i, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
                check(d->SetSamplerState(i, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP)); check(d->SetSamplerState(i, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
                check(d->SetSamplerState(i, D3DSAMP_SRGBTEXTURE, 0));
            }
            check(d->SetTexture(0, copy)); check(d->SetTexture(1, depth)); check(d->SetTexture(2, mask));
            check(d->SetPixelShaderConstantF(0, c, 13));
            check(host.setPS(d, ssrPS));
            if (drawn) drawn = WosIndirect::Quad(d, width, height);
            for (DWORD i = 0; i < 3; ++i) check(d->SetTexture(i, nullptr));
        }
        const bool restored = state.valid && state.Restore();
        if (!restored) { faulted = true; status = "Water reflection state restore failed; disabled for this session"; }
        else if (!drawn) { ++failures; status = "Water reflection pass failed; native water kept"; }
        else { ++frames; status = WosVolumetric::sunValid ? "Water reflections ON (SSR + sun glint)" : "Water reflections ON (SSR; sun glint needs deferred lighting)"; }
    }
    Release(hdrRT); Release(identity); Release(depth); Release(hdr); Release(bound);
}

} // namespace WosWaterSSR
