// Offline HAL check of the soft particle path with the engine's projection
// (ViewToScreen captured from a z-feather draw: near 0.5, far ~12500).
//
// Scene: INTZ with a wall at view depth 10 m on the left half, sky (z = 1) on
// the right. The depth is copied to R32F with soft_particles_depth.cso, then
// small view-facing quads are drawn at several depths, depth test off, blend
// off, into an R32G32B32A32F target so the shader's alpha can be read back.
//
//   native_empty  engine z-feather pair, s1 empty (what the game does on PC)
//   native_fixed  engine z-feather pair, s1 = our copy, c0 = half pixel
//   soft          replacement pair on a plain particle
//   exact         replacement pair at strength 0 against the engine plain pair
//   multiply      replacement pair in multiply mode (colour towards white)
#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#pragma comment(lib, "d3d9.lib")

static const UINT W = 640, H = 360;
static const float SX = 0.63709f, SY = 1.1326f, A = 1.00004f, B = 0.50002f, WALL = 10.0f;

static std::vector<unsigned char> ReadAll(const char* p) {
    std::vector<unsigned char> v; FILE* f = std::fopen(p, "rb"); if (!f) return v;
    std::fseek(f, 0, SEEK_END); v.resize(std::ftell(f)); std::fseek(f, 0, SEEK_SET);
    if (std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear(); std::fclose(f); return v;
}
static IDirect3DDevice9* d = nullptr;
template<class T> static T* Shader(const char* path) {
    auto code = ReadAll(path);
    if (code.empty()) { std::printf("missing %s\n", path); std::exit(1); }
    T* s = nullptr; HRESULT hr;
    if constexpr (std::is_same_v<T, IDirect3DPixelShader9>) hr = d->CreatePixelShader(reinterpret_cast<const DWORD*>(code.data()), &s);
    else hr = d->CreateVertexShader(reinterpret_cast<const DWORD*>(code.data()), &s);
    if (FAILED(hr)) { std::printf("rejected %s\n", path); std::exit(1); }
    return s;
}
struct Vertex { float x, y, z; D3DCOLOR color; float u, v; };
static IDirect3DVertexDeclaration9* decl = nullptr;
// View-space quad centred on screen pixel (px, py) at view depth z.
static void Quad(float px, float py, float z, D3DCOLOR color, float halfPixels = 6.0f) {
    const float ndcX = (px + 0.0f) / W * 2.0f - 1.0f, ndcY = 1.0f - (py + 0.0f) / H * 2.0f;
    const float cx = ndcX * z / SX, cy = ndcY * z / SY;
    const float hx = halfPixels * 2.0f / W * z / SX, hy = halfPixels * 2.0f / H * z / SY;
    const Vertex q[4] = { { cx - hx, cy + hy, z, color, 0, 0 }, { cx + hx, cy + hy, z, color, 1, 0 },
                          { cx - hx, cy - hy, z, color, 0, 1 }, { cx + hx, cy - hy, z, color, 1, 1 } };
    d->SetVertexDeclaration(decl);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vertex));
}
static void ScreenQuad(float z, float x0, float x1) {
    struct V { float x, y, z, rhw, u, v; };
    const V q[4] = { { x0 - .5f, -.5f, z, 1, x0 / W, 0 }, { x1 - .5f, -.5f, z, 1, x1 / W, 0 },
                     { x0 - .5f, H - .5f, z, 1, x0 / W, 1 }, { x1 - .5f, H - .5f, z, 1, x1 / W, 1 } };
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
}

int main() {
    WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.lpszClassName = "soft_test"; RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("soft_test", "", WS_OVERLAPPEDWINDOW, 0, 0, W, H, nullptr, nullptr, nullptr, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferWidth = W; pp.BackBufferHeight = H;
    pp.hDeviceWindow = hwnd;
    if (!d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &d))) {
        std::printf("no HAL device\n"); return 1;
    }
    IDirect3DTexture9 *target = nullptr, *depth = nullptr, *copy = nullptr, *white = nullptr, *tint = nullptr;
    IDirect3DSurface9 *targetS = nullptr, *depthS = nullptr, *copyS = nullptr, *readS = nullptr;
    d->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &target, nullptr);
    d->CreateTexture(W, H, 1, D3DUSAGE_DEPTHSTENCIL, D3DFORMAT(MAKEFOURCC('I', 'N', 'T', 'Z')), D3DPOOL_DEFAULT, &depth, nullptr);
    d->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &copy, nullptr);
    d->CreateOffscreenPlainSurface(W, H, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &readS, nullptr);
    d->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &white, nullptr);
    d->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tint, nullptr);
    if (!target || !depth || !copy || !readS || !white || !tint) { std::printf("resources\n"); return 1; }
    target->GetSurfaceLevel(0, &targetS); depth->GetSurfaceLevel(0, &depthS); copy->GetSurfaceLevel(0, &copyS);
    D3DLOCKED_RECT lr;
    white->LockRect(0, &lr, nullptr, 0); *static_cast<DWORD*>(lr.pBits) = 0xFFFFFFFF; white->UnlockRect(0);
    tint->LockRect(0, &lr, nullptr, 0); *static_cast<DWORD*>(lr.pBits) = 0xC0336699; tint->UnlockRect(0);
    const D3DVERTEXELEMENT9 elements[] = { { 0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0 },
                                           { 0, 12, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0 },
                                           { 0, 16, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0 }, D3DDECL_END() };
    d->CreateVertexDeclaration(elements, &decl);

    auto featherVS = Shader<IDirect3DVertexShader9>("native\\feather_vs.cso");
    auto featherPS = Shader<IDirect3DPixelShader9>("native\\feather_ps.cso");
    auto plainVS = Shader<IDirect3DVertexShader9>("native\\plain_vs.cso");
    auto plainPS = Shader<IDirect3DPixelShader9>("native\\plain_ps.cso");
    auto softVS = Shader<IDirect3DVertexShader9>("soft_particles_vs.cso");
    auto softPS = Shader<IDirect3DPixelShader9>("soft_particles_ps.cso");
    auto copyPS = Shader<IDirect3DPixelShader9>("soft_particles_depth.cso");

    d->BeginScene();
    // 1. Scene depth: clear to the far plane, wall on the left half.
    d->SetRenderTarget(0, targetS); d->SetDepthStencilSurface(depthS);
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
    d->SetRenderState(D3DRS_ZENABLE, TRUE); d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE); d->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetVertexShader(nullptr); d->SetPixelShader(nullptr); d->SetTexture(0, nullptr);
    ScreenQuad(A - B / WALL, 0.0f, W / 2.0f);
    // 2. Copy exactly as the DLL does.
    d->SetDepthStencilSurface(nullptr); d->SetRenderTarget(0, copyS);
    d->SetRenderState(D3DRS_ZENABLE, FALSE); d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    d->SetTexture(0, depth);
    for (DWORD s = 0; s < 2; ++s) {
        d->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT); d->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        d->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        d->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); d->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
    d->SetPixelShader(copyPS);
    ScreenQuad(0.0f, 0.0f, float(W));
    d->SetTexture(0, nullptr);
    d->EndScene();

    // Constants: view space in, LocalToView = identity.
    const float l2s[16] = { SX, 0, 0, 0,  0, SY, 0, 0,  0, 0, A, -B,  0, 0, 1, 0 };
    const float ident[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
    const float feather[4] = { 1.0f, 0, 0, 0 }, half[4] = { 0.5f / W, 0.5f / H, 0, 0 };
    const float slope = -1.0f / B, offset = A / B;

    struct Probe { const char* name; float depth; bool wall; float expect; };
    const Probe probes[] = {
        { "2.0 m in front of wall", WALL - 2.0f, true, 1.0f },
        { "0.5 m in front of wall", WALL - 0.5f, true, 0.5f },
        { "0.1 m in front of wall", WALL - 0.1f, true, 0.1f },
        { "0.5 m behind wall", WALL + 0.5f, true, 0.0f },
        { "against sky at 50 m", 50.0f, false, 1.0f },
    };
    const int count = int(sizeof(probes) / sizeof(probes[0]));
    auto run = [&](int mode, std::vector<float>& alpha, std::vector<float>& red) {
        d->BeginScene();
        d->SetRenderTarget(0, targetS); d->SetDepthStencilSurface(nullptr);
        d->Clear(0, nullptr, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);
        d->SetRenderState(D3DRS_ZENABLE, FALSE); d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        d->SetTexture(0, mode >= 3 ? tint : white);
        d->SetTexture(1, mode == 0 ? nullptr : copy);
        d->SetVertexShaderConstantF(0, l2s, 4);
        if (mode <= 1) {
            d->SetVertexShaderConstantF(4, ident, 4); d->SetVertexShaderConstantF(8, l2s, 4); d->SetVertexShaderConstantF(12, feather, 1);
            d->SetPixelShaderConstantF(0, half, 1);
            d->SetVertexShader(featherVS); d->SetPixelShader(featherPS);
        } else if (mode == 5) {
            d->SetVertexShader(plainVS); d->SetPixelShader(plainPS);
        } else {
            const float zero[4] = {};
            const float ps[12] = { 1.0f / W, 1.0f / H, slope, offset,
                                   1.0f, 0.0f, mode == 3 ? 0.0f : 1.0f, 0.0f,
                                   0.0f, mode == 4 ? 1.0f : 0.0f, mode == 4 ? 0.0f : 1.0f, 0.0f };
            d->SetVertexShaderConstantF(4, zero, 1); d->SetPixelShaderConstantF(0, ps, 3);
            d->SetVertexShader(softVS); d->SetPixelShader(softPS);
        }
        for (int i = 0; i < count; ++i) {
            const float px = probes[i].wall ? 40.0f + 50.0f * i : W - 60.0f, py = H / 2.0f;
            Quad(px, py, probes[i].depth, 0xFFFFFFFF);
        }
        d->EndScene();
        d->GetRenderTargetData(targetS, readS);
        readS->LockRect(&lr, nullptr, D3DLOCK_READONLY);
        alpha.assign(count, 0); red.assign(count, 0);
        for (int i = 0; i < count; ++i) {
            const int px = probes[i].wall ? 40 + 50 * i : int(W) - 60, py = int(H / 2);
            const float* p = reinterpret_cast<const float*>(static_cast<const char*>(lr.pBits) + py * lr.Pitch) + px * 4;
            red[i] = p[0]; alpha[i] = p[3];
        }
        readS->UnlockRect();
    };
    const char* names[] = { "native_empty", "native_fixed", "soft", "exact_soft0", "multiply", "exact_native" };
    std::vector<float> a[6], r[6];
    for (int m = 0; m < 6; ++m) run(m, a[m], r[m]);

    int failures = 0;
    std::printf("probe                     ");
    for (int m = 0; m < 5; ++m) std::printf(" %13s", names[m]);
    std::printf("   expect\n");
    for (int i = 0; i < count; ++i) {
        std::printf("%-26s", probes[i].name);
        for (int m = 0; m < 5; ++m) std::printf(" %13.4f", m == 4 ? r[m][i] : a[m][i]);
        std::printf("   %.2f\n", probes[i].expect);
        // Fixed native and the replacement follow the feather within 3 % (native is linear in NDC z, not in metres).
        if (std::fabs(a[1][i] - probes[i].expect) > 0.03f) { ++failures; std::printf("  FAIL native_fixed\n"); }
        if (std::fabs(a[2][i] - probes[i].expect) > 0.01f) { ++failures; std::printf("  FAIL soft\n"); }
        // Multiply mode: red goes from the texture's red (0x33/255 = 0.2) towards 1 as the feather closes.
        const float mulExpect = 1.0f + (0.2f - 1.0f) * probes[i].expect;
        if (std::fabs(r[4][i] - mulExpect) > 0.01f) { ++failures; std::printf("  FAIL multiply (%.4f)\n", mulExpect); }
        // Strength 0 reproduces the engine's plain pair exactly.
        if (a[3][i] != a[5][i] || r[3][i] != r[5][i]) { ++failures; std::printf("  FAIL exact (%.6f %.6f vs %.6f %.6f)\n", r[3][i], a[3][i], r[5][i], a[5][i]); }
    }
    bool emptyHides = true;
    for (int i = 0; i < count; ++i) emptyHides = emptyHides && a[0][i] == 0.0f;
    std::printf("engine z-feather with empty s1 hides every probe: %s\n", emptyHides ? "yes" : "no");
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
