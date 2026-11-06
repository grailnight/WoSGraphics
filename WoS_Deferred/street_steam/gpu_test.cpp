// HAL test for the street steam shaders: one 20 m sprite 30 m ahead, a depth
// "wall" at 25 m over the right half of the screen. Expected: steam visible on
// the left (open street), nothing where the wall is in front of it, a soft
// edge, no NaN. Writes gpu_test.png-ready raw floats to gpu_test.raw.
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

static std::vector<unsigned char> Read(const char* p) {
    std::vector<unsigned char> b; FILE* f = nullptr;
    if (fopen_s(&f, p, "rb") || !f) return b;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    b.resize(n); fread(b.data(), 1, n, f); fclose(f); return b;
}

static IDirect3DTexture9* LoadDds(IDirect3DDevice9* d, const std::vector<unsigned char>& bytes) {
    if (bytes.size() < 128 || std::memcmp(bytes.data(), "DDS ", 4)) return nullptr;
    const unsigned char* h = bytes.data() + 4;
    unsigned height, width, mips, fourcc;
    std::memcpy(&height, h + 8, 4); std::memcpy(&width, h + 12, 4); std::memcpy(&mips, h + 24, 4); std::memcpy(&fourcc, h + 80, 4);
    if (fourcc != MAKEFOURCC('D', 'X', 'T', '5')) return nullptr;
    IDirect3DTexture9* t = nullptr;
    if (FAILED(d->CreateTexture(width, height, mips, 0, D3DFMT_DXT5, D3DPOOL_MANAGED, &t, nullptr))) return nullptr;
    size_t offset = 128;
    for (unsigned level = 0; level < mips; ++level) {
        unsigned w = (std::max)(1u, width >> level), hh = (std::max)(1u, height >> level);
        unsigned rowBytes = (std::max)(1u, (w + 3) / 4) * 16, rows = (std::max)(1u, (hh + 3) / 4);
        if (offset + size_t(rowBytes) * rows > bytes.size()) { t->Release(); return nullptr; }
        D3DLOCKED_RECT r; t->LockRect(level, &r, nullptr, 0);
        for (unsigned y = 0; y < rows; ++y) std::memcpy((unsigned char*)r.pBits + y * r.Pitch, bytes.data() + offset + y * rowBytes, rowBytes);
        t->UnlockRect(level); offset += size_t(rowBytes) * rows;
    }
    return t;
}

// Row-major 4x4 helpers.
static void Mul(const double a[16], const double b[16], double o[16]) {
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) { double s = 0; for (int k = 0; k < 4; ++k) s += a[r * 4 + k] * b[k * 4 + c]; o[r * 4 + c] = s; }
}

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : ".";
    const unsigned W = 320, H = 200;
    HWND wnd = CreateWindowA("STATIC", "t", WS_POPUP, 0, 0, W, H, nullptr, nullptr, nullptr, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = wnd;
    pp.BackBufferWidth = W; pp.BackBufferHeight = H; pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    IDirect3DDevice9* d = nullptr;
    if (FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &d))) { printf("FAIL device\n"); return 1; }
    auto vsb = Read((dir + "\\street_steam_vs.cso").c_str()), psb = Read((dir + "\\street_steam_ps.cso").c_str());
    IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr;
    if (vsb.empty() || psb.empty() || FAILED(d->CreateVertexShader((const DWORD*)vsb.data(), &vs)) || FAILED(d->CreatePixelShader((const DWORD*)psb.data(), &ps))) { printf("FAIL shaders\n"); return 1; }
    IDirect3DTexture9* smoke = LoadDds(d, Read((dir + "\\fx_smk_light.dds").c_str()));
    if (!smoke) { printf("FAIL texture\n"); return 1; }

    // Camera at origin looking down +z; D3D perspective, near 0.5, far 12500.
    const double n = 0.5, f = 12500, fy = 1.0 / std::tan(0.35), fx = fy * H / W;
    const double P[16] = { fx, 0, 0, 0,  0, fy, 0, 0,  0, 0, f / (f - n), -n * f / (f - n),  0, 0, 1, 0 };
    // Inverse projection (camera-relative world = view space here).
    const double Pi[16] = { 1 / fx, 0, 0, 0,  0, 1 / fy, 0, 0,  0, 0, 0, 1,  0, 0, -(f - n) / (n * f), 1 / n };
    float inv[16], vp[20];
    for (int i = 0; i < 16; ++i) { inv[i] = float(Pi[i]); vp[i] = float(P[i]); }
    vp[16] = 0; vp[17] = 0; vp[18] = 1; vp[19] = 0;

    // Depth: right half is a wall at 25 m, left half open to 400 m.
    auto zOf = [&](double dist) { return float(f / (f - n) - n * f / (f - n) / dist); };
    IDirect3DTexture9* depth = nullptr; d->CreateTexture(W, H, 1, D3DUSAGE_DYNAMIC, D3DFMT_R32F, D3DPOOL_DEFAULT, &depth, nullptr);
    D3DLOCKED_RECT lr; depth->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD);
    for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) ((float*)((char*)lr.pBits + y * lr.Pitch))[x] = zOf(x >= W / 2 ? 25.0 : 400.0);
    depth->UnlockRect(0);

    IDirect3DTexture9* target = nullptr; IDirect3DSurface9* rt = nullptr;
    d->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &target, nullptr);
    target->GetSurfaceLevel(0, &rt);
    d->SetRenderTarget(0, rt);
    d->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 25, 25, 25), 1, 0);
    d->BeginScene();
    struct V { float x, y, z, u, v, a, pad; };
    const float s = 10, zc = 30;   // 20 m sprite at 30 m, frame 1
    V q[6] = { { -s, s, zc, .25f, 0, .38f, 1 }, { s, s, zc, .5f, 0, .38f, 1 }, { -s, -s, zc, .25f, .5f, .38f, 1 },
               { -s, -s, zc, .25f, .5f, .38f, 1 }, { s, s, zc, .5f, 0, .38f, 1 }, { s, -s, zc, .5f, .5f, .38f, 1 } };
    d->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE4(0));
    d->SetVertexShader(vs); d->SetPixelShader(ps);
    d->SetRenderState(D3DRS_ZENABLE, 0); d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); d->SetRenderState(D3DRS_LIGHTING, 0);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, 1); d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE); d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 7);
    d->SetTexture(0, smoke); d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, 1);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR); d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    d->SetTexture(1, depth); d->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_POINT); d->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetVertexShaderConstantF(0, vp, 5);
    const float psc[12] = { 1.f / W, 1.f / H, 8.f, .6666667f,  1.2f, 1.2f, 1.3f, 0,  0, 0, 1, 0 };
    d->SetPixelShaderConstantF(0, inv, 4); d->SetPixelShaderConstantF(4, psc, 3);
    HRESULT hr = d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, q, sizeof(V));
    d->EndScene();

    IDirect3DSurface9* sys = nullptr; d->CreateOffscreenPlainSurface(W, H, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    d->GetRenderTargetData(rt, sys);
    sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
    auto half = [](unsigned short h) { unsigned s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023; float v = e == 0 ? m / 1024.f * std::pow(2.f, -14.f) : e == 31 ? NAN : (1 + m / 1024.f) * std::pow(2.f, float(int(e) - 15)); return s ? -v : v; };
    double left = 0, right = 0, edge = 0; int nan = 0, ln = 0, rn = 0;
    std::vector<float> raw(W * H);
    for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) {
        const unsigned short* p = (const unsigned short*)((char*)lr.pBits + y * lr.Pitch) + x * 4;
        float r = half(p[0]); if (!(r == r)) ++nan;
        raw[y * W + x] = r;
        // the sprite covers x in about [W/2 - 0.7 W/2, W/2 + 0.7 W/2]
        if (y > H / 2 - 20 && y < H / 2 + 20) { if (x > W / 2 - 60 && x < W / 2 - 10) { left += r; ++ln; } if (x > W / 2 + 10 && x < W / 2 + 60) { right += r; ++rn; } }
    }
    sys->UnlockRect();
    left /= ln; right /= rn;
    const double background = std::pow(25 / 255.0, 2.2) * 0;   // Clear writes 25/255 raw into the float target
    printf("draw hr=%08lx left(open street)=%.4f right(behind wall)=%.4f nan=%d\n", hr, left, right, nan);
    FILE* fo = nullptr; if (!fopen_s(&fo, (dir + "\\gpu_test.raw").c_str(), "wb") && fo) { fwrite(raw.data(), 4, raw.size(), fo); fclose(fo); }
    const double clear = 25 / 255.0;
    bool ok = SUCCEEDED(hr) && nan == 0 && left > clear + 0.02 && std::fabs(right - clear) < 0.005;
    (void)background;
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}
