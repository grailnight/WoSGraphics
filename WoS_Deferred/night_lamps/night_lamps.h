#pragma once
// Night street lamps: synthetic point lights at NYC lamp bulbs.
//
// Probe 2026-10-05 (day cycle at Act1_midnight, standing under a lamp): the
// engine submits no lamp lights in free roam at any hour, only signs. So the
// renderer lights the lamps itself:
//
// 1. Observe. The chimney observer (chimney_smoke.h) already reads every native
//    draw's model box (gDecompPosScale/Bias) and world matrix; this module chains
//    into its extra-observer slot. A box from lamp_models.h is a lamp, the world
//    matrix puts its bulbs in the world. Lamps are remembered for a while, so a
//    light stays when its pole is culled.
// 2. Select. Once per frame the nearest lamps to the camera are picked.
// 3. Append. Every deferred light set gets them in its free point slots (the
//    resolve loops over up to 8 points per set), before the local-shadow
//    observer, so the two strongest can win a shadow cube and lamp shafts.
//    Every set gains the same suffix, so the light registry does not grow.
//
// Night comes from the game clock (day_cycle.h) and, for a WoSTweaks-style
// night with a frozen clock, from the sky zenith (night_reflection.h).
// Settings: [NightLamps] in clouds_menu.ini; status night_lamps_status.json.
// Include before deferred_runtime.h. No references to other modules here: the
// glue (observer chain, night inputs) lives in wos_clouds_menu.cpp.
#include "lamp_models.h"
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace WosNightLamps {

static int mode = 1;                       // 0 off, 1 at night, 2 always (testing)
static int maxLights = 6;                  // per light set, after the native ones
static float intensity = 4.0f;
static float color[3] = { 1.0f, 0.58f, 0.26f };   // sodium orange, like the native NYC lamp lights
static float rangeTall = 18.0f, rangeShort = 9.0f;  // cobra heads at 10 m, double/park heads at 4 m
static float maxDistance = 90.0f, keepSeconds = 60.0f;
// Lamp shafts (deferred_volumetric.h) are tuned for the native lamps (range 10);
// six of ours at range 18 fogged whole streets orange, so they glow less.
static float shaftScale = 0.3f;
static std::string directory;

static const char* status = "Night lamps waiting for lamps";
static float night = 0.0f;                 // 0 day .. 1 night, set by Update
static unsigned long matchedDraws = 0, rejectedWorld = 0, appendedSets = 0, fullSets = 0;

static bool (*prevWanted)() = nullptr;
static void (*prevObserver)(IDirect3DDevice9*, int, const float*, const float*) = nullptr;

struct Lamp { double p[3]; int kind; double seen; };
static std::vector<Lamp> lamps;
struct Pick { double p[3]; float invRange; float rgb[3]; };
static Pick picks[8];
static unsigned pickCount = 0;
static double origin[3] = {};
static bool originValid = false;

static double Now() {
    static LARGE_INTEGER frequency = {}, start = {};
    if (!frequency.QuadPart) { QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start); }
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return double(t.QuadPart - start.QuadPart) / double(frequency.QuadPart);
}

static bool Active() { return mode == 2 || (mode == 1 && night > 0.0f); }

static bool Close(float a, float b) { return std::fabs(a - b) <= 1e-5f * (std::max)(1.0f, std::fabs(b)); }
static int Match(const float scale[4], const float bias[4]) {
    for (int m = 0; m < int(sizeof(kLampModels) / sizeof(kLampModels[0])); ++m) {
        const LampModel& k = kLampModels[m];
        if (Close(scale[1], k.scale[1]) && Close(bias[1], k.bias[1]) && Close(scale[0], k.scale[0]) &&
            Close(scale[2], k.scale[2]) && Close(bias[0], k.bias[0]) && Close(bias[2], k.bias[2]))
            return m;
    }
    return -1;
}

static void Remember(const double p[3], int kind, double now) {
    for (auto& l : lamps) {
        const double dx = l.p[0] - p[0], dy = l.p[1] - p[1], dz = l.p[2] - p[2];
        if (dx * dx + dy * dy + dz * dz < 0.25) { l.seen = now; return; }
    }
    Lamp l; std::memcpy(l.p, p, sizeof(l.p)); l.kind = kind; l.seen = now;
    if (lamps.size() >= 512) {
        auto oldest = std::min_element(lamps.begin(), lamps.end(), [](const Lamp& a, const Lamp& b) { return a.seen < b.seen; });
        *oldest = l;
    } else {
        try { lamps.push_back(l); } catch (...) {}
    }
}

// Chimney observer chain: wanted while our lamps or the previous client want draws.
static bool Wanted() { return Active() || (prevWanted && prevWanted()); }

static void Observe(IDirect3DDevice9* d, int worldRegister, const float scale[4], const float bias[4]) {
    if (prevObserver && prevWanted && prevWanted()) prevObserver(d, worldRegister, scale, bias);
    if (!Active()) return;
    const int index = Match(scale, bias);
    if (index < 0) return;
    float w[12];
    if (FAILED(d->GetVertexShaderConstantF(worldRegister, w, 3))) return;
    ++matchedDraws;
    const float sy = std::sqrt(w[1] * w[1] + w[5] * w[5] + w[9] * w[9]);
    // Upright poles only; a lamp knocked over mid-fall is not lit.
    if (!std::isfinite(sy) || sy < 0.25f || sy > 4.0f || w[5] < 0.95f * sy) { ++rejectedWorld; return; }
    const LampModel& model = kLampModels[index];
    const double now = Now();
    for (unsigned b = 0; b < model.bulbs && b < 2; ++b) {
        const float local[3] = { model.bulb[b].x, model.bulb[b].y, model.bulb[b].z };
        double p[3];
        for (int r = 0; r < 3; ++r)
            p[r] = double(w[r * 4]) * local[0] + double(w[r * 4 + 1]) * local[1] + double(w[r * 4 + 2]) * local[2] + double(w[r * 4 + 3]);
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]) || std::fabs(p[0]) > 1e6 || std::fabs(p[2]) > 1e6) { ++rejectedWorld; return; }
        Remember(p, model.kind, now);
    }
}

// Night ramp from the clock: lamps come on 18:00-19:00 and go off 05:30-06:30.
static float ClockNight(unsigned secondsOfDay) {
    const float h = float(secondsOfDay % 86400) / 3600.0f;
    if (h >= 19.0f || h < 5.5f) return 1.0f;
    if (h >= 18.0f) return h - 18.0f;
    if (h < 6.5f) return 6.5f - h;
    return 0.0f;
}

// Once per frame (EndScene): night factor, forgetting, selection for the next frame.
static void Update(bool clockRunning, unsigned secondsOfDay, bool skyNight) {
    night = clockRunning ? ClockNight(secondsOfDay) : 0.0f;
    if (skyNight) night = 1.0f;
    pickCount = 0;
    if (!Active()) { status = mode == 0 ? "Night lamps off" : "Night lamps waiting for night"; return; }
    const double now = Now();
    lamps.erase(std::remove_if(lamps.begin(), lamps.end(), [&](const Lamp& l) { return now - l.seen > keepSeconds; }), lamps.end());
    if (!originValid) { status = "Night lamps waiting for the camera"; return; }
    struct Ranked { double d2; const Lamp* lamp; };
    Ranked best[8]; unsigned n = 0;
    const unsigned want = unsigned((std::min)((std::max)(maxLights, 0), 8));
    const double maxD2 = double(maxDistance) * maxDistance;
    for (const auto& l : lamps) {
        const double dx = l.p[0] - origin[0], dy = l.p[1] - origin[1], dz = l.p[2] - origin[2];
        const double d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > maxD2) continue;
        if (n < want) best[n++] = { d2, &l };
        else {
            unsigned worst = 0;
            for (unsigned i = 1; i < n; ++i) if (best[i].d2 > best[worst].d2) worst = i;
            if (n && d2 < best[worst].d2) best[worst] = { d2, &l };
        }
    }
    std::sort(best, best + n, [](const Ranked& a, const Ranked& b) { return a.d2 < b.d2; });
    const float gain = intensity * (mode == 2 ? 1.0f : night);
    for (unsigned i = 0; i < n; ++i) {
        Pick& p = picks[i];
        std::memcpy(p.p, best[i].lamp->p, sizeof(p.p));
        p.invRange = 1.0f / (std::max)(best[i].lamp->kind == 0 ? rangeTall : rangeShort, 0.5f);
        for (int j = 0; j < 3; ++j) p.rgb[j] = color[j] * gain;
    }
    pickCount = n;
    status = n ? "Night lamps lit" : "Night lamps: no lamp seen nearby";
}

// Per deferred draw, after the native light set is read. texels: 32 float4 rows,
// [0].w point count, [2+2i] camera-relative position + 1/range, [3+2i] RGB.
static void Append(float* texels, const double camOrigin[3]) {
    if (!Active()) return;
    std::memcpy(origin, camOrigin, sizeof(origin));
    originValid = true;
    unsigned count = unsigned(texels[3]);
    if (count >= 8) { if (pickCount) ++fullSets; return; }
    for (unsigned i = 0; i < pickCount && count < 8; ++i, ++count) {
        const Pick& p = picks[i];
        float* pos = texels + (2 + 2 * count) * 4;
        float* rgb = texels + (3 + 2 * count) * 4;
        for (int j = 0; j < 3; ++j) { pos[j] = float(p.p[j] - camOrigin[j]); rgb[j] = p.rgb[j]; }
        pos[3] = p.invRange;
        rgb[3] = 0.0f;
    }
    if (pickCount) { texels[3] = float(count); ++appendedSets; }
}

// Shaft weight for a light the local-shadow observer saw at world position p:
// shaftScale for one of ours, 1 for a native light.
static float ShaftGain(const double p[3]) {
    for (unsigned i = 0; i < pickCount; ++i) {
        const double dx = picks[i].p[0] - p[0], dy = picks[i].p[1] - p[1], dz = picks[i].p[2] - p[2];
        if (dx * dx + dy * dy + dz * dz < 0.01) return shaftScale;
    }
    return 1.0f;
}

static ULONGLONG lastStatusWrite = 0;
static void WriteStatus() {
    const ULONGLONG tick = GetTickCount64();
    if (tick - lastStatusWrite < 1000 || directory.empty()) return;
    lastStatusWrite = tick;
    FILE* f = std::fopen((directory + "\\night_lamps_status.json").c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "{\"mode\":%d,\"status\":\"%s\",\"night\":%.3f,\"known_lamps\":%u,\"lit\":%u,\"matched_draws\":%lu,"
                    "\"rejected_world\":%lu,\"appended_sets\":%lu,\"full_sets\":%lu,\"camera\":[%.1f,%.1f,%.1f],\"lights\":[",
                 mode, status, night, unsigned(lamps.size()), pickCount, matchedDraws, rejectedWorld, appendedSets, fullSets,
                 origin[0], origin[1], origin[2]);
    for (unsigned i = 0; i < pickCount; ++i)
        std::fprintf(f, "%s{\"p\":[%.2f,%.2f,%.2f],\"range\":%.1f}", i ? "," : "", picks[i].p[0], picks[i].p[1], picks[i].p[2],
                     1.0f / picks[i].invRange);
    std::fprintf(f, "]}\n");
    std::fclose(f);
}

} // namespace WosNightLamps
