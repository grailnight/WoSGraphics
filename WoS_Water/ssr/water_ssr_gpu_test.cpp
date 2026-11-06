// GPU test for WoS_Water/ssr/water_ssr.h on a real D3D9 HAL device.
//
// Scene (camera-relative, camera 5 m above the water, looking 10 degrees down):
//   sky clear colour, a red wall 40 m ahead, a water plane drawn through the
//   runtime's mask replay, and a green "boat" drawn over the water afterwards.
// Checks: mask replay count and state restoration, wall reflection in the water,
// sky reflection at the side, untouched non-water and boat pixels, the disabled
// and stale-frame paths, the sun glint, and finite output.
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------- host stubs
static IDirect3DPixelShader9* g_current_shader = nullptr;
static int g_frame_index = 1;
static HRESULT STDMETHODCALLTYPE SetPS(IDirect3DDevice9* d, IDirect3DPixelShader9* p) { return d->SetPixelShader(p); }
static HRESULT STDMETHODCALLTYPE DrawUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT n, const void* v, UINT s) { return d->DrawPrimitiveUP(t, n, v, s); }
static auto g_original_set_pixel_shader = SetPS;
static auto g_original_draw_primitive_up = DrawUP;
namespace WosProbe { static bool suppress = false; }
namespace WosDeferred { static bool busy = false; }
namespace WosVolumetric { static bool sunValid = false; static float sunDirection[3] = {}, sunColour[3] = {}; }
namespace WosCascadeProbe {
static std::vector<unsigned char> ReadFile(const std::string& path) {
    std::vector<unsigned char> b; FILE* f = std::fopen(path.c_str(), "rb"); if (!f) return b;
    std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::rewind(f);
    if (n > 0) { b.resize(size_t(n)); if (std::fread(b.data(), 1, b.size(), f) != b.size()) b.clear(); }
    std::fclose(f); return b;
}
}
namespace WosShadowTransform { struct Matrix4 { float v[16]; }; }
namespace WosCascades {
static bool Inverse(const WosShadowTransform::Matrix4& m, double out[16]) {
    double a[4][8] = {};
    for (int i = 0; i < 4; ++i) { for (int j = 0; j < 4; ++j) a[i][j] = m.v[i * 4 + j]; a[i][4 + i] = 1; }
    for (int c = 0; c < 4; ++c) {
        int p = c; for (int i = c + 1; i < 4; ++i) if (std::fabs(a[i][c]) > std::fabs(a[p][c])) p = i;
        if (!(std::fabs(a[p][c]) > 1e-300)) return false;
        if (p != c) for (int j = 0; j < 8; ++j) std::swap(a[c][j], a[p][j]);
        double inv = 1.0 / a[c][c]; for (int j = 0; j < 8; ++j) a[c][j] *= inv;
        for (int i = 0; i < 4; ++i) if (i != c) { double f = a[i][c]; for (int j = 0; j < 8; ++j) a[i][j] -= f * a[c][j]; }
    }
    for (int i = 0; i < 16; ++i) out[i] = a[i / 4][4 + i % 4];
    return true;
}
}
namespace WosIndirectCamera {
struct Camera { float inverse[16] = {}, position[3] = {}, relativeInverse[16] = {}; double worldOrigin[3] = {}; uint32_t scene = 0; };
static bool frameReady = true;
static uint32_t scene = 7;
static Camera camera;
static IDirect3DTexture9* depth = nullptr;
static uint32_t CurrentScene() { return scene; }
static bool Same(IDirect3DBaseTexture9* t) {
    IUnknown* a = nullptr, * b = nullptr; bool same = false;
    if (t && depth && SUCCEEDED(t->QueryInterface(IID_IUnknown, (void**)&a)) && SUCCEEDED(depth->QueryInterface(IID_IUnknown, (void**)&b))) same = a == b;
    if (a) a->Release(); if (b) b->Release(); return same;
}
static bool Find(IDirect3DBaseTexture9* t, unsigned, unsigned, Camera& out, uint32_t s) { if (!frameReady || s != scene || !Same(t)) return false; out = camera; return true; }
static bool Acquire(unsigned, unsigned, Camera& out, IDirect3DTexture9*& d, uint32_t s) {
    d = nullptr; if (!frameReady || s != scene || !depth) return false; depth->AddRef(); d = depth; out = camera; return true;
}
}
// The two WosIndirect helpers the runtime borrows, copied from native_indirect.h.
namespace WosIndirect {
struct State {
 IDirect3DDevice9*d;IDirect3DStateBlock9*block=nullptr;IDirect3DSurface9*rt[4]={},*ds=nullptr;IDirect3DPixelShader9*ps=nullptr;D3DVIEWPORT9 vp={};unsigned slots=0;bool valid=false;
 explicit State(IDirect3DDevice9*device):d(device){
  D3DCAPS9 caps={};if(FAILED(d->GetDeviceCaps(&caps))||FAILED(d->GetViewport(&vp))||FAILED(d->CreateStateBlock(D3DSBT_ALL,&block))||FAILED(block->Capture()))return;
  slots=(std::min)(caps.NumSimultaneousRTs,DWORD(4));
  for(unsigned i=0;i<slots;++i){HRESULT h=d->GetRenderTarget(i,&rt[i]);if(FAILED(h)&&h!=D3DERR_NOTFOUND)return;}
  HRESULT h=d->GetDepthStencilSurface(&ds);if(FAILED(h)&&h!=D3DERR_NOTFOUND)return;
  if(FAILED(d->GetPixelShader(&ps)))return;valid=true;
 }
 bool Restore(){
  if(!valid)return false;bool ok=SUCCEEDED(d->SetDepthStencilSurface(nullptr));
  for(unsigned i=0;i<slots;++i)ok=SUCCEEDED(d->SetRenderTarget(i,rt[i]))&&ok;
  ok=SUCCEEDED(d->SetDepthStencilSurface(ds))&&ok;ok=SUCCEEDED(block->Apply())&&ok;ok=SUCCEEDED(g_original_set_pixel_shader(d,ps))&&ok;ok=SUCCEEDED(d->SetViewport(&vp))&&ok;valid=false;return ok;
 }
 ~State(){if(valid)Restore();for(auto*r:rt)if(r)r->Release();if(ds)ds->Release();if(ps)ps->Release();if(block)block->Release();}
};
static bool Quad(IDirect3DDevice9*d,unsigned w,unsigned h){
 struct V{float x,y,z,rhw,u,v;};const V q[]={{-.5f,-.5f,0,1,0,0},{float(w)-.5f,-.5f,0,1,1,0},{-.5f,float(h)-.5f,0,1,0,1},{float(w)-.5f,float(h)-.5f,0,1,1,1}};
 D3DVIEWPORT9 vp={0,0,w,h,0,1};return SUCCEEDED(d->SetViewport(&vp))&&SUCCEEDED(g_original_draw_primitive_up(d,D3DPT_TRIANGLESTRIP,2,q,sizeof(V)));
}
}
#include "water_ssr.h"

// ---------------------------------------------------------------------- test
static int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("  FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static const UINT W = 800, H = 450;
static const std::string Here = "B:\\Games\\Spider Man - Web of Shadows\\WoS_Water\\ssr";

struct Mat { double m[16]; };
static Mat Mul(const Mat& a, const Mat& b) { Mat r{}; for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) for (int k = 0; k < 4; ++k) r.m[i * 4 + j] += a.m[i * 4 + k] * b.m[k * 4 + j]; return r; }

struct Fixture {
    IDirect3D9* d3d = nullptr; IDirect3DDevice9* d = nullptr; HWND window = nullptr;
    IDirect3DTexture9* scene = nullptr, * depth = nullptr, * normal = nullptr;
    IDirect3DSurface9* sceneRT = nullptr, * depthDS = nullptr;
    IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* solid = nullptr, * water = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr;
    float vp[16] = {};
    double eye[3] = { 0, 0, 0 };
};

static std::vector<unsigned char> Read(const std::string& p) { return WosCascadeProbe::ReadFile(p); }

static bool Setup(Fixture& f) {
    f.window = CreateWindowA("STATIC", "water_ssr_test", WS_OVERLAPPEDWINDOW, 0, 0, W, H, nullptr, nullptr, nullptr, nullptr);
    f.d3d = Direct3DCreate9(D3D_SDK_VERSION); if (!f.d3d) return false;
    D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferWidth = W; pp.BackBufferHeight = H; pp.hDeviceWindow = f.window;
    if (FAILED(f.d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, f.window, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &f.d))) return false;
    const D3DFORMAT intz = D3DFORMAT(MAKEFOURCC('I', 'N', 'T', 'Z'));
    bool ok = SUCCEEDED(f.d->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &f.scene, nullptr)) &&
              SUCCEEDED(f.scene->GetSurfaceLevel(0, &f.sceneRT)) &&
              SUCCEEDED(f.d->CreateTexture(W, H, 1, D3DUSAGE_DEPTHSTENCIL, intz, D3DPOOL_DEFAULT, &f.depth, nullptr)) &&
              SUCCEEDED(f.depth->GetSurfaceLevel(0, &f.depthDS)) &&
              SUCCEEDED(f.d->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &f.normal, nullptr));
    if (!ok) return false;
    D3DLOCKED_RECT lr = {}; if (FAILED(f.normal->LockRect(0, &lr, nullptr, 0))) return false;
    *static_cast<DWORD*>(lr.pBits) = 0xff8080ff; f.normal->UnlockRect(0);
    auto vsb = Read(Here + "\\test_scene_vs.cso"), psb = Read(Here + "\\test_scene_ps.cso");
    if (vsb.empty() || psb.empty()) return false;
    ok = SUCCEEDED(f.d->CreateVertexShader((const DWORD*)vsb.data(), &f.vs)) &&
         SUCCEEDED(f.d->CreatePixelShader((const DWORD*)psb.data(), &f.solid)) &&
         SUCCEEDED(f.d->CreatePixelShader((const DWORD*)psb.data(), &f.water));
    const D3DVERTEXELEMENT9 el[] = { {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END() };
    ok = ok && SUCCEEDED(f.d->CreateVertexDeclaration(el, &f.decl));
    // Camera 5 m above the water at the origin, looking +z and 10 degrees down.
    const double pitch = 10.0 * 3.14159265358979 / 180.0, fov = 60.0 * 3.14159265358979 / 180.0, aspect = double(W) / H, n = 0.25, far_ = 2000.0;
    Mat view = { { 1, 0, 0, 0,  0, std::cos(pitch), std::sin(pitch), 0,  0, -std::sin(pitch), std::cos(pitch), 0,  0, 0, 0, 1 } };
    const double ty = 1.0 / std::tan(fov / 2), tx = ty / aspect;
    Mat proj = { { tx, 0, 0, 0,  0, ty, 0, 0,  0, 0, far_ / (far_ - n), -n * far_ / (far_ - n),  0, 0, 1, 0 } };
    Mat vpm = Mul(proj, view);
    WosShadowTransform::Matrix4 m; for (int i = 0; i < 16; ++i) { f.vp[i] = float(vpm.m[i]); m.v[i] = f.vp[i]; }
    double inv[16]; if (!WosCascades::Inverse(m, inv)) return false;
    for (int i = 0; i < 16; ++i) WosIndirectCamera::camera.relativeInverse[i] = float(inv[i]);
    WosIndirectCamera::camera.scene = WosIndirectCamera::scene;
    WosIndirectCamera::depth = f.depth;
    return ok;
}

// Positions are camera-relative: the water plane is 5 m below the eye.
static bool DrawQuad(Fixture& f, IDirect3DPixelShader9* ps, const float colour[4], const float corners[12]) {
    const WORD idx[] = { 0, 1, 2, 0, 2, 3 };
    f.d->SetVertexDeclaration(f.decl); f.d->SetVertexShader(f.vs); f.d->SetPixelShader(ps);
    f.d->SetVertexShaderConstantF(0, f.vp, 4); f.d->SetPixelShaderConstantF(0, colour, 1);
    return SUCCEEDED(f.d->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 4, 2, idx, D3DFMT_INDEX16, corners, sizeof(float) * 3));
}

// DrawIndexedPrimitiveUP forgets the stream after each call, so the test draws
// its water through a real vertex/index buffer like the engine does.
struct Buffers { IDirect3DVertexBuffer9* vb = nullptr; IDirect3DIndexBuffer9* ib = nullptr; };
static Buffers WaterBuffers(Fixture& f) {
    Buffers b; const float y = -5.0f;
    const float v[] = { -200, y, 0.5f,  200, y, 0.5f,  200, y, 400,  -200, y, 400 };
    const WORD idx[] = { 0, 2, 1, 0, 3, 2 };
    void* p = nullptr;
    if (SUCCEEDED(f.d->CreateVertexBuffer(sizeof(v), 0, 0, D3DPOOL_MANAGED, &b.vb, nullptr)) && SUCCEEDED(b.vb->Lock(0, 0, &p, 0))) { std::memcpy(p, v, sizeof(v)); b.vb->Unlock(); }
    if (SUCCEEDED(f.d->CreateIndexBuffer(sizeof(idx), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &b.ib, nullptr)) && SUCCEEDED(b.ib->Lock(0, 0, &p, 0))) { std::memcpy(p, idx, sizeof(idx)); b.ib->Unlock(); }
    return b;
}

struct Frame { std::vector<float> rgba; };
static bool ReadBack(Fixture& f, Frame& out) {
    IDirect3DSurface9* cpu = nullptr;
    if (FAILED(f.d->CreateOffscreenPlainSurface(W, H, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &cpu, nullptr))) return false;
    bool ok = SUCCEEDED(f.d->GetRenderTargetData(f.sceneRT, cpu));
    D3DLOCKED_RECT lr = {};
    if (ok && SUCCEEDED(cpu->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        out.rgba.resize(size_t(W) * H * 4);
        auto half = [](uint16_t h) {
            int s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
            float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp(float(m | 1024), e - 25);
            return s ? -v : v;
        };
        for (UINT y = 0; y < H; ++y) {
            const uint16_t* row = static_cast<uint16_t*>(lr.pBits) + y * (lr.Pitch / 2);
            for (UINT x = 0; x < W * 4; ++x) out.rgba[y * W * 4 + x] = half(row[x]);
        }
        cpu->UnlockRect();
    } else ok = false;
    cpu->Release();
    return ok;
}
static const float* Pixel(const Frame& fr, double u, double v) { UINT x = UINT(u * W), y = UINT(v * H); return &fr.rgba[(y * W + x) * 4]; }

// Draws the scene. withBoat covers part of the water after it was drawn.
static bool DrawScene(Fixture& f, Buffers& water, bool withBoat) {
    bool ok = SUCCEEDED(f.d->SetRenderTarget(0, f.sceneRT)) && SUCCEEDED(f.d->SetDepthStencilSurface(f.depthDS));
    ok = ok && SUCCEEDED(f.d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f, 0));
    const float sky[4] = { 0.3f, 0.5f, 1.0f, 1.0f };
    // Clear writes UNORM-clamped colour; paint the HDR sky with a far quad instead.
    f.d->SetRenderState(D3DRS_ZENABLE, TRUE); f.d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE); f.d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    f.d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); f.d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    f.d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE); f.d->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
    { struct V { float x, y, z, rhw; }; const V q[] = { {-.5f,-.5f,0,1},{W-.5f,-.5f,0,1},{-.5f,H-.5f,0,1},{W-.5f,H-.5f,0,1} };
      f.d->SetVertexShader(nullptr); f.d->SetFVF(D3DFVF_XYZRHW); f.d->SetPixelShader(f.solid); f.d->SetPixelShaderConstantF(0, sky, 1);
      ok = SUCCEEDED(f.d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V))) && ok; }
    f.d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE); f.d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    const float red[4] = { 1, 0, 0, 1 };
    const float wall[] = { -10, -5, 40,  10, -5, 40,  10, 15, 40,  -10, 15, 40 };
    ok = DrawQuad(f, f.solid, red, wall) && ok;
    // Water through the runtime, exactly as the hook calls it.
    const float blue[4] = { 0.02f, 0.05f, 0.08f, 1 };
    const float waterConstants[] = { 0, 0, 0, 0,  1, 1, 1, 1,  1, 0, 0, 0,  -0.025f, -0.025f, 0, 0,  8.392f, 0, 0, 0,  0.025f, -0.025f, 0, 0,  0.8f, 0, 0, 0,  0, -0.025f, 0, 0,  1.2f, 0, 0, 0,  0.3f, 0, 0, 0 };
    f.d->SetVertexDeclaration(f.decl); f.d->SetVertexShader(f.vs); f.d->SetPixelShader(f.water);
    f.d->SetVertexShaderConstantF(0, f.vp, 4); f.d->SetPixelShaderConstantF(0, blue, 1);
    f.d->SetPixelShaderConstantF(6, waterConstants, 10);
    f.d->SetTexture(1, f.normal);
    f.d->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP); f.d->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    f.d->SetStreamSource(0, water.vb, 0, sizeof(float) * 3); f.d->SetIndices(water.ib);
    g_current_shader = f.water;
    ok = SUCCEEDED(WosWaterSSR::Draw(f.d, [&]() { return f.d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2); })) && ok;
    g_current_shader = nullptr;
    f.d->SetTexture(1, nullptr);
    if (withBoat) {
        const float green[4] = { 0, 1, 0, 1 };
        const float boat[] = { -3, -4.2f, 8,  3, -4.2f, 8,  3, -4.2f, 11,  -3, -4.2f, 11 };
        ok = DrawQuad(f, f.solid, green, boat) && ok;
    }
    return ok;
}

static bool Bloom(Fixture& f) {
    // The bloom composite samples the scene on stage 0 and writes elsewhere.
    IDirect3DSurface9* back = nullptr; f.d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
    f.d->SetRenderTarget(0, back); f.d->SetDepthStencilSurface(nullptr); back->Release();
    f.d->SetTexture(0, f.scene);
    WosWaterSSR::BloomDraw(f.d, true);
    f.d->SetTexture(0, nullptr);
    return true;
}

int main() {
    Fixture f;
    if (!Setup(f)) { std::printf("SETUP FAILED\n"); return 2; }
    WosWaterSSR::host.setPS = SetPS;
    WosWaterSSR::directory = Here;
    WosWaterSSR::RegisterWater(f.water);
    Buffers water = WaterBuffers(f);
    const double wallUV[2] = { 0.5, 0.561 }, skyUV[2] = { 0.874, 0.56 }, boatUV[2] = { 0.5, 0.72 }, wallPixel[2] = { 0.5, 0.30 }, skyPixel[2] = { 0.1, 0.1 };

    std::printf("case: disabled pass leaves the frame unchanged\n");
    WosWaterSSR::enabled = false;
    CHECK(f.d->BeginScene() == D3D_OK);
    CHECK(DrawScene(f, water, true));
    Frame reference; CHECK(ReadBack(f, reference));
    Bloom(f);
    Frame disabled; CHECK(ReadBack(f, disabled));
    CHECK(disabled.rgba == reference.rgba);
    CHECK(WosWaterSSR::maskDraws == 0);
    WosWaterSSR::enabled = true;

    std::printf("case: mask replay restores device state\n");
    ++g_frame_index;
    CHECK(DrawScene(f, water, true));
    CHECK(WosWaterSSR::lastMaskDraws == 1);
    CHECK(!WosWaterSSR::faulted);
    { IDirect3DSurface9* rt = nullptr, * rt1 = nullptr; IDirect3DPixelShader9* ps = nullptr; DWORD zf = 0, zw = 0;
      f.d->GetRenderTarget(0, &rt); f.d->GetRenderTarget(1, &rt1); f.d->GetPixelShader(&ps); f.d->GetRenderState(D3DRS_ZFUNC, &zf); f.d->GetRenderState(D3DRS_ZWRITEENABLE, &zw);
      CHECK(rt == f.sceneRT); CHECK(rt1 == nullptr); CHECK(ps == f.solid); CHECK(zf == D3DCMP_LESSEQUAL); CHECK(zw == TRUE);
      if (rt) rt->Release(); if (rt1) rt1->Release(); if (ps) ps->Release(); }
    Frame before; CHECK(ReadBack(f, before));
    CHECK(before.rgba == reference.rgba); // the replay never touches the scene

    std::printf("case: reflections\n");
    Bloom(f);
    Frame after; CHECK(ReadBack(f, after));
    CHECK(WosWaterSSR::frames == 1);
    const float* w0 = Pixel(before, wallUV[0], wallUV[1]), * w1 = Pixel(after, wallUV[0], wallUV[1]);
    const float* s0 = Pixel(before, skyUV[0], skyUV[1]), * s1 = Pixel(after, skyUV[0], skyUV[1]);
    std::printf("  wall reflection  before %.4f %.4f %.4f  after %.4f %.4f %.4f\n", w0[0], w0[1], w0[2], w1[0], w1[1], w1[2]);
    std::printf("  sky reflection   before %.4f %.4f %.4f  after %.4f %.4f %.4f\n", s0[0], s0[1], s0[2], s1[0], s1[1], s1[2]);
    CHECK(w1[0] - w0[0] > 0.1f);            // the red wall appears in the water in front of it
    CHECK(w1[0] - w0[0] > 4.0f * std::fabs(w1[2] - w0[2]));
    CHECK(s1[2] - s0[2] > 0.02f);           // the side of the lake mirrors the sky
    // and not the wall: every channel moves the same fraction toward the sky colour.
    CHECK(std::fabs((s1[0] - s0[0]) / (0.3f - s0[0]) - (s1[2] - s0[2]) / (1.0f - s0[2])) < 0.02f);
    CHECK(std::fabs((s1[1] - s0[1]) / (0.5f - s0[1]) - (s1[2] - s0[2]) / (1.0f - s0[2])) < 0.02f);
    CHECK(std::memcmp(Pixel(before, boatUV[0], boatUV[1]), Pixel(after, boatUV[0], boatUV[1]), 16) == 0);
    CHECK(std::memcmp(Pixel(before, wallPixel[0], wallPixel[1]), Pixel(after, wallPixel[0], wallPixel[1]), 16) == 0);
    CHECK(std::memcmp(Pixel(before, skyPixel[0], skyPixel[1]), Pixel(after, skyPixel[0], skyPixel[1]), 16) == 0);
    size_t changed = 0, nonFinite = 0, alphaChanged = 0;
    for (size_t i = 0; i < after.rgba.size(); i += 4) {
        bool diff = std::memcmp(&after.rgba[i], &before.rgba[i], 12) != 0;
        changed += diff; alphaChanged += after.rgba[i + 3] != before.rgba[i + 3];
        for (int c = 0; c < 4; ++c) nonFinite += !std::isfinite(after.rgba[i + c]);
        // Only pixels that showed the native water may change.
        const bool wasWater = std::fabs(before.rgba[i] - 0.02f) < 0.002f && std::fabs(before.rgba[i + 1] - 0.05f) < 0.002f && std::fabs(before.rgba[i + 2] - 0.08f) < 0.002f;
        if (diff && !wasWater) { ++failures; std::printf("  FAIL: non-water pixel %zu changed\n", i / 4); break; }
    }
    std::printf("  changed pixels %zu, non-finite %zu, alpha changed %zu\n", changed, nonFinite, alphaChanged);
    CHECK(changed > 10000); CHECK(nonFinite == 0); CHECK(alphaChanged == 0);
    if (FILE* dump = std::fopen((Here + "\\test_frames.raw").c_str(), "wb")) {
        std::fwrite(before.rgba.data(), sizeof(float), before.rgba.size(), dump);
        std::fwrite(after.rgba.data(), sizeof(float), after.rgba.size(), dump);
        std::fclose(dump);
    }

    std::printf("case: a bloom in a later frame without water does nothing\n");
    ++g_frame_index;
    CHECK(DrawScene(f, water, true));
    Frame fresh; CHECK(ReadBack(f, fresh));
    ++g_frame_index; // the water above belongs to the previous frame
    Bloom(f);
    Frame stale; CHECK(ReadBack(f, stale));
    CHECK(stale.rgba == fresh.rgba);

    std::printf("case: second bloom draw in the same frame is ignored\n");
    ++g_frame_index;
    CHECK(DrawScene(f, water, false));
    Bloom(f); Frame once; CHECK(ReadBack(f, once));
    Bloom(f); Frame twice; CHECK(ReadBack(f, twice));
    CHECK(once.rgba == twice.rgba);

    std::printf("case: sun glint\n");
    ++g_frame_index;
    CHECK(DrawScene(f, water, false));
    Frame noSun; WosWaterSSR::glint = 0.0f; Bloom(f); CHECK(ReadBack(f, noSun));
    ++g_frame_index;
    CHECK(DrawScene(f, water, false));
    // The sun where the view ray towards skyUV mirrors to.
    const float dir[3] = { 0.606f, 0.2525f, 0.7576f }; const float len = std::sqrt(dir[0]*dir[0]+dir[1]*dir[1]+dir[2]*dir[2]);
    for (int i = 0; i < 3; ++i) { WosVolumetric::sunDirection[i] = dir[i] / len; WosVolumetric::sunColour[i] = 3.0f; }
    WosVolumetric::sunValid = true; WosWaterSSR::glint = 1.0f;
    Frame withSun; Bloom(f); CHECK(ReadBack(f, withSun));
    const float* g0 = Pixel(noSun, skyUV[0], skyUV[1]), * g1 = Pixel(withSun, skyUV[0], skyUV[1]);
    std::printf("  glint  without %.4f  with %.4f\n", g0[1], g1[1]);
    CHECK(g1[1] - g0[1] > 1.0f);
    CHECK(std::memcmp(Pixel(noSun, wallPixel[0], wallPixel[1]), Pixel(withSun, wallPixel[0], wallPixel[1]), 16) == 0);
    f.d->EndScene();

    std::printf(failures ? "FAILED (%d)\n" : "PASS\n", failures);
    return failures ? 1 : 0;
}
