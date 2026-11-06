// Offline HAL check of chimney_smoke.cso with the exact states the DLL uses.
// Scene: sky-coloured HDR target, INTZ depth with a near wall covering the
// lower left, a house plume behind the wall edge and a smokestack plume far
// away. Writes gpu_test.raw (RGBA16F) and gpu_test.json.
#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#pragma comment(lib, "d3d9.lib")

static const UINT W = 960, H = 540;
static std::vector<unsigned char> ReadAll(const char* p) {
    std::vector<unsigned char> v; FILE* f = std::fopen(p, "rb"); if (!f) return v;
    std::fseek(f, 0, SEEK_END); v.resize(std::ftell(f)); std::fseek(f, 0, SEEK_SET);
    if (std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear(); std::fclose(f); return v;
}
static bool Invert(const double m[16], double out[16]) {
    double a[4][8];
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r);
    for (int c = 0; c < 4; ++c) {
        int p = c; for (int r = c + 1; r < 4; ++r) if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (std::fabs(a[p][c]) < 1e-15) return false;
        for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[p][k]);
        double d = a[c][c]; for (int k = 0; k < 8; ++k) a[c][k] /= d;
        for (int r = 0; r < 4; ++r) if (r != c) { double f = a[r][c]; for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k]; }
    }
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) out[r * 4 + c] = a[r][c + 4];
    return true;
}
struct Plume { float base[3], radius, drift[3], height, box[2][3], kind; };

int main(int argc, char** argv) {
    const char* cso = argc > 1 ? argv[1] : "chimney_smoke.cso";
    auto code = ReadAll(cso);
    if (code.empty()) { std::printf("{\"error\":\"no shader\"}\n"); return 1; }
    WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.lpszClassName = "smoke_test"; RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("smoke_test", "", WS_OVERLAPPEDWINDOW, 0, 0, W, H, nullptr, nullptr, nullptr, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferWidth = W; pp.BackBufferHeight = H;
    pp.BackBufferFormat = D3DFMT_UNKNOWN; pp.hDeviceWindow = hwnd;
    IDirect3DDevice9* d = nullptr;
    if (!d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &d))) {
        std::printf("{\"error\":\"device\"}\n"); return 1;
    }
    IDirect3DTexture9 *hdr = nullptr, *depth = nullptr; IDirect3DSurface9 *hdrS = nullptr, *depthS = nullptr;
    d->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &hdr, nullptr);
    d->CreateTexture(W, H, 1, D3DUSAGE_DEPTHSTENCIL, D3DFORMAT(MAKEFOURCC('I', 'N', 'T', 'Z')), D3DPOOL_DEFAULT, &depth, nullptr);
    if (!hdr || !depth) { std::printf("{\"error\":\"targets\"}\n"); return 1; }
    hdr->GetSurfaceLevel(0, &hdrS); depth->GetSurfaceLevel(0, &depthS);
    IDirect3DPixelShader9* ps = nullptr;
    if (FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(code.data()), &ps))) { std::printf("{\"error\":\"shader\"}\n"); return 1; }

    // Camera-relative projection * view: eye at the origin, looking down +z, pitched up 8 degrees.
    const double n = 0.5, f = 5000.0, fovY = 60.0 * 3.14159265 / 180.0, aspect = double(W) / H;
    const double ys = 1.0 / std::tan(fovY / 2), xs = ys / aspect, pitch = 8.0 * 3.14159265 / 180.0;
    const double cp = std::cos(pitch), sp = std::sin(pitch);
    // view rows: right, up, forward (world y up, forward tilted up)
    const double right[3] = { 1, 0, 0 }, up[3] = { 0, cp, -sp }, fwd[3] = { 0, sp, cp };
    double vp[16] = {};
    for (int c = 0; c < 3; ++c) {
        vp[0 * 4 + c] = xs * right[c];
        vp[1 * 4 + c] = ys * up[c];
        vp[2 * 4 + c] = f / (f - n) * fwd[c];
        vp[3 * 4 + c] = fwd[c];
    }
    vp[2 * 4 + 3] = -n * f / (f - n);
    double inv[16]; Invert(vp, inv);
    float relativeInverse[16]; for (int i = 0; i < 16; ++i) relativeInverse[i] = float(inv[i]);

    // Scene: sky clear, then a wall 30 m away on the lower left (depth only).
    d->SetRenderTarget(0, hdrS); d->SetDepthStencilSurface(depthS);
    d->BeginScene();
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    struct V { float x, y, z, rhw; DWORD c; };
    auto fill = [&](float x0, float y0, float x1, float y1, float z, DWORD col) {
        const V q[] = { { x0, y0, z, 1, col }, { x1, y0, z, 1, col }, { x0, y1, z, 1, col }, { x1, y1, z, 1, col } };
        d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE); d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
    };
    d->SetRenderState(D3DRS_ZENABLE, TRUE); d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE); d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    // fp16 clear colour cannot exceed 1 via D3DCOLOR; sky and wall colours are LDR here.
    fill(0, 0, float(W), float(H), 1.0f, D3DCOLOR_XRGB(120, 160, 215));
    const float wallZ = float(f / (f - n) * (1.0 - n / 30.0));
    fill(0, float(H) * 0.62f, float(W) * 0.47f, float(H), wallZ, D3DCOLOR_XRGB(90, 70, 60));
    d->EndScene();

    // Smoke pass, same states as WosChimneySmoke::BloomDraw.
    d->SetDepthStencilSurface(nullptr);
    d->SetRenderTarget(0, hdrS);
    d->BeginScene();
    d->SetVertexShader(nullptr); d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    for (auto s : { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE,
                    D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE, D3DRS_CLIPPLANEENABLE, D3DRS_SEPARATEALPHABLENDENABLE })
        d->SetRenderState(s, 0);
    const bool debugAlpha = argc > 2 && argv[2][0] != '-';
    d->SetRenderState(D3DRS_COLORWRITEENABLE, debugAlpha ? 15 : 7);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, debugAlpha ? FALSE : TRUE); d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE); d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCALPHA);
    d->SetTexture(0, depth);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT); d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d->SetPixelShader(ps);
    d->SetPixelShaderConstantF(0, relativeInverse, 4);

    const float sunDir[3] = { 0.55f, 0.6f, -0.58f };
    const float sl = std::sqrt(sunDir[0] * sunDir[0] + sunDir[1] * sunDir[1] + sunDir[2] * sunDir[2]);
    Plume plumes[2] = {};
    auto setup = [](Plume& p, float x, float y, float z, float r, float height, float grow, float driftScale) {
        p.base[0] = x; p.base[1] = y; p.base[2] = z; p.radius = r; p.height = height;
        p.drift[0] = 0.45f * 0.5f * driftScale; p.drift[2] = 0.45f * 0.866f * driftScale;
        const float top = r + grow * height, rx = p.drift[0] * height, rz = p.drift[2] * height;
        p.box[0][0] = x + (std::min)(0.f, rx) - top; p.box[1][0] = x + (std::max)(0.f, rx) + top;
        p.box[0][1] = y - 0.3f; p.box[1][1] = y + height;
        p.box[0][2] = z + (std::min)(0.f, rz) - top; p.box[1][2] = z + (std::max)(0.f, rz) + top;
    };
    const bool closeUp = argc > 3; setup(plumes[0], closeUp ? 60.f : 400.f, closeUp ? -40.f : -60.f, closeUp ? 320.f : 1500.f, 3.4f, 140.f, 0.16f * 0.9f, 0.9f); plumes[0].kind = 1;
    setup(plumes[1], -4.f, -3.f, 40.f, 0.45f, 16.f, 0.16f, 1.0f); plumes[1].kind = 0;
    int drawn = 0;
    for (auto& p : plumes) {
        if (debugAlpha && argv[2][0] - '0' != int(&p - plumes)) continue;
        const bool stack = p.kind > 0.5f;
        const float c[10][4] = {
            { p.base[0], p.base[1], p.base[2], p.radius },
            { p.drift[0], 0, p.drift[2], p.height },
            { 1.6f * (stack ? 2.5f : 1.f), 12.3f, stack ? 0.16f * 0.9f : 0.16f, stack ? 0.55f : 1.0f },
            { sunDir[0] / sl, sunDir[1] / sl, sunDir[2] / sl, 1 },
            { 3.0f, 2.8f, 2.5f, 1.0f },
            { 0.25f, 0.32f, 0.40f, 24 },
            { stack ? 0.78f : 0.52f, stack ? 0.78f : 0.53f, stack ? 0.80f : 0.56f, stack ? 0.37f : 0.81f },
            { p.box[0][0], p.box[0][1], p.box[0][2], 0 },
            { p.box[1][0], p.box[1][1], p.box[1][2], 0 },
            { 1.0f, 0.9f, stack ? 3.0f : 0.6f, 0.35f },
        };
        d->SetPixelShaderConstantF(4, &c[0][0], 10);
        // full-screen rect keeps the test independent of the CPU rectangle code
        struct T { float x, y, z, rhw, u, v; };
        const T q[] = { { -.5f, -.5f, 0, 1, 0, 0 }, { W - .5f, -.5f, 0, 1, 1, 0 }, { -.5f, H - .5f, 0, 1, 0, 1 }, { W - .5f, H - .5f, 0, 1, 1, 1 } };
        if (SUCCEEDED(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(T)))) ++drawn;
    }
    d->EndScene();

    IDirect3DSurface9* sys = nullptr;
    d->CreateOffscreenPlainSurface(W, H, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    HRESULT hr = d->GetRenderTargetData(hdrS, sys);
    D3DLOCKED_RECT lr = {};
    if (FAILED(hr) || FAILED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) { std::printf("{\"error\":\"readback\"}\n"); return 1; }
    FILE* out = std::fopen("gpu_test.raw", "wb");
    for (UINT y = 0; y < H; ++y) std::fwrite(static_cast<unsigned char*>(lr.pBits) + y * lr.Pitch, 8, W, out);
    std::fclose(out);
    sys->UnlockRect();
    std::printf("{\"drawn\":%d,\"width\":%u,\"height\":%u,\"wall_z\":%.6f}\n", drawn, W, H, wallZ);
    return 0;
}
