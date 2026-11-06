// Offline HAL check of the low-resolution cloud march.
//
// Includes the real cloud_lowres.h (with the DLL's host functions stubbed) and
// drives it exactly as the bloom hook does: upload the bloom constants, open a
// BloomScope, draw the bloom's cloud lines (composite_test.hlsl), close the
// scope. The result is compared with the per-pixel march the bloom used before
// (64 steps) and with a 256-step per-pixel ground truth, on sky pixels,
// geometry in front of the layer and geometry inside it.
//
// Scenes: a static camera, a fast pan, and the quarter-resolution buffer.
// Then GPU time of the old per-pixel march against march + resolve + composite.
// Writes gpu_test.json and, per scene, <scene>_{new,old,gt}.raw (float RGB,
// the clouds over a flat sky) for make_png.py.
#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "user32.lib")

// --- host stubs for cloud_lowres.h ------------------------------------------
static int g_frame_index = 0;
static HRESULT STDMETHODCALLTYPE OriginalSetPixelShader(IDirect3DDevice9* d, IDirect3DPixelShader9* p) { return d->SetPixelShader(p); }
static HRESULT STDMETHODCALLTYPE OriginalDrawPrimitiveUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT n, const void* v, UINT s) { return d->DrawPrimitiveUP(t, n, v, s); }
static HRESULT (STDMETHODCALLTYPE* g_original_set_pixel_shader)(IDirect3DDevice9*, IDirect3DPixelShader9*) = OriginalSetPixelShader;
static HRESULT (STDMETHODCALLTYPE* g_original_draw_primitive_up)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT) = OriginalDrawPrimitiveUP;
namespace WosProbe { static bool suppress = false; }
namespace WosDeferred { static bool busy = false; }
namespace WosVolumetric { struct HostScope {}; }
namespace WosCascadeProbe {
static std::vector<unsigned char> ReadFile(const std::string& path) {
    std::vector<unsigned char> v; FILE* f = std::fopen(path.c_str(), "rb"); if (!f) return v;
    std::fseek(f, 0, SEEK_END); v.resize(size_t(std::ftell(f))); std::fseek(f, 0, SEEK_SET);
    if (std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear(); std::fclose(f); return v;
}
}
// Copied from native_indirect.h, which the DLL uses.
namespace WosIndirect {
struct State {
    IDirect3DDevice9* d; IDirect3DStateBlock9* block = nullptr; IDirect3DSurface9* rt[4] = {}, * ds = nullptr; IDirect3DPixelShader9* ps = nullptr; D3DVIEWPORT9 vp = {}; unsigned slots = 0; bool valid = false;
    explicit State(IDirect3DDevice9* device) : d(device) {
        D3DCAPS9 caps = {}; if (FAILED(d->GetDeviceCaps(&caps)) || FAILED(d->GetViewport(&vp)) || FAILED(d->CreateStateBlock(D3DSBT_ALL, &block)) || FAILED(block->Capture())) return;
        slots = (std::min)(caps.NumSimultaneousRTs, DWORD(4));
        for (unsigned i = 0; i < slots; ++i) { HRESULT h = d->GetRenderTarget(i, &rt[i]); if (FAILED(h) && h != D3DERR_NOTFOUND) return; }
        HRESULT h = d->GetDepthStencilSurface(&ds); if (FAILED(h) && h != D3DERR_NOTFOUND) return;
        if (FAILED(d->GetPixelShader(&ps))) return; valid = true;
    }
    bool Restore() {
        if (!valid) return false; bool ok = SUCCEEDED(d->SetDepthStencilSurface(nullptr));
        for (unsigned i = 0; i < slots; ++i) ok = SUCCEEDED(d->SetRenderTarget(i, rt[i])) && ok;
        ok = SUCCEEDED(d->SetDepthStencilSurface(ds)) && ok; ok = SUCCEEDED(block->Apply()) && ok; ok = SUCCEEDED(g_original_set_pixel_shader(d, ps)) && ok; ok = SUCCEEDED(d->SetViewport(&vp)) && ok; valid = false; return ok;
    }
    ~State() { if (valid) Restore(); for (auto* r : rt) if (r) r->Release(); if (ds) ds->Release(); if (ps) ps->Release(); if (block) block->Release(); }
};
static bool Quad(IDirect3DDevice9* d, unsigned w, unsigned h) {
    struct V { float x, y, z, rhw, u, v; }; const V q[] = { { -.5f, -.5f, 0, 1, 0, 0 }, { float(w) - .5f, -.5f, 0, 1, 1, 0 }, { -.5f, float(h) - .5f, 0, 1, 0, 1 }, { float(w) - .5f, float(h) - .5f, 0, 1, 1, 1 } };
    D3DVIEWPORT9 vp = { 0, 0, w, h, 0, 1 }; return SUCCEEDED(d->SetViewport(&vp)) && SUCCEEDED(g_original_draw_primitive_up(d, D3DPT_TRIANGLESTRIP, 2, q, sizeof(V)));
}
}
#include "../cloud_lowres.h"

// --- scene ------------------------------------------------------------------
static const UINT W = 1920, H = 1080;
static const double kNear = 0.5, kFar = 30000.0, kFovY = 60.0 * 3.14159265358979 / 180.0;
static const float kSkyDepth = 1.0f;

struct Camera { double pos[3], right[3], up[3], fwd[3]; };
static Camera MakeCamera(double x, double y, double z, double yawDeg, double pitchDeg) {
    const double yaw = yawDeg * 3.14159265358979 / 180.0, pitch = pitchDeg * 3.14159265358979 / 180.0;
    Camera c = { { x, y, z } };
    const double f[3] = { std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch) };
    const double r[3] = { std::cos(yaw), 0, -std::sin(yaw) };
    // Left-handed, y up: up = forward x right.
    const double u[3] = { f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0] };
    for (int i = 0; i < 3; ++i) { c.fwd[i] = f[i]; c.right[i] = r[i]; c.up[i] = u[i]; }
    return c;
}
static void ViewProj(const Camera& c, double m[16]) {
    const double ys = 1.0 / std::tan(kFovY / 2), xs = ys / (double(W) / H);
    const double* rows[3] = { c.right, c.up, c.fwd };
    const double scale[3] = { xs, ys, kFar / (kFar - kNear) };
    for (int r = 0; r < 3; ++r) {
        double t = 0;
        for (int k = 0; k < 3; ++k) { m[r * 4 + k] = scale[r] * rows[r][k]; t += rows[r][k] * c.pos[k]; }
        m[r * 4 + 3] = -scale[r] * t;
    }
    m[2 * 4 + 3] += -kNear * kFar / (kFar - kNear);
    double t = 0; for (int k = 0; k < 3; ++k) { m[12 + k] = c.fwd[k]; t += c.fwd[k] * c.pos[k]; }
    m[15] = -t;
}
static float RawDepth(double viewZ) { return float(kFar / (kFar - kNear) - kNear * kFar / ((kFar - kNear) * viewZ)); }

static std::vector<unsigned char> ReadAll(const char* p) { return WosCascadeProbe::ReadFile(p); }

struct Device {
    IDirect3DDevice9* d = nullptr;
    IDirect3DTexture9* depth = nullptr; IDirect3DSurface9* depthS = nullptr;
    IDirect3DTexture9* out = nullptr; IDirect3DSurface9* outS = nullptr; IDirect3DSurface9* readback = nullptr;
    IDirect3DPixelShader9* composite = nullptr, * truth = nullptr;
};

static void UploadBloomConstants(IDirect3DDevice9* d, const Camera& c, float invOut[16], float posOut[3]) {
    double m[16], inv[16]; ViewProj(c, m); WosCloudLowRes::Invert(m, inv);
    for (int i = 0; i < 16; ++i) invOut[i] = float(inv[i]);
    for (int i = 0; i < 3; ++i) posOut[i] = float(c.pos[i]);
    const float sunRadiance[4] = { 2.2f, 2.0f, 1.75f, 11.0f }, ambient[4] = { 0.30f, 0.36f, 0.46f, 11.0f };
    const float layer[4] = { 520.0f, 0.45f, 620.0f, 0.95f }, shape[4] = { 320.0f, 0.45f, 1.0f, 3.0f };
    const float right[4] = { float(c.right[0]), float(c.right[1]), float(c.right[2]), float(c.pos[0]) };
    const float up[4] = { float(c.up[0]), float(c.up[1]), float(c.up[2]), float(c.pos[1]) };
    const float fwd[4] = { float(c.fwd[0]), float(c.fwd[1]), float(c.fwd[2]), float(c.pos[2]) };
    const double sl = std::sqrt(0.35 * 0.35 + 0.6 * 0.6 + 0.72 * 0.72);
    const float sun[4] = { float(0.35 / sl), float(0.6 / sl), float(0.72 / sl), 8.0f }, status[4] = { 1, 0, 1, 1 };
    d->SetPixelShaderConstantF(16, sunRadiance, 1); d->SetPixelShaderConstantF(17, ambient, 1);
    d->SetPixelShaderConstantF(20, layer, 1); d->SetPixelShaderConstantF(21, shape, 1);
    d->SetPixelShaderConstantF(22, right, 1); d->SetPixelShaderConstantF(23, up, 1); d->SetPixelShaderConstantF(24, fwd, 1);
    d->SetPixelShaderConstantF(29, sun, 1); d->SetPixelShaderConstantF(31, status, 1);
    d->SetPixelShaderConstantF(32, invOut, 4);
    const float zero[4] = {}; d->SetPixelShaderConstantF(36, zero, 1);
}

// Depth: sky, a near block (in front of the layer) along the bottom, and a
// tower block on the left whose surface sits inside or behind the layer.
static void FillDepth(Device& g) {
    IDirect3DDevice9* d = g.d;
    IDirect3DSurface9* color = nullptr; d->GetRenderTarget(0, &color);
    d->SetRenderTarget(0, g.outS); d->SetDepthStencilSurface(g.depthS);
    D3DVIEWPORT9 vp = { 0, 0, W, H, 0, 1 }; d->SetViewport(&vp);
    d->Clear(0, nullptr, D3DCLEAR_ZBUFFER | D3DCLEAR_TARGET, 0, kSkyDepth, 0);
    d->SetPixelShader(nullptr); d->SetVertexShader(nullptr);
    d->SetRenderState(D3DRS_ZENABLE, TRUE); d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE); d->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 0); d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); d->SetRenderState(D3DRS_LIGHTING, FALSE);
    struct V { float x, y, z, rhw; };
    auto fill = [&](float x0, float y0, float x1, float y1, float z) {
        const V q[] = { { x0, y0, z, 1 }, { x1, y0, z, 1 }, { x0, y1, z, 1 }, { x1, y1, z, 1 } };
        d->SetFVF(D3DFVF_XYZRHW); d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
    };
    fill(0, H * 0.78f, float(W), float(H), RawDepth(300.0));            // near block
    fill(W * 0.05f, H * 0.08f, W * 0.30f, H * 0.78f, RawDepth(1250.0)); // tower inside the deck
    fill(W * 0.62f, H * 0.30f, W * 0.70f, H * 0.78f, RawDepth(900.0));  // slim tower crossing the deck base
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 15); d->SetRenderState(D3DRS_ZENABLE, FALSE); d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    d->SetDepthStencilSurface(nullptr);
    d->SetRenderTarget(0, color); if (color) color->Release();
}

static bool DrawComposite(Device& g, IDirect3DPixelShader9* shader, bool reference) {
    IDirect3DDevice9* d = g.d;
    d->SetRenderTarget(0, g.outS);
    D3DVIEWPORT9 vp = { 0, 0, W, H, 0, 1 }; d->SetViewport(&vp);
    d->SetVertexShader(nullptr); d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    d->SetRenderState(D3DRS_ZENABLE, FALSE); d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE); d->SetRenderState(D3DRS_COLORWRITEENABLE, 15);
    d->SetTexture(3, g.depth);
    for (auto s : { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER }) d->SetSamplerState(3, s, D3DTEXF_POINT);
    d->SetSamplerState(3, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(3, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); d->SetSamplerState(3, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    const float mode[4] = { reference ? 1.0f : 0.0f, 0, 0, 0 }; d->SetPixelShaderConstantF(39, mode, 1);
    d->SetPixelShader(shader);
    return WosIndirect::Quad(d, W, H);
}

static bool ReadBack(Device& g, std::vector<float>& rgba) {
    if (FAILED(g.d->GetRenderTargetData(g.outS, g.readback))) return false;
    D3DLOCKED_RECT lr = {}; if (FAILED(g.readback->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return false;
    rgba.resize(size_t(W) * H * 4);
    for (UINT y = 0; y < H; ++y) std::memcpy(&rgba[size_t(y) * W * 4], static_cast<const char*>(lr.pBits) + size_t(y) * lr.Pitch, size_t(W) * 16);
    g.readback->UnlockRect(); return true;
}

// Pixel classes from the depth layout above.
static int ClassOf(UINT x, UINT y) {
    if (y >= H * 0.78f) return 1;                                                   // in front of the layer
    if ((x >= W * 0.05f && x < W * 0.30f && y >= H * 0.08f) || (x >= W * 0.62f && x < W * 0.70f && y >= H * 0.30f)) return 2; // towers
    return 0;                                                                        // sky
}
struct Stats { double t = 0, l = 0, tMax = 0, lMax = 0, lumaMean = 0; unsigned long n = 0; };
static void Compare(const std::vector<float>& a, const std::vector<float>& b, Stats s[3]) {
    for (UINT y = 0; y < H; ++y) for (UINT x = 0; x < W; ++x) {
        const size_t i = (size_t(y) * W + x) * 4; Stats& k = s[ClassOf(x, y)];
        const double la = .2126 * a[i] + .7152 * a[i + 1] + .0722 * a[i + 2], lb = .2126 * b[i] + .7152 * b[i + 1] + .0722 * b[i + 2];
        const double dt = std::fabs(double(a[i + 3]) - b[i + 3]), dl = std::fabs(la - lb);
        if (!std::isfinite(dt) || !std::isfinite(dl)) { k.t = k.l = 1e9; continue; }
        k.t += dt; k.l += dl; k.tMax = (std::max)(k.tMax, dt); k.lMax = (std::max)(k.lMax, dl); k.lumaMean += lb; ++k.n;
    }
    for (int c = 0; c < 3; ++c) if (s[c].n) { s[c].t /= s[c].n; s[c].l /= s[c].n; s[c].lumaMean /= s[c].n; }
}
static void WriteRaw(const std::string& name, const std::vector<float>& rgba) {
    // Clouds over a flat sky, so the png shows what a player would.
    std::vector<float> rgb(size_t(W) * H * 3);
    for (size_t p = 0; p < size_t(W) * H; ++p) {
        const int cls = ClassOf(UINT(p % W), UINT(p / W));
        const float bg[3][3] = { { 0.32f, 0.52f, 0.85f }, { 0.12f, 0.11f, 0.10f }, { 0.35f, 0.33f, 0.31f } };
        for (int c = 0; c < 3; ++c) rgb[p * 3 + c] = bg[cls][c] * rgba[p * 4 + 3] + rgba[p * 4 + c];
    }
    FILE* f = std::fopen(name.c_str(), "wb"); if (!f) return;
    std::fwrite(rgb.data(), sizeof(float), rgb.size(), f); std::fclose(f);
}
static void PrintStats(FILE* json, const char* label, const Stats s[3], bool comma) {
    static const char* names[3] = { "sky", "front", "towers" };
    std::fprintf(json, "    \"%s\": {", label);
    for (int c = 0; c < 3; ++c)
        std::fprintf(json, "%s\"%s\": {\"mean_abs_transmittance\": %.5f, \"max_abs_transmittance\": %.5f, \"mean_abs_luma\": %.5f, \"max_abs_luma\": %.5f, \"reference_luma\": %.5f, \"pixels\": %lu}",
                     c ? ", " : "", names[c], s[c].t, s[c].tMax, s[c].l, s[c].lMax, s[c].lumaMean, s[c].n);
    std::fprintf(json, "}%s\n", comma ? "," : "");
}

// One frame as the DLL runs it: bloom constants, BloomScope, cloud lines.
static bool Frame(Device& g, const Camera& c) {
    ++g_frame_index;
    float inv[16], pos[3];
    UploadBloomConstants(g.d, c, inv, pos);
    g.d->SetRenderTarget(0, g.outS);
    D3DVIEWPORT9 vp = { 0, 0, W, H, 0, 1 }; g.d->SetViewport(&vp);
    WosCloudLowRes::BloomScope scope(g.d, true, true, pos, inv);
    float info[4] = {}; g.d->GetPixelShaderConstantF(36, info, 1);
    if (info[3] != 11.0f) return false;
    return DrawComposite(g, g.composite, false);
}

static bool Scene(Device& g, FILE* json, const char* name, int divisor, int frames, bool moving, bool last, double height = 30) {
    WosCloudLowRes::Reset(); WosCloudLowRes::directory = "..";
    WosCloudLowRes::divisor = divisor;
    Camera c = MakeCamera(0, height, 0, 0, 22);
    for (int k = 0; k < frames; ++k) {
        // Fast traversal: about 90 world units per second at 60 fps, turning 24 degrees a second.
        if (moving) c = MakeCamera(1.0 * k, height + 0.2 * k, 1.2 * k, 0.4 * k, 22 - 0.05 * k);
        if (!Frame(g, c)) { std::fprintf(json, "  \"%s\": {\"error\": \"frame %d: %s\"},\n", name, k, WosCloudLowRes::status); return false; }
    }
    std::vector<float> fresh, old, truth;
    if (!ReadBack(g, fresh)) return false;
    float inv[16], pos[3];
    UploadBloomConstants(g.d, c, inv, pos);
    if (!DrawComposite(g, g.composite, true) || !ReadBack(g, old)) return false;
    if (!DrawComposite(g, g.truth, true) || !ReadBack(g, truth)) return false;
    Stats lowres[3], perPixel[3];
    Compare(fresh, truth, lowres); Compare(old, truth, perPixel);
    std::fprintf(json, "  \"%s\": {\n    \"divisor\": %d, \"frames\": %d, \"moving\": %s, \"history_resets\": %lu, \"buffer\": [%u, %u], \"march\": [%u, %u],\n",
                 name, divisor, frames, moving ? "true" : "false", WosCloudLowRes::historyResets,
                 WosCloudLowRes::bufferWidth, WosCloudLowRes::bufferHeight, WosCloudLowRes::marchWidth, WosCloudLowRes::marchHeight);
    PrintStats(json, "lowres_vs_truth", lowres, true);
    PrintStats(json, "per_pixel_64_vs_truth", perPixel, false);
    std::fprintf(json, "  }%s\n", last ? "" : ",");
    WriteRaw(std::string(name) + "_new.raw", fresh); WriteRaw(std::string(name) + "_old.raw", old); WriteRaw(std::string(name) + "_gt.raw", truth);
    return true;
}

// GPU milliseconds per frame of a workload, from timestamp queries.
template<class F> static double Time(IDirect3DDevice9* d, int runs, F&& work) {
    IDirect3DQuery9 *disjoint = nullptr, *freq = nullptr, *t0 = nullptr, *t1 = nullptr;
    if (FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &disjoint)) || FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &freq)) ||
        FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &t0)) || FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &t1))) return -1;
    work(); // warm up
    disjoint->Issue(D3DISSUE_BEGIN); t0->Issue(D3DISSUE_END);
    for (int i = 0; i < runs; ++i) work();
    t1->Issue(D3DISSUE_END); disjoint->Issue(D3DISSUE_END); freq->Issue(D3DISSUE_END);
    UINT64 a = 0, b = 0, f = 0; BOOL bad = TRUE;
    while (t1->GetData(&b, sizeof(b), D3DGETDATA_FLUSH) == S_FALSE) {}
    while (t0->GetData(&a, sizeof(a), D3DGETDATA_FLUSH) == S_FALSE) {}
    while (freq->GetData(&f, sizeof(f), D3DGETDATA_FLUSH) == S_FALSE) {}
    while (disjoint->GetData(&bad, sizeof(bad), D3DGETDATA_FLUSH) == S_FALSE) {}
    disjoint->Release(); freq->Release(); t0->Release(); t1->Release();
    return bad || !f ? -1 : double(b - a) * 1000.0 / double(f) / runs;
}

int main() {
    Device g;
    WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.lpszClassName = "cloud_lowres_test"; RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("cloud_lowres_test", "", WS_OVERLAPPEDWINDOW, 0, 0, 640, 360, nullptr, nullptr, nullptr, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferWidth = 640; pp.BackBufferHeight = 360;
    pp.BackBufferFormat = D3DFMT_UNKNOWN; pp.hDeviceWindow = hwnd;
    if (!d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g.d))) { std::printf("{\"error\":\"device\"}\n"); return 1; }
    IDirect3DDevice9* d = g.d;
    D3DADAPTER_IDENTIFIER9 id = {}; d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
    bool ok = SUCCEEDED(d->CreateTexture(W, H, 1, D3DUSAGE_DEPTHSTENCIL, D3DFORMAT(MAKEFOURCC('I', 'N', 'T', 'Z')), D3DPOOL_DEFAULT, &g.depth, nullptr)) &&
              SUCCEEDED(g.depth->GetSurfaceLevel(0, &g.depthS)) &&
              SUCCEEDED(d->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &g.out, nullptr)) &&
              SUCCEEDED(g.out->GetSurfaceLevel(0, &g.outS)) &&
              SUCCEEDED(d->CreateOffscreenPlainSurface(W, H, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &g.readback, nullptr));
    auto composite = ReadAll("composite_test.cso"), truth = ReadAll("composite_truth.cso");
    ok = ok && !composite.empty() && !truth.empty() &&
         SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(composite.data()), &g.composite)) &&
         SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(truth.data()), &g.truth));
    if (!ok) { std::printf("{\"error\":\"resources\"}\n"); return 1; }
    d->BeginScene();
    FillDepth(g);

    FILE* json = std::fopen("gpu_test.json", "w");
    std::fprintf(json, "{\n  \"adapter\": \"%s\", \"width\": %u, \"height\": %u,\n", id.Description, W, H);
    bool scenes = Scene(g, json, "static_half", 2, 32, false, false) &&
                  Scene(g, json, "pan_half", 2, 32, true, false) &&
                  Scene(g, json, "static_quarter", 4, 32, false, false) &&
                  Scene(g, json, "inside_half", 2, 32, false, true, 680);

    // Timing on the static camera.
    Camera c = MakeCamera(0, 30, 0, 0, 22);
    float inv[16], pos[3];
    double perPixel = Time(d, 20, [&] { UploadBloomConstants(d, c, inv, pos); DrawComposite(g, g.composite, true); });
    double lowTimes[2] = {};
    for (int k = 0; k < 2; ++k) {
        WosCloudLowRes::Reset(); WosCloudLowRes::directory = ".."; WosCloudLowRes::divisor = k ? 4 : 2;
        lowTimes[k] = Time(d, 20, [&] { Frame(g, c); });
    }
    double compositeOnly = Time(d, 20, [&] {
        UploadBloomConstants(d, c, inv, pos);
        const float info[4] = { 0, 0, 0, 11.0f }; d->SetPixelShaderConstantF(36, info, 1);
        d->SetTexture(4, WosCloudLowRes::history[WosCloudLowRes::current]); DrawComposite(g, g.composite, false); d->SetTexture(4, nullptr);
    });
    // Camera inside the deck: every geometry pixel takes the short per-pixel march.
    Camera inside = MakeCamera(0, 680, 0, 0, 22);
    double insidePerPixel = Time(d, 20, [&] { UploadBloomConstants(d, inside, inv, pos); DrawComposite(g, g.composite, true); });
    WosCloudLowRes::Reset(); WosCloudLowRes::directory = ".."; WosCloudLowRes::divisor = 2;
    double insideHalf = Time(d, 20, [&] { Frame(g, inside); });
    std::fprintf(json, "  ,\"gpu_ms\": {\"per_pixel_march\": %.3f, \"half_march_resolve_composite\": %.3f, \"quarter_march_resolve_composite\": %.3f, \"composite_only\": %.3f,"
                       " \"inside_deck_per_pixel\": %.3f, \"inside_deck_half\": %.3f}\n",
                 perPixel, lowTimes[0], lowTimes[1], compositeOnly, insidePerPixel, insideHalf);
    std::fprintf(json, "  ,\"scenes_ok\": %s, \"failures\": %lu\n}\n", scenes ? "true" : "false", WosCloudLowRes::failures);
    std::fclose(json);
    d->EndScene();
    std::printf("done\n");
    return scenes ? 0 : 2;
}
