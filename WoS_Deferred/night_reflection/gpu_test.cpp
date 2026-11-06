// HAL test for night_reflection.h. A managed cube built from the real day
// cubemapfromlod must be recognised and swapped for the night cube when it is
// night (mode 1 + dark zenith, or mode 2), and passed through by day, with
// mode 0, and for any other texture (the night cube itself, a 2D texture, a
// cube that differs in one byte). The swapped cube must hold the night data.
// Usage: gpu_test.exe [dir with cubemapfromlod_night.bin and extracted/]
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace WosCascadeProbe {
static std::vector<unsigned char> ReadFile(const std::string& p) {
    std::vector<unsigned char> b; FILE* f = nullptr;
    if (fopen_s(&f, p.c_str(), "rb") || !f) return b;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    b.resize(n); fread(b.data(), 1, n, f); fclose(f); return b;
}
}
#include "night_reflection.h"

static int failures = 0;
static void Check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

static IDirect3DCubeTexture9* MakeCube(IDirect3DDevice9* d, const std::vector<unsigned char>& phys) {
    IDirect3DCubeTexture9* c = nullptr;
    if (FAILED(d->CreateCubeTexture(128, 8, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, &c, nullptr))) return nullptr;
    size_t off = 0;
    for (UINT f = 0; f < 6; ++f)
        for (UINT l = 0; l < 8; ++l) {
            const UINT w = (std::max)(1u, 128u >> l), rows = (std::max)(1u, (w + 3) / 4), rb = rows * 8;
            D3DLOCKED_RECT r; c->LockRect(D3DCUBEMAP_FACES(f), l, &r, nullptr, 0);
            for (UINT y = 0; y < rows; ++y) std::memcpy((unsigned char*)r.pBits + y * r.Pitch, phys.data() + off + y * rb, rb);
            c->UnlockRect(D3DCUBEMAP_FACES(f), l); off += size_t(rb) * rows;
        }
    return c;
}

static bool CubeEquals(IDirect3DCubeTexture9* c, const std::vector<unsigned char>& phys) {
    size_t off = 0;
    for (UINT f = 0; f < 6; ++f)
        for (UINT l = 0; l < 8; ++l) {
            const UINT w = (std::max)(1u, 128u >> l), rows = (std::max)(1u, (w + 3) / 4), rb = rows * 8;
            D3DLOCKED_RECT r;
            if (FAILED(c->LockRect(D3DCUBEMAP_FACES(f), l, &r, nullptr, D3DLOCK_READONLY))) return false;
            bool same = true;
            for (UINT y = 0; y < rows && same; ++y) same = !std::memcmp((unsigned char*)r.pBits + y * r.Pitch, phys.data() + off + y * rb, rb);
            c->UnlockRect(D3DCUBEMAP_FACES(f), l); off += size_t(rb) * rows;
            if (!same) return false;
        }
    return true;
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    HWND wnd = CreateWindowA("STATIC", "t", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = wnd;
    pp.BackBufferWidth = 64; pp.BackBufferHeight = 64; pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    IDirect3DDevice9* d = nullptr;
    if (FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &d))) { std::printf("FAIL device\n"); return 1; }

    const std::string dayPath = dir + "\\extracted\\GAME\\_0x003A261A.Game.pcapk\\0x812D2DD0.cubemapfromlod.wrap.tex";
    auto dayFile = WosCascadeProbe::ReadFile(dayPath);
    auto night = WosCascadeProbe::ReadFile(dir + "\\cubemapfromlod_night.bin");
    if (dayFile.size() < 65616 || night.size() != 65616) { std::printf("FAIL inputs (run extract_cubes.py)\n"); return 1; }
    std::vector<unsigned char> day(dayFile.end() - 65616, dayFile.end());
    std::vector<unsigned char> almost = day; almost[5 * 10936 + 31] ^= 1;   // last signature byte of face 5

    IDirect3DCubeTexture9* dayCube = MakeCube(d, day);
    IDirect3DCubeTexture9* nightCopy = MakeCube(d, night);
    IDirect3DCubeTexture9* almostCube = MakeCube(d, almost);
    IDirect3DTexture9* flat = nullptr; d->CreateTexture(128, 128, 8, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, &flat, nullptr);
    if (!dayCube || !nightCopy || !almostCube || !flat) { std::printf("FAIL resources\n"); return 1; }

    using namespace WosNightReflection;
    directory = dir;
    const float bright[3] = { 0.30f, 0.45f, 0.80f }, dark[3] = { 0.01f, 0.015f, 0.03f }, dusk[3] = { 0.07f, 0.08f, 0.09f };

    mode = 1; UpdateSky(true, bright);
    Check(Substitute(d, dayCube) == dayCube, "day: day cube passes through");
    Check(dayCube == static_cast<IDirect3DCubeTexture9*>(WosNightReflection::dayCube), "day cube recognised by signature");
    UpdateSky(true, dark);
    IDirect3DBaseTexture9* got = Substitute(d, dayCube);
    Check(got != dayCube && got == nightCube, "night: day cube swapped for the night cube");
    Check(nightCube && CubeEquals(nightCube, night), "night cube holds cubemapfromlod_night data, all faces and mips");
    Check(Substitute(d, nightCopy) == nightCopy, "night: other cube (night copy) passes through");
    Check(Substitute(d, almostCube) == almostCube, "night: cube differing in one signature byte passes through");
    Check(Substitute(d, flat) == flat, "night: 2D texture passes through");
    Check(Substitute(d, nullptr) == nullptr, "null passes through");
    UpdateSky(true, dusk);
    Check(Substitute(d, dayCube) == nightCube, "hysteresis: dusk (between thresholds) stays night");
    UpdateSky(false, bright);
    Check(Substitute(d, dayCube) == nightCube, "no sky draw this frame: decision held");
    UpdateSky(true, bright);
    Check(Substitute(d, dayCube) == dayCube, "bright zenith: back to day");
    mode = 2;
    Check(Substitute(d, dayCube) == nightCube, "mode 2: always night");
    mode = 0; UpdateSky(true, dark);
    Check(Substitute(d, dayCube) == dayCube, "mode 0: off");
    Check(lockFailures == 0, "no lock failures on managed cubes");

    // Device change: Reset releases our cube, the next night swap rebuilds it.
    mode = 1; Reset(); UpdateSky(true, dark);
    got = Substitute(d, dayCube);
    Check(got == nightCube && nightCube && CubeEquals(nightCube, night), "after Reset the night cube is rebuilt");

    WriteStatus();
    Reset();
    dayCube->Release(); nightCopy->Release(); almostCube->Release(); flat->Release();
    d->Release(); d3d->Release();
    std::printf(failures ? "RESULT FAIL (%d)\n" : "RESULT PASS\n", failures);
    return failures ? 1 : 0;
}
