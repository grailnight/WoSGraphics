// GPU proof for native TAA (taa_vs_patch.h, taa_runtime.h, taa_resolve/present).
//
// A. Real shaders. Every unique vertex shader from the F7 captures is patched;
//    the HAL must accept each twin, and the twin must equal the original token
//    for token except the redirected position writes and the 8 appended tokens.
// B. Exactness. Synthetic vs_2_0/vs_3_0 shaders (position on o2 with four
//    partial writes, relative bone addressing) are drawn through the original
//    and the twin. With zero jitter the colour is bit-identical and a second
//    draw with ZFUNC EQUAL covers every pixel, which is what the game's material
//    replay does. With one pixel of jitter the image moves by exactly one pixel.
// C. End to end. The runtime itself (JitterScope, Resolve) runs a small frame
//    loop: jittered scene into 16F with INTZ, a copy to an 8-bit display image,
//    resolve. Results are compared against an 8x8 supersampled reference:
//    static edges must get much closer to it than the aliased frame, a flat
//    image must come through unchanged, a panning camera must stay close to
//    the reference thanks to reprojection, and a moving object must not leave
//    a lasting ghost.
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "taa_runtime.h"

static unsigned g_checks = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
#define EXPECT(x) do { ++g_checks; if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static const char* kRoot = "WoS_Deferred\\taa_native\\";

static std::vector<unsigned char> ReadFile(const std::string& path) {
    std::vector<unsigned char> bytes;
    FILE* f = std::fopen(path.c_str(), "rb"); if (!f) return bytes;
    std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::rewind(f);
    if (n > 0 && n < (1 << 22)) { bytes.resize(size_t(n)); if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear(); }
    std::fclose(f); return bytes;
}

static HRESULT STDMETHODCALLTYPE HostSetVS(IDirect3DDevice9* d, IDirect3DVertexShader9* s) { return d->SetVertexShader(s); }
static HRESULT STDMETHODCALLTYPE HostSetPS(IDirect3DDevice9* d, IDirect3DPixelShader9* s) { return d->SetPixelShader(s); }
static HRESULT STDMETHODCALLTYPE HostCreateVS(IDirect3DDevice9* d, const DWORD* c, IDirect3DVertexShader9** s) { return d->CreateVertexShader(c, s); }
static HRESULT STDMETHODCALLTYPE HostDrawUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT n, const void* v, UINT s) { return d->DrawPrimitiveUP(t, n, v, s); }

// ------------------------------------------------------------------ geometry
struct Vertex { float x, y, z, u, v; };
static IDirect3DDevice9* g_d = nullptr;
static IDirect3DVertexDeclaration9* g_decl = nullptr;

struct Matrix { double m[16]; };
static Matrix Multiply(const Matrix& a, const Matrix& b) {
    Matrix r = {};
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { double s = 0; for (int k = 0; k < 4; ++k) s += a.m[i * 4 + k] * b.m[k * 4 + j]; r.m[i * 4 + j] = s; }
    return r;
}
// Row-vector perspective, D3D left-handed, z in [0,1].
static Matrix Projection(double aspect) {
    const double f = 1.0 / std::tan(30.0 * 3.14159265358979 / 180.0), zNear = 0.5, zFar = 100.0;
    Matrix p = {}; p.m[0] = f / aspect; p.m[5] = f; p.m[10] = zFar / (zFar - zNear); p.m[11] = 1; p.m[14] = -zNear * zFar / (zFar - zNear);
    return p;
}
static Matrix Translation(double x, double y, double z) {
    Matrix t = {}; t.m[0] = t.m[5] = t.m[10] = t.m[15] = 1; t.m[12] = x; t.m[13] = y; t.m[14] = z; return t;
}
struct Camera { float viewProj[16]; float relativeInverse[16]; double origin[3]; };
// Camera at (cx, 0, 0) looking down +z. The runtime wants the inverse of the
// camera-relative VP laid out as rows that dot with clip space.
static bool MakeCamera(double cx, Camera& out) {
    const Matrix projection = Projection(1.0);
    const Matrix vp = Multiply(Translation(-cx, 0, 0), projection);
    for (int i = 0; i < 16; ++i) out.viewProj[i] = float(vp.m[i]);
    double column[16], inverse[16];
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) column[r * 4 + c] = projection.m[c * 4 + r];
    if (!WosTaa::Inverse(column, inverse)) return false;
    for (int i = 0; i < 16; ++i) out.relativeInverse[i] = float(inverse[i]);
    out.origin[0] = cx; out.origin[1] = 0; out.origin[2] = 0;
    return true;
}

struct Target {
    IDirect3DTexture9* texture = nullptr; IDirect3DSurface9* surface = nullptr; UINT w = 0, h = 0; D3DFORMAT format = D3DFMT_UNKNOWN;
    bool Create(UINT width, UINT height, D3DFORMAT f, DWORD usage = D3DUSAGE_RENDERTARGET) {
        w = width; h = height; format = f;
        return SUCCEEDED(g_d->CreateTexture(w, h, 1, usage, f, D3DPOOL_DEFAULT, &texture, nullptr)) && SUCCEEDED(texture->GetSurfaceLevel(0, &surface));
    }
    void Free() { WosTaa::Release(surface); WosTaa::Release(texture); }
};

static bool ReadColor(const Target& t, std::vector<float>& out) {
    IDirect3DSurface9* cpu = nullptr;
    bool ok = SUCCEEDED(g_d->CreateOffscreenPlainSurface(t.w, t.h, t.format, D3DPOOL_SYSTEMMEM, &cpu, nullptr)) && SUCCEEDED(g_d->GetRenderTargetData(t.surface, cpu));
    D3DLOCKED_RECT rect = {};
    if (ok && SUCCEEDED(cpu->LockRect(&rect, nullptr, D3DLOCK_READONLY))) {
        out.assign(size_t(t.w) * t.h * 4, 0.0f);
        for (UINT y = 0; y < t.h; ++y) {
            const char* row = static_cast<const char*>(rect.pBits) + size_t(rect.Pitch) * y;
            for (UINT x = 0; x < t.w; ++x) {
                float* o = &out[(size_t(y) * t.w + x) * 4];
                if (t.format == D3DFMT_A32B32G32R32F) std::memcpy(o, row + x * 16, 16);
                else if (t.format == D3DFMT_A8R8G8B8) { const unsigned char* p = reinterpret_cast<const unsigned char*>(row + x * 4); o[0] = p[2] / 255.0f; o[1] = p[1] / 255.0f; o[2] = p[0] / 255.0f; o[3] = p[3] / 255.0f; }
                else ok = false;
            }
        }
        cpu->UnlockRect();
    } else ok = false;
    WosTaa::Release(cpu);
    return ok;
}

static void Basics() {
    for (auto s : { D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE, D3DRS_LIGHTING })
        g_d->SetRenderState(s, 0);
    g_d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_d->SetRenderState(D3DRS_COLORWRITEENABLE, 15);
}

// Draws triangles through vs (optionally inside a JitterScope) with ps.
static bool DrawMesh(IDirect3DVertexShader9* vs, IDirect3DPixelShader9* ps, const std::vector<Vertex>& tris, const float viewProj[16],
                     const float color[4], const float checker[4], const std::function<HRESULT(const std::function<HRESULT()>&)>& wrap) {
    Basics();
    bool ok = SUCCEEDED(g_d->SetVertexDeclaration(g_decl)) && SUCCEEDED(g_d->SetVertexShader(vs)) && SUCCEEDED(g_d->SetPixelShader(ps));
    // fxc packs float4x4 column-major: register rows are the matrix columns.
    float packed[16];
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) packed[r * 4 + c] = viewProj[c * 4 + r];
    ok = ok && SUCCEEDED(g_d->SetVertexShaderConstantF(0, packed, 4));
    float bones[8 * 3 * 4] = {};
    for (int b = 0; b < 8; ++b) { bones[b * 12 + 0] = bones[b * 12 + 5] = bones[b * 12 + 10] = 1; }
    ok = ok && SUCCEEDED(g_d->SetVertexShaderConstantF(10, bones, 24));
    const int iterations[4] = { 2, 0, 1, 0 };
    ok = ok && SUCCEEDED(g_d->SetVertexShaderConstantI(0, iterations, 1));
    ok = ok && SUCCEEDED(g_d->SetPixelShaderConstantF(0, color, 1)) && SUCCEEDED(g_d->SetPixelShaderConstantF(1, checker, 1));
    if (!ok) return false;
    auto draw = [&]() { return g_d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, UINT(tris.size() / 3), tris.data(), sizeof(Vertex)); };
    return SUCCEEDED(wrap ? wrap(draw) : draw());
}

static void Quad(std::vector<Vertex>& tris, double cx, double cy, double z, double half, double angle) {
    const double c = std::cos(angle), s = std::sin(angle);
    auto corner = [&](double x, double y, float u, float v) { return Vertex{ float(cx + x * c - y * s), float(cy + x * s + y * c), float(z), u, v }; };
    Vertex a = corner(-half, -half, 0, 0), b = corner(half, -half, 1, 0), d = corner(-half, half, 0, 1), e = corner(half, half, 1, 1);
    tris.insert(tris.end(), { a, b, d, b, e, d });
}

static bool Copy(IDirect3DPixelShader9* copy, const Target& from, const Target& to) {
    struct V { float x, y, z, rhw, u, v; };
    const float w = float(to.w), h = float(to.h);
    const V q[] = { {-.5f,-.5f,0,1,0,0}, {w - .5f,-.5f,0,1,1,0}, {-.5f,h - .5f,0,1,0,1}, {w - .5f,h - .5f,0,1,1,1} };
    Basics();
    bool ok = SUCCEEDED(g_d->SetRenderTarget(0, to.surface)) && SUCCEEDED(g_d->SetDepthStencilSurface(nullptr));
    D3DVIEWPORT9 vp = { 0, 0, to.w, to.h, 0, 1 };
    ok = ok && SUCCEEDED(g_d->SetViewport(&vp)) && SUCCEEDED(g_d->SetVertexShader(nullptr)) && SUCCEEDED(g_d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1));
    ok = ok && SUCCEEDED(g_d->SetRenderState(D3DRS_ZENABLE, FALSE)) && SUCCEEDED(g_d->SetPixelShader(copy)) && SUCCEEDED(g_d->SetTexture(0, from.texture));
    for (auto st : { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER }) g_d->SetSamplerState(0, st, D3DTEXF_POINT);
    ok = ok && SUCCEEDED(g_d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V)));
    g_d->SetTexture(0, nullptr);
    return ok;
}

static double MeanAbs(const std::vector<float>& a, const std::vector<float>& b, const std::vector<unsigned>& pixels) {
    double s = 0; for (unsigned p : pixels) for (int c = 0; c < 3; ++c) s += std::fabs(a[p * 4 + c] - b[p * 4 + c]);
    return pixels.empty() ? 0 : s / (pixels.size() * 3.0);
}

int main() {
    HWND window = CreateWindowExW(0, L"STATIC", L"WoS TAA test", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    CHECK(window);
    IDirect3D9* api = Direct3DCreate9(D3D_SDK_VERSION); CHECK(api);
    D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window;
    pp.BackBufferWidth = 320; pp.BackBufferHeight = 240; pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    CHECK(SUCCEEDED(api->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_d)));
    WosTaa::host.setVS = HostSetVS; WosTaa::host.setPS = HostSetPS; WosTaa::host.createVS = HostCreateVS; WosTaa::host.drawUP = HostDrawUP;
    D3DCAPS9 caps = {}; CHECK(SUCCEEDED(g_d->GetDeviceCaps(&caps)));
    const UINT maxConst = std::min<UINT>(caps.MaxVertexShaderConst, 256);
    std::printf("device max VS constants %lu (using %u)\n", caps.MaxVertexShaderConst, maxConst);
    CHECK(g_d->BeginScene() == D3D_OK);

    // ================================================== A. real game shaders
    unsigned corpus = 0, patched = 0, accepted = 0, subroutines = 0;
    for (unsigned index = 0;; ++index) {
        char name[64]; std::snprintf(name, sizeof(name), "test_corpus\\vs_%03u.cso", index);
        auto bytes = ReadFile(std::string(kRoot) + name);
        if (bytes.empty()) break;
        ++corpus;
        CHECK(bytes.size() % 4 == 0);
        const DWORD* code = reinterpret_cast<const DWORD*>(bytes.data());
        const size_t words = bytes.size() / 4;
        IDirect3DVertexShader9* original = nullptr;
        EXPECT(SUCCEEDED(g_d->CreateVertexShader(code, &original)));
        WosTaaPatch::Result result; std::string why;
        if (!WosTaaPatch::Patch(code, words, maxConst, result, why)) { std::printf("corpus %u rejected: %s\n", index, why.c_str()); original->Release(); continue; }
        ++patched;
        if (result.subroutines) ++subroutines;
        EXPECT(result.code.size() == words + 8);
        EXPECT(result.jitterRegister == maxConst - 1);
        // Token for token: identical except redirected destinations, plus the
        // eight appended tokens (mul, add) at the insertion point.
        size_t differing = 0;
        EXPECT(result.insertAt > 0 && result.insertAt < words);
        for (size_t i = 0; i < words; ++i) {
            const DWORD have = result.code[i < result.insertAt ? i : i + 8], want = code[i];
            if (have == want) continue;
            const bool redirect = WosTaaPatch::RegType(have) == WosTaaPatch::kRegTemp && WosTaaPatch::RegNum(have) == result.positionTemp &&
                                  (have & ~(0x70001FFFu)) == (want & ~(0x70001FFFu));
            EXPECT(redirect);
            ++differing;
        }
        EXPECT((result.code[result.insertAt] & 0xFFFF) == WosTaaPatch::kOpMul && (result.code[result.insertAt + 4] & 0xFFFF) == WosTaaPatch::kOpAdd);
        EXPECT(differing == result.redirectedWrites);
        IDirect3DVertexShader9* twin = nullptr;
        if (SUCCEEDED(g_d->CreateVertexShader(result.code.data(), &twin))) { ++accepted; twin->Release(); }
        else {
            std::printf("corpus %u: HAL rejected the twin\n", index);
        }
        original->Release();
    }
    std::printf("corpus: %u shaders, %u patched, %u accepted by HAL, %u with subroutines\n", corpus, patched, accepted, subroutines);
    EXPECT(corpus >= 40);
    EXPECT(accepted == patched);
    EXPECT(patched * 10 >= corpus * 9);

    // ======================================================= B. exactness
    const UINT kB = 97;
    Target exactColor, exactDepth;
    CHECK(exactColor.Create(kB, kB, D3DFMT_A32B32G32R32F));
    CHECK(SUCCEEDED(g_d->CreateTexture(kB, kB, 1, D3DUSAGE_DEPTHSTENCIL, D3DFORMAT(MAKEFOURCC('I', 'N', 'T', 'Z')), D3DPOOL_DEFAULT, &exactDepth.texture, nullptr)));
    CHECK(SUCCEEDED(exactDepth.texture->GetSurfaceLevel(0, &exactDepth.surface)));
    const D3DVERTEXELEMENT9 elements[] = { {0,0,D3DDECLTYPE_FLOAT3,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0}, {0,12,D3DDECLTYPE_FLOAT2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,0}, D3DDECL_END() };
    CHECK(SUCCEEDED(g_d->CreateVertexDeclaration(elements, &g_decl)));
    auto sceneBytes = ReadFile(std::string(kRoot) + "test_shaders\\ps_scene.cso"), copyBytes = ReadFile(std::string(kRoot) + "test_shaders\\ps_copy.cso");
    IDirect3DPixelShader9* scenePS = nullptr, * copyPS = nullptr;
    CHECK(SUCCEEDED(g_d->CreatePixelShader(reinterpret_cast<const DWORD*>(sceneBytes.data()), &scenePS)));
    CHECK(SUCCEEDED(g_d->CreatePixelShader(reinterpret_cast<const DWORD*>(copyBytes.data()), &copyPS)));

    Camera still; CHECK(MakeCamera(0, still));
    std::vector<Vertex> slanted;
    Quad(slanted, 0.1, -0.05, 4.0, 1.1, 0.29);
    Quad(slanted, -0.9, 0.7, 6.0, 0.6, -0.61);
    const float white[4] = { 1, 0.6f, 0.2f, 1 }, noChecker[4] = { 0, 0, 0, 0 }, red[4] = { 1, 0, 0, 1 };
    unsigned exactShaders = 0;
    for (const char* file : { "vs_basic3.cso", "vs_basic2.cso", "vs_skin3.cso", "vs_skin2.cso" }) {
        auto bytes = ReadFile(std::string(kRoot) + "test_shaders\\" + file);
        CHECK(!bytes.empty());
        IDirect3DVertexShader9* original = nullptr, * twin = nullptr;
        CHECK(SUCCEEDED(g_d->CreateVertexShader(reinterpret_cast<const DWORD*>(bytes.data()), &original)));
        WosTaaPatch::Result result; std::string why;
        EXPECT(WosTaaPatch::Patch(reinterpret_cast<const DWORD*>(bytes.data()), bytes.size() / 4, maxConst, result, why));
        CHECK(SUCCEEDED(g_d->CreateVertexShader(result.code.data(), &twin)));
        auto render = [&](IDirect3DVertexShader9* vs, float jitterX, std::vector<float>& out, bool replay) {
            const float jitter[4] = { jitterX, 0, 0, 0 };
            bool ok = SUCCEEDED(g_d->SetRenderTarget(0, exactColor.surface)) && SUCCEEDED(g_d->SetDepthStencilSurface(exactDepth.surface));
            D3DVIEWPORT9 vp = { 0, 0, kB, kB, 0, 1 }; ok = ok && SUCCEEDED(g_d->SetViewport(&vp));
            ok = ok && SUCCEEDED(g_d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f, 0));
            ok = ok && SUCCEEDED(g_d->SetRenderState(D3DRS_ZENABLE, TRUE)) && SUCCEEDED(g_d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE)) && SUCCEEDED(g_d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL));
            ok = ok && SUCCEEDED(g_d->SetVertexShaderConstantF(result.jitterRegister, jitter, 1));
            ok = ok && DrawMesh(vs == twin && replay ? original : vs, scenePS, slanted, still.viewProj, white, noChecker, nullptr);
            if (replay) {
                // The material replay: same geometry again, ZFUNC EQUAL, no depth write.
                ok = ok && SUCCEEDED(g_d->SetRenderState(D3DRS_ZFUNC, D3DCMP_EQUAL)) && SUCCEEDED(g_d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE));
                ok = ok && DrawMesh(twin, scenePS, slanted, still.viewProj, red, noChecker, nullptr);
                g_d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL); g_d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
            }
            return ok && ReadColor(exactColor, out);
        };
        std::vector<float> a, b, shifted, replay;
        EXPECT(render(original, 0, a, false));
        EXPECT(render(twin, 0, b, false));
        EXPECT(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
        unsigned covered = 0, replaced = 0;
        EXPECT(render(twin, 0, replay, true));
        for (UINT p = 0; p < kB * kB; ++p) if (a[p * 4 + 3] > 0.5f) { ++covered; if (replay[p * 4] == 1.0f && replay[p * 4 + 1] == 0.0f) ++replaced; }
        std::printf("replay %s: covered %u, replaced %u\n", file, covered, replaced);
        EXPECT(covered > kB * kB / 8 && replaced == covered);
        // One pixel right: clip +2/w.
        EXPECT(render(twin, 2.0f / kB, shifted, false));
        unsigned mismatched = 0, compared = 0;
        for (UINT y = 0; y < kB; ++y) for (UINT x = 1; x < kB; ++x) {
            ++compared;
            if (std::memcmp(&shifted[(y * kB + x) * 4 + 3], &a[(y * kB + x - 1) * 4 + 3], sizeof(float))) ++mismatched;
        }
        std::printf("%s: redirected %u writes, jitter c%u, temps r%u/r%u, shift mismatches %u of %u\n", file, result.redirectedWrites, result.jitterRegister, result.positionTemp, result.scratchTemp, mismatched, compared);
        EXPECT(mismatched == 0);
        ++exactShaders;
        twin->Release(); original->Release();
    }
    EXPECT(exactShaders == 4);

    // ======================================================== C. end to end
    const UINT kW = 96, kH = 96, kSS = 8;
    Target scene, display, reference;
    IDirect3DTexture9* sceneDepth = nullptr; IDirect3DSurface9* sceneDepthSurface = nullptr, * referenceDepth = nullptr;
    CHECK(scene.Create(kW, kH, D3DFMT_A16B16G16R16F));
    CHECK(display.Create(kW, kH, D3DFMT_A8R8G8B8));
    CHECK(reference.Create(kW * kSS, kH * kSS, D3DFMT_A32B32G32R32F));
    CHECK(SUCCEEDED(g_d->CreateTexture(kW, kH, 1, D3DUSAGE_DEPTHSTENCIL, D3DFORMAT(MAKEFOURCC('I', 'N', 'T', 'Z')), D3DPOOL_DEFAULT, &sceneDepth, nullptr)));
    CHECK(SUCCEEDED(sceneDepth->GetSurfaceLevel(0, &sceneDepthSurface)));
    CHECK(SUCCEEDED(g_d->CreateDepthStencilSurface(kW * kSS, kH * kSS, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &referenceDepth, nullptr)));
    CHECK(WosTaa::LoadShaders(std::string(kRoot)));
    auto vsBytes = ReadFile(std::string(kRoot) + "test_shaders\\vs_basic3.cso");
    IDirect3DVertexShader9* sceneVS = nullptr;
    CHECK(SUCCEEDED(g_d->CreateVertexShader(reinterpret_cast<const DWORD*>(vsBytes.data()), &sceneVS)));

    // One frame of the game: scene with jitter, composite, resolve.
    auto frame = [&](const Camera& cam, const std::vector<Vertex>& tris, const float checker[4], bool resolve, std::vector<float>& out) {
        bool ok = SUCCEEDED(g_d->SetRenderTarget(0, scene.surface)) && SUCCEEDED(g_d->SetDepthStencilSurface(sceneDepthSurface));
        D3DVIEWPORT9 vp = { 0, 0, kW, kH, 0, 1 }; ok = ok && SUCCEEDED(g_d->SetViewport(&vp));
        ok = ok && SUCCEEDED(g_d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(20, 30, 40), 1.0f, 0));
        ok = ok && SUCCEEDED(g_d->SetRenderState(D3DRS_ZENABLE, TRUE)) && SUCCEEDED(g_d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE)) && SUCCEEDED(g_d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL));
        ok = ok && DrawMesh(sceneVS, scenePS, tris, cam.viewProj, white, checker, [&](const std::function<HRESULT()>& draw) {
            WosTaa::JitterScope jitter(g_d, true);
            return jitter.Draw(draw);
        });
        ok = ok && Copy(copyPS, scene, display);
        if (ok && resolve) ok = WosTaa::Resolve(g_d, sceneDepth, cam.relativeInverse, cam.origin);
        ok = ok && ReadColor(display, out);
        WosTaa::FrameEnd();
        return ok;
    };
    auto supersampled = [&](const Camera& cam, const std::vector<Vertex>& tris, const float checker[4], std::vector<float>& out) {
        bool ok = SUCCEEDED(g_d->SetRenderTarget(0, reference.surface)) && SUCCEEDED(g_d->SetDepthStencilSurface(referenceDepth));
        D3DVIEWPORT9 vp = { 0, 0, kW * kSS, kH * kSS, 0, 1 }; ok = ok && SUCCEEDED(g_d->SetViewport(&vp));
        ok = ok && SUCCEEDED(g_d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(20, 30, 40), 1.0f, 0));
        ok = ok && SUCCEEDED(g_d->SetRenderState(D3DRS_ZENABLE, TRUE)) && SUCCEEDED(g_d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE));
        ok = ok && DrawMesh(sceneVS, scenePS, tris, cam.viewProj, white, checker, nullptr);
        std::vector<float> big; ok = ok && ReadColor(reference, big);
        // D3D9 samples a pixel at its integer coordinate. Big pixel X samples at
        // X, and its box is [X-0.5, X+0.5); small pixel x samples at x/kSS, so
        // the samples inside that box are x = kSS*X - kSS/2 ... kSS*X + kSS/2 - 1.
        out.assign(kW * kH * 4, 0.0f);
        std::vector<float> weight(kW * kH, 0.0f);
        for (UINT y = 0; y < kH * kSS && ok; ++y) for (UINT x = 0; x < kW * kSS; ++x) {
            const UINT X = (x + kSS / 2) / kSS, Y = (y + kSS / 2) / kSS;
            if (X >= kW || Y >= kH) continue;
            weight[Y * kW + X] += 1.0f;
            for (int c = 0; c < 4; ++c) out[(Y * kW + X) * 4 + c] += std::min(std::max(big[(y * kW * kSS + x) * 4 + c], 0.0f), 1.0f);
        }
        for (UINT p = 0; p < kW * kH; ++p) for (int c = 0; c < 4; ++c) out[p * 4 + c] /= std::max(weight[p], 1.0f);
        return ok;
    };
    auto resetTaa = [&]() {
        WosTaa::Reset(); WosTaa::historyValid = false; WosTaa::lastWidth = WosTaa::lastHeight = 0;
        WosTaa::frame = WosTaa::FrameJitter{}; WosTaa::sequence = 0;
    };

    // C1. Static edges.
    resetTaa();
    WosTaa::sharpen = 0.0f;
    std::vector<float> aliased, ssRef, taa;
    EXPECT(frame(still, slanted, noChecker, false, aliased));
    WosTaa::FrameEnd();
    EXPECT(!WosTaa::frame.active); // nothing resolved yet: no jitter
    EXPECT(supersampled(still, slanted, noChecker, ssRef));
    std::vector<unsigned> edges, interior;
    for (UINT y = 1; y + 1 < kH; ++y) for (UINT x = 1; x + 1 < kW; ++x) {
        const unsigned p = y * kW + x;
        const double luma = ssRef[p * 4] + ssRef[p * 4 + 1] + ssRef[p * 4 + 2];
        bool flat = true;
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
            const unsigned q = (y + dy) * kW + (x + dx);
            if (std::fabs(ssRef[q * 4] + ssRef[q * 4 + 1] + ssRef[q * 4 + 2] - luma) > 1e-4) flat = false;
        }
        if (std::fabs(aliased[p * 4] - ssRef[p * 4]) > 0.02 || std::fabs(aliased[p * 4 + 1] - ssRef[p * 4 + 1]) > 0.02) edges.push_back(p);
        else if (flat) interior.push_back(p);
    }
    // C0. Without jitter a still image must come through the resolve unchanged,
    // every pixel of the 8-bit output. Two clip bugs (a box excluding the
    // centre, a degenerate chroma axis) each dimmed bright edge pixels here.
    {
        WosTaa::jitterScale = 0.0f;
        std::vector<float> still8;
        unsigned changed = 0;
        for (int i = 0; i < 24; ++i) EXPECT(frame(still, slanted, noChecker, true, still8));
        for (size_t i = 0; i < still8.size(); ++i) if (i % 4 != 3 && std::fabs(still8[i] - aliased[i]) > 0.25f / 255.0f) ++changed;
        std::printf("still without jitter: %u channels changed\n", changed);
        EXPECT(changed == 0);
        EXPECT(WosTaa::lastJitteredDraws == 0);
        WosTaa::jitterScale = 1.0f;
        resetTaa();
        std::vector<float> discard;
        EXPECT(frame(still, slanted, noChecker, false, discard));
    }
    unsigned jitteredFrames = 0;
    for (int i = 0; i < 32; ++i) {
        EXPECT(frame(still, slanted, noChecker, true, taa));
        if (WosTaa::lastJitteredDraws) ++jitteredFrames;
    }
    const double aliasedError = MeanAbs(aliased, ssRef, edges), taaError = MeanAbs(taa, ssRef, edges);
    unsigned interiorChanged = 0;
    for (unsigned p : interior) for (int c = 0; c < 3; ++c) if (std::fabs(taa[p * 4 + c] - aliased[p * 4 + c]) > 0.5f / 255.0f) { ++interiorChanged; break; }
    std::printf("static: %zu edge pixels, aliased error %.4f, TAA error %.4f; %zu interior pixels, %u changed; %u jittered frames, draws %lu\n",
                edges.size(), aliasedError, taaError, interior.size(), interiorChanged, jitteredFrames, WosTaa::lastJitteredDraws);
    EXPECT(edges.size() > 100);
    EXPECT(jitteredFrames >= 30);
    EXPECT(taaError < aliasedError * 0.5);
    EXPECT(interiorChanged == 0);
    EXPECT(WosTaa::patchedShaders >= 1 && WosTaa::failures == 0);

    // C2. Sharpening keeps flat areas untouched. Sharpening moves edges away
    // from a box-filtered reference by design, so the bound is looser: the
    // default still keeps most of the gain, the maximum stays better than none.
    std::vector<float> sharp;
    for (float amount : { 0.2f, 1.0f }) {
        WosTaa::sharpen = amount;
        EXPECT(frame(still, slanted, noChecker, true, sharp));
        unsigned sharpInterior = 0;
        for (unsigned p : interior) for (int c = 0; c < 3; ++c) if (std::fabs(sharp[p * 4 + c] - aliased[p * 4 + c]) > 0.5f / 255.0f) { ++sharpInterior; break; }
        EXPECT(sharpInterior == 0);
        const double sharpError = MeanAbs(sharp, ssRef, edges);
        std::printf("sharpen %.1f: edge error %.4f\n", amount, sharpError);
        EXPECT(sharpError < aliasedError * (amount < 0.5f ? 0.5 : 0.75));
    }
    WosTaa::sharpen = 0.0f;

    // C3. Panning camera over a checker plane, with and without reprojection.
    const float checker[4] = { 1, 24, 0, 0 };
    std::vector<Vertex> plane;
    Quad(plane, 0, 0, 5.0, 4.0, 0.0);
    auto pan = [&](bool reproject, double& error) {
        resetTaa();
        error = 0; unsigned measured = 0;
        std::vector<float> out, ref;
        for (int i = 0; i < 24; ++i) {
            Camera cam; if (!MakeCamera(0.012 * i, cam)) return false;
            if (!reproject && i > 0) { std::memcpy(WosTaa::prevRelativeInverse, cam.relativeInverse, 64); for (int k = 0; k < 3; ++k) WosTaa::prevOrigin[k] = cam.origin[k]; }
            if (!frame(cam, plane, checker, true, out)) return false;
            if (i >= 12) {
                if (!supersampled(cam, plane, checker, ref)) return false;
                std::vector<unsigned> all;
                for (UINT y = 4; y + 4 < kH; ++y) for (UINT x = 4; x + 4 < kW; ++x) all.push_back(y * kW + x);
                error += MeanAbs(out, ref, all); ++measured;
            }
        }
        error /= measured; return true;
    };
    double withReprojection = 0, without = 0;
    EXPECT(pan(true, withReprojection));
    EXPECT(pan(false, without));
    std::vector<float> panAliased, panRef;
    resetTaa();
    { Camera cam; CHECK(MakeCamera(0.012 * 20, cam)); EXPECT(frame(cam, plane, checker, false, panAliased)); EXPECT(supersampled(cam, plane, checker, panRef)); }
    std::vector<unsigned> all;
    for (UINT y = 4; y + 4 < kH; ++y) for (UINT x = 4; x + 4 < kW; ++x) all.push_back(y * kW + x);
    const double panAliasedError = MeanAbs(panAliased, panRef, all);
    std::printf("pan: aliased %.4f, TAA reprojected %.4f, TAA without reprojection %.4f\n", panAliasedError, withReprojection, without);
    EXPECT(withReprojection < without);
    EXPECT(withReprojection < panAliasedError);

    // C4. A moving object with no motion vectors: clipping must remove the trail.
    resetTaa();
    std::vector<float> out, ref;
    double worstTrail = 0;
    for (int i = 0; i < 28; ++i) {
        std::vector<Vertex> mover;
        Quad(mover, -1.2 + 0.09 * i, 0.0, 4.0, 0.35, 0.2);
        EXPECT(frame(still, mover, noChecker, true, out));
        if (i >= 8) {
            EXPECT(supersampled(still, mover, noChecker, ref));
            // Background pixels well clear of the object now: any colour here is a ghost.
            for (UINT y = 2; y + 2 < kH; ++y) for (UINT x = 2; x + 2 < kW; ++x) {
                bool clear = true;
                for (int dy = -2; dy <= 2 && clear; ++dy) for (int dx = -2; dx <= 2; ++dx) {
                    const unsigned q = (y + dy) * kW + (x + dx);
                    if (std::fabs(ref[q * 4] - ref[(2 * kW + 2) * 4]) > 1e-3) { clear = false; break; }
                }
                if (!clear) continue;
                const unsigned p = y * kW + x;
                for (int c = 0; c < 3; ++c) worstTrail = std::max(worstTrail, double(std::fabs(out[p * 4 + c] - ref[p * 4 + c])));
            }
        }
    }
    std::printf("moving object: worst ghost on clear background %.4f\n", worstTrail);
    EXPECT(worstTrail < 0.06);

    // C5. An unsupported target is left alone.
    {
        resetTaa();
        std::vector<float> before, after;
        EXPECT(frame(still, slanted, noChecker, false, before));
        CHECK(SUCCEEDED(g_d->SetRenderTarget(0, display.surface)));
        D3DVIEWPORT9 vp = { 1, 0, kW - 1, kH, 0, 1 }; CHECK(SUCCEEDED(g_d->SetViewport(&vp)));
        const unsigned long skippedBefore = WosTaa::skipped;
        EXPECT(!WosTaa::Resolve(g_d, sceneDepth, still.relativeInverse, still.origin));
        EXPECT(WosTaa::skipped == skippedBefore + 1);
        EXPECT(ReadColor(display, after));
        EXPECT(std::memcmp(before.data(), after.data(), before.size() * sizeof(float)) == 0);
        WosTaa::FrameEnd();
        EXPECT(!WosTaa::frame.active);
    }

    g_d->EndScene();
    std::printf("TAA GPU checks passed: %u checks, corpus %u/%u patched\n", g_checks, patched, corpus);
    WosTaa::ReleaseTwins(); WosTaa::r.Clear();
    return 0;
}
