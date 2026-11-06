#pragma once
// Night reflections: Spider-Man 3's night reflection cube, switched on at night.
//
// Building windows (facade shaders with propertySampler), car paint and glass
// sample ReflectionSampler, which the engine binds to cubemapfromlod: a 128x128
// DXT1 cube of a sunny Manhattan street. SM3 swaps it for cubemapfromlod_night
// (lit windows, moon) at night. WoS still ships that night cube in MEGACITY,
// byte-identical to SM3's, but its exe no longer knows the name, so at night
// every window reflects a daytime sky.
//
// 1. Recognise the day cube when it is bound: a cube texture, 128x128 DXT1,
//    8 levels, whose mip 0 face data starts with kDaySignature. Read once per
//    texture pointer through a read-only lock (the engine's textures are managed).
// 2. Decide day/night from the native sky palette (zenith colour of the
//    rvb_skybox draw), with hysteresis so dusk does not flicker.
// 3. At night pass our own copy of the night cube (cubemapfromlod_night.bin,
//    extracted from WoS's own MEGACITY by extract_cubes.py) to SetTexture.
//
// Settings: [NightReflection] in clouds_menu.ini. Mode 0 = off, 1 = auto,
// 2 = always night (for checking). Include after the d3d9 headers.
#include "day_signature.h"
#include <unordered_map>

namespace WosNightReflection {

static int mode = 1;
static float nightZenith = 0.06f;     // zenith luminance below which it is night
static float dayZenith = 0.10f;       // and above which it is day again
static std::string directory;

static const char* status = "Night reflections waiting for the day cube";
static bool night = false, faulted = false;
static float zenithLuminance = -1.0f;
static unsigned long swaps = 0, checked = 0, lockFailures = 0;

static IDirect3DDevice9* owner = nullptr;
static IDirect3DCubeTexture9* nightCube = nullptr;
static IDirect3DBaseTexture9* dayCube = nullptr;
static bool nightTried = false;
// Every texture pointer seen bound, with whether it is the day cube. Textures
// the engine frees can come back under the same pointer, so a hit is checked
// again (type and level count) before every swap.
static std::unordered_map<IDirect3DBaseTexture9*, bool> seen;

static const UINT kWidth = 128, kLevels = 8;

static void Reset() {
    if (nightCube) { nightCube->Release(); nightCube = nullptr; }
    owner = nullptr; dayCube = nullptr; nightTried = false; faulted = false;
    seen.clear();
}

static UINT LevelBytes(UINT level) {
    const UINT w = (std::max)(1u, kWidth >> level);
    return (std::max)(1u, (w + 3) / 4) * (std::max)(1u, (w + 3) / 4) * 8;
}

static bool IsCubeShape(IDirect3DBaseTexture9* t) {
    if (t->GetType() != D3DRTYPE_CUBETEXTURE || t->GetLevelCount() != kLevels) return false;
    D3DSURFACE_DESC desc;
    auto* cube = static_cast<IDirect3DCubeTexture9*>(t);
    return SUCCEEDED(cube->GetLevelDesc(0, &desc)) && desc.Format == D3DFMT_DXT1 &&
           desc.Width == kWidth && desc.Height == kWidth;
}

static bool IsDayCube(IDirect3DBaseTexture9* t) {
    if (!IsCubeShape(t)) return false;
    ++checked;
    auto* cube = static_cast<IDirect3DCubeTexture9*>(t);
    for (UINT face = 0; face < 6; ++face) {
        D3DLOCKED_RECT r;
        if (FAILED(cube->LockRect(D3DCUBEMAP_FACES(face), 0, &r, nullptr, D3DLOCK_READONLY))) { ++lockFailures; return false; }
        // DXT rows are 4 pixel rows of blocks; the signature fits in the first row.
        const bool same = std::memcmp(r.pBits, kDaySignature[face], kSignatureBytes) == 0;
        cube->UnlockRect(D3DCUBEMAP_FACES(face), 0);
        if (!same) return false;
    }
    return true;
}

static bool LoadNight(IDirect3DDevice9* d) {
    if (owner && owner != d) Reset();
    if (nightCube) return true;
    if (nightTried) return false;
    nightTried = true;
    auto bytes = WosCascadeProbe::ReadFile(directory + "\\cubemapfromlod_night.bin");
    UINT chain = 0;
    for (UINT level = 0; level < kLevels; ++level) chain += LevelBytes(level);
    if (bytes.size() != size_t(chain) * 6) {
        faulted = true; status = "Night reflections: cubemapfromlod_night.bin missing or wrong size";
        return false;
    }
    if (FAILED(d->CreateCubeTexture(kWidth, kLevels, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, &nightCube, nullptr))) {
        nightCube = nullptr; faulted = true; status = "Night reflections: CreateCubeTexture failed";
        return false;
    }
    size_t offset = 0;
    for (UINT face = 0; face < 6; ++face) {
        for (UINT level = 0; level < kLevels; ++level) {
            const UINT w = (std::max)(1u, kWidth >> level);
            const UINT rows = (std::max)(1u, (w + 3) / 4), rowBytes = (std::max)(1u, (w + 3) / 4) * 8;
            D3DLOCKED_RECT r;
            if (FAILED(nightCube->LockRect(D3DCUBEMAP_FACES(face), level, &r, nullptr, 0))) {
                nightCube->Release(); nightCube = nullptr; faulted = true; status = "Night reflections: cube lock failed";
                return false;
            }
            for (UINT y = 0; y < rows; ++y)
                std::memcpy(static_cast<unsigned char*>(r.pBits) + size_t(y) * r.Pitch, bytes.data() + offset + size_t(y) * rowBytes, rowBytes);
            nightCube->UnlockRect(D3DCUBEMAP_FACES(face), level);
            offset += size_t(rowBytes) * rows;
        }
    }
    owner = d;
    return true;
}

// Called with the frame's sky palette; zenith is the rvb_skybox gZenithColor.
static void UpdateSky(bool valid, const float zenith[3]) {
    if (!valid) return;   // held: frames without a sky draw keep the last decision
    zenithLuminance = 0.2126f * zenith[0] + 0.7152f * zenith[1] + 0.0722f * zenith[2];
    if (night && zenithLuminance > dayZenith) night = false;
    else if (!night && zenithLuminance < nightZenith) night = true;
}

static bool WantNight() { return mode == 2 || (mode == 1 && night); }

// From the SetTexture hook: the texture to bind instead of t.
static IDirect3DBaseTexture9* Substitute(IDirect3DDevice9* d, IDirect3DBaseTexture9* t) {
    if (!t || mode == 0 || faulted) return t;
    bool isDay;
    const auto found = seen.find(t);
    if (found == seen.end()) {
        isDay = IsDayCube(t);
        seen.emplace(t, isDay);
        if (isDay) { dayCube = t; status = "Night reflections: day cube found"; }
    } else {
        isDay = found->second;
    }
    if (!isDay || !WantNight()) return t;
    if (t->GetType() != D3DRTYPE_CUBETEXTURE || t->GetLevelCount() != kLevels) {
        seen.erase(t);   // the pointer was reused by another texture
        return t;
    }
    if (!LoadNight(d)) return t;
    ++swaps;
    return nightCube;
}

static ULONGLONG lastStatusWrite = 0;
static void WriteStatus() {
    const ULONGLONG tick = GetTickCount64();
    if (tick - lastStatusWrite < 1000 || directory.empty()) return;
    lastStatusWrite = tick;
    FILE* f = std::fopen((directory + "\\night_reflection_status.json").c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "{\"mode\":%d,\"faulted\":%s,\"status\":\"%s\",\"night\":%s,\"zenith_luminance\":%.4f,"
                    "\"night_below\":%.4f,\"day_above\":%.4f,\"day_cube_found\":%s,\"night_cube_loaded\":%s,"
                    "\"swaps\":%lu,\"cubes_checked\":%lu,\"lock_failures\":%lu,\"textures_seen\":%u}\n",
                 mode, faulted ? "true" : "false", status, night ? "true" : "false", zenithLuminance,
                 nightZenith, dayZenith, dayCube ? "true" : "false", nightCube ? "true" : "false",
                 swaps, checked, lockFailures, unsigned(seen.size()));
    std::fclose(f);
}

} // namespace WosNightReflection
