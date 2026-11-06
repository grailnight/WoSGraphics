#pragma once
// Day/night cycle: restores the native game clock tick that WoS shipped without.
//
// The engine already does time of day from the clock: FUN_0097E820 maps the hour
// to Act1_dawn/noon/sunset/midnight (Act3_* in act 3), the state machine
// FUN_00986520 streams that sky pack on change, the environment evaluator
// FUN_007D43F0 samples its 24 h tracks at clock/86400, entity "active hours"
// windows, night shader branches and the vehicle headlight collector 0x590D30 all
// read the same clock. Only the tick is dead: 0x0097F340 accumulates
// rate(+0xC8) * dt into +0xC4, and its per-second loop at 0x0097F390 fires the
// minute/hour script events and wraps at 86400 but never increments +0xBC.
// Spider-Man 3 has the same loop with `add [esi+0xDC],1` (rate 10, a day in 2.4 h).
//
// Patch (two edits, both checked against the original bytes):
//   0x0097F389  lea esp,[esp] (7-byte filler right before the loop head)
//               -> inc dword ptr [esi+0xBC]; nop
//   0x0097F4D8  jae 0x0097F390 (loop back edge) -> jae 0x0097F389
// so every whole accumulated second increments the clock once, then runs the
// native minute/hour events and wrap. Pause and menus stay native (the stopwatch).
//
// Settings: [DayCycle] in clouds_menu.ini. Enabled 0/1, Rate = game seconds per
// real second (SM3 = 10). Status: day_cycle_status.json (clock, TOD name, swap
// state, native point light count) - the probe for "do lamps light at night".
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace WosDayCycle {

static bool enabled = true;
static int rate = 10;
static std::string directory;

static const char* status = "Day cycle waiting for the game object";
static bool patched = false, patchFailed = false;
static int originalRate = -1;

static const uintptr_t kGameObject = 0x01111760;   // game/world object pointer
static const uintptr_t kPointLightCount = 0x0111A9D0;
static const uintptr_t kIncAt = 0x0097F389, kJaeAt = 0x0097F4D8;
static const unsigned char kIncOriginal[7] = {0x8D, 0xA4, 0x24, 0x00, 0x00, 0x00, 0x00};
static const unsigned char kIncPatched[7] = {0xFF, 0x86, 0xBC, 0x00, 0x00, 0x00, 0x90};
// jae rel32: target - (kJaeAt + 6)
static const unsigned char kJaeOriginal[6] = {0x0F, 0x83, 0xB2, 0xFE, 0xFF, 0xFF};   // -> 0x0097F390
static const unsigned char kJaePatched[6] = {0x0F, 0x83, 0xAB, 0xFE, 0xFF, 0xFF};    // -> 0x0097F389

static bool WriteCode(uintptr_t at, const unsigned char* bytes, size_t n) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), n, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(reinterpret_cast<void*>(at), bytes, n);
    VirtualProtect(reinterpret_cast<void*>(at), n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), n);
    return true;
}

static bool Matches(uintptr_t at, const unsigned char* bytes, size_t n) {
    return std::memcmp(reinterpret_cast<const void*>(at), bytes, n) == 0;
}

static void SetPatched(bool on) {
    if (on == patched || (on && patchFailed)) return;
    const unsigned char* incFrom = on ? kIncOriginal : kIncPatched;
    const unsigned char* jaeFrom = on ? kJaeOriginal : kJaePatched;
    if (!Matches(kIncAt, incFrom, 7) || !Matches(kJaeAt, jaeFrom, 6)) {
        patchFailed = true;
        status = "Day cycle: clock tick bytes differ from the expected EXE, patch skipped";
        return;
    }
    // Back edge first when enabling, last when disabling: the inc is a harmless
    // filler replacement either way, the jump is what makes the loop reach it.
    if (on) { WriteCode(kIncAt, kIncPatched, 7); WriteCode(kJaeAt, kJaePatched, 6); }
    else { WriteCode(kJaeAt, kJaeOriginal, 6); WriteCode(kIncAt, kIncOriginal, 7); }
    patched = on;
}

// Raw reads of game memory; no C++ objects here so __try is allowed.
static uintptr_t GameObject() {
    __try { return *reinterpret_cast<uintptr_t*>(kGameObject); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static bool ReadClock(uintptr_t obj, unsigned* seconds, int* clockRate, int* swapState, int* act, char* tod, size_t todSize) {
    __try {
        *seconds = *reinterpret_cast<unsigned*>(obj + 0xBC);
        *clockRate = *reinterpret_cast<int*>(obj + 0xC8);
        *act = *reinterpret_cast<int*>(obj + 0x25C);
        *swapState = *reinterpret_cast<int*>(obj + 0x290);
        tod[0] = 0;
        const char* name = *reinterpret_cast<const char**>(obj + 0x27C);
        if (name) {
            size_t i = 0;
            for (; i + 1 < todSize && name[i] >= 0x20 && name[i] < 0x7F; ++i) tod[i] = name[i];
            tod[i] = 0;
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool WriteInt(uintptr_t at, int value) {
    __try { *reinterpret_cast<int*>(at) = value; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static int PointLightCount() {
    __try { return *reinterpret_cast<int*>(kPointLightCount); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

static unsigned seconds = 0;
static int clockRate = 0, swapState = 0, act = 0;
static char tod[32] = {};
static bool clockValid = false;
static int pendingSetSeconds = -1;   // menu request, applied on the next frame

// ------------------------------------------------------------- sun and moon
// The direct light the engine uploads per draw is a sun object in the scene's
// render environment (current_sun_decode.h): scene = *0x01115C54, slot table at
// scene+0x4D8, slot id *0x010F853C, environment+0x32C -> sun, colour sun+0x40,
// direction sun+0x50. Probe 2026-10-05: the direction never changes with the
// clock (only the colour, at each TOD pack swap). So the direction is turned
// here every frame: the native direction is kept as the noon key, the sun rises
// in the plane through it at 06:00 and sets at 18:00, and at night the same arc
// carries the moon (the engine's night "sun" is moonlight). Native shading,
// cascades, the resolve and the bloom sun all read this one vector.
static bool moveSun = true;
static float minElevation = 6.0f;          // degrees; keeps shadows sane at the horizon
static const char* sunStatus = "Sun path waiting";
// The current scene (*0x01115C54) is set only while the engine draws, so the
// sun objects are collected from the draw hooks (ObserveDraw) and turned at
// EndScene. A frame can render several scenes; each sun keeps its own base.
struct SunSlot { uint32_t object = 0; float base[3] = {}, written[3] = {}; bool writtenValid = false; unsigned lastFrame = 0; };
static SunSlot sunSlots[4];
static unsigned sunFrame = 1;
static uint32_t lastScene = 0;
static uint32_t sunObject = 0;               // first slot, for the status
static float sunBase[3] = {}, sunNow[3] = {};
static unsigned long sunOverwrites = 0, sunFinds = 0;
static unsigned sunCount = 0;
static float clockFraction = 0.0f;

static uint32_t FindSun() {
    __try {
        const uint32_t scene = *reinterpret_cast<uint32_t*>(0x01115C54);
        if (scene < 0x10000) return 0;
        const uint32_t table = *reinterpret_cast<uint32_t*>(scene + 0x4D8);
        const uint32_t id = *reinterpret_cast<uint32_t*>(0x010F853C);
        if (table < 0x10000 || id >= 64) return 0;
        const uint32_t environment = *reinterpret_cast<uint32_t*>(table + 8 + 4 * id);
        if (environment < 0x10000) return 0;
        const uint32_t sun = *reinterpret_cast<uint32_t*>(environment + 0x32C);
        return sun >= 0x10000 && (sun & 3) == 0 ? sun : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static bool ReadVec3(uint32_t at, float out[3]) {
    __try { std::memcpy(out, reinterpret_cast<const void*>(at), 12); return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool WriteVec3(uint32_t at, const float v[3]) {
    __try { std::memcpy(reinterpret_cast<void*>(at), v, 12); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool ReadFraction(uintptr_t obj, float* out) {
    __try { *out = *reinterpret_cast<float*>(obj + 0xC4); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool Same(const float a[3], const float b[3]) {
    return std::fabs(a[0] - b[0]) < 1e-6f && std::fabs(a[1] - b[1]) < 1e-6f && std::fabs(a[2] - b[2]) < 1e-6f;
}

// Direction along the arc for a day-time hour; `base` is the native (noon) vector.
static void SunArc(const float base[3], float hours, float out[3]) {
    // Work in the "toward the light" sense; the stored vector may point either way.
    const float sign = base[1] >= 0.0f ? 1.0f : -1.0f;
    float n[3] = { base[0] * sign, base[1] * sign, base[2] * sign };
    float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len < 1e-6f) { std::memcpy(out, base, 12); return; }
    for (float& v : n) v /= len;
    // East: horizontal and perpendicular to the noon azimuth.
    float hx = n[0], hz = n[2], hl = std::sqrt(hx * hx + hz * hz);
    if (hl < 1e-4f) { hx = 0.0f; hz = 1.0f; hl = 1.0f; }
    const float east[3] = { hz / hl, 0.0f, -hx / hl };
    // Day hours 6..18 (night hours are shifted by 12 for the moon) -> 0..pi, noon = pi/2 = n.
    float h = std::fmod(hours, 24.0f); if (h < 0) h += 24.0f;
    if (h < 6.0f || h >= 18.0f) h = std::fmod(h + 12.0f, 24.0f);
    const float theta = (h - 6.0f) / 12.0f * 3.14159265f;
    float d[3];
    for (int i = 0; i < 3; ++i) d[i] = std::cos(theta) * east[i] + std::sin(theta) * n[i];
    const float minY = std::sin(minElevation * 3.14159265f / 180.0f);
    if (d[1] < minY) {
        const float hz2 = std::sqrt((std::max)(d[0] * d[0] + d[2] * d[2], 1e-8f)), k = std::sqrt(1.0f - minY * minY) / hz2;
        d[0] *= k; d[2] *= k; d[1] = minY;
    }
    for (int i = 0; i < 3; ++i) out[i] = d[i] * len * sign;
}

static uint32_t CurrentScene() {
    __try { return *reinterpret_cast<uint32_t*>(0x01115C54); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Every native draw. The sun pointer is only ever dereferenced right after it
// was resolved through the live scene -> environment chain in this same draw:
// a TOD pack swap frees and rebuilds the environment, and the 2026-10-05 build
// that wrote remembered pointers at EndScene crashed (driver AV, heap) right
// after the 18:00 swap. Nothing is written while a swap is in progress.
// Slots keep only per-object bookkeeping; their pointers are compared, never
// followed. Cheap: one read per draw unless the scene changed.
static float sunHours = 12.0f;             // set by Update from the clock
static unsigned sunCountThisFrame = 0;

static SunSlot& SlotFor(uint32_t sun) {
    for (auto& s : sunSlots) if (s.object == sun) return s;
    SunSlot* slot = &sunSlots[0];
    for (auto& s : sunSlots) if (s.lastFrame < slot->lastFrame) slot = &s;
    *slot = SunSlot();
    slot->object = sun;
    return *slot;
}

static void ObserveDraw() {
    const bool on = enabled && moveSun;
    bool anyWritten = false;
    for (const auto& s : sunSlots) anyWritten |= s.writtenValid;
    if (!on && !anyWritten) return;
    const uint32_t scene = CurrentScene();
    if (scene == lastScene) return;
    lastScene = scene;
    if (scene < 0x10000 || !clockValid || swapState != 0) return;
    const uint32_t sun = FindSun();
    if (!sun) return;
    ++sunFinds;
    SunSlot& slot = SlotFor(sun);
    float cur[3];
    if (!ReadVec3(sun + 0x50, cur)) { slot = SunSlot(); return; }
    // Anything but our own last write is the engine's value: the new noon key.
    if (!slot.writtenValid || !Same(cur, slot.written)) {
        if (slot.writtenValid) ++sunOverwrites;
        std::memcpy(slot.base, cur, 12);
        slot.writtenValid = false;
    }
    if (!on) {
        // Switched off: hand the native direction back, then forget the object.
        if (slot.writtenValid) WriteVec3(sun + 0x50, slot.base);
        slot = SunSlot();
        return;
    }
    float now[3];
    SunArc(slot.base, sunHours, now);
    if (WriteVec3(sun + 0x50, now)) { std::memcpy(slot.written, now, 12); slot.writtenValid = true; }
    slot.lastFrame = sunFrame;
    if (!sunCountThisFrame) { sunObject = sun; std::memcpy(sunBase, slot.base, 12); std::memcpy(sunNow, now, 12); }
    ++sunCountThisFrame;
}

// ------------------------------------------------------------ shadow camera
// The sun light turns with sun+0x50, but shadows did not move (2026-10-05):
// cascades take their orientation from the native orthographic shadow camera,
// which FUN_007D3D30 places at camera + dir * distance, dir coming from an
// environment track sampled by FUN_007C0640 (cdecl: out float4*, keys, count, t).
// Both of its calls there (0x007D3ED1, 0x007D419D) are redirected here: the
// track value is taken as the noon key and turned on the same arc as the sun.
typedef void* (__cdecl* TrackFn)(float* out, void* keys, int count, float t);
static const TrackFn kTrackSample = reinterpret_cast<TrackFn>(0x007C0640);
static const uintptr_t kShadowCalls[2] = { 0x007D3ED1, 0x007D419D };
static const unsigned char kShadowCallOriginal[2][5] = { {0xE8, 0x6A, 0xC7, 0xFE, 0xFF}, {0xE8, 0x9E, 0xC4, 0xFE, 0xFF} };
static bool shadowPatched = false, shadowPatchFailed = false;
static unsigned long shadowCalls = 0;
static float shadowBase[3] = {}, shadowNow[3] = {};

static void* __cdecl ShadowTrackHook(float* out, void* keys, int count, float t) {
    void* result = kTrackSample(out, keys, count, t);
    ++shadowCalls;
    if (enabled && moveSun && clockValid && out && std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2])) {
        const float base[3] = { out[0], out[1], out[2] };
        SunArc(base, sunHours, out);
        std::memcpy(shadowBase, base, 12);
        std::memcpy(shadowNow, out, 12);
    }
    return result;
}

static void SetShadowPatched(bool on) {
    if (on == shadowPatched || (on && shadowPatchFailed)) return;
    unsigned char patchedBytes[2][5];
    for (int i = 0; i < 2; ++i) {
        patchedBytes[i][0] = 0xE8;
        const int32_t rel = int32_t(reinterpret_cast<uintptr_t>(&ShadowTrackHook) - (kShadowCalls[i] + 5));
        std::memcpy(patchedBytes[i] + 1, &rel, 4);
    }
    for (int i = 0; i < 2; ++i)
        if (!Matches(kShadowCalls[i], on ? kShadowCallOriginal[i] : patchedBytes[i], 5)) {
            shadowPatchFailed = true;
            return;
        }
    for (int i = 0; i < 2; ++i) WriteCode(kShadowCalls[i], on ? patchedBytes[i] : kShadowCallOriginal[i], 5);
    shadowPatched = on;
}

// Toward-the-sun (or moon) unit vector for the cascade orientation
// (WosCascadeProbe::sunToward). False while the sun path is off or unseen.
static bool SunToward(double out[3]) {
    if (!enabled || !moveSun || !sunCount) return false;
    const double l = std::sqrt(double(sunNow[0]) * sunNow[0] + double(sunNow[1]) * sunNow[1] + double(sunNow[2]) * sunNow[2]);
    if (!(l > 1e-6) || !std::isfinite(l)) return false;
    const double s = sunNow[1] >= 0.0f ? 1.0 / l : -1.0 / l;
    for (int i = 0; i < 3; ++i) out[i] = sunNow[i] * s;
    return true;
}

// Once per frame (EndScene): clock for the next frame's draws, status. No writes.
static void MoveSun() {
    const uintptr_t obj = GameObject();
    float fraction = 0.0f;
    if (obj && ReadFraction(obj, &fraction) && fraction >= 0.0f && fraction < 1.0f) clockFraction = fraction;
    sunHours = (float(seconds % 86400) + clockFraction) / 3600.0f;
    sunCount = sunCountThisFrame;
    sunCountThisFrame = 0;
    SetShadowPatched(false);   // that track is vertical and is not the shadow direction (see cascade_probe.h OrientToSun)
    if (!enabled || !moveSun) sunStatus = "Sun path off";
    else if (swapState != 0) sunStatus = "Sun path paused: TOD pack swap";
    else sunStatus = sunCount ? "Sun path moving" : "Sun path: no sun object seen in draws";
    lastScene = 0;   // resolve again next frame
    ++sunFrame;
}

// Called once per frame (EndScene).
static void Update() {
    SetPatched(enabled);
    const uintptr_t obj = GameObject();
    clockValid = obj && ReadClock(obj, &seconds, &clockRate, &swapState, &act, tod, sizeof(tod));
    if (!clockValid) { status = "Day cycle waiting for the game object"; return; }
    if (enabled) {
        if (originalRate < 0) originalRate = clockRate;
        if (clockRate != rate) WriteInt(obj + 0xC8, rate);
    } else if (originalRate >= 0 && clockRate != originalRate) {
        WriteInt(obj + 0xC8, originalRate);
    }
    if (pendingSetSeconds >= 0) {
        WriteInt(obj + 0xBC, pendingSetSeconds % 86400);
        pendingSetSeconds = -1;
    }
    if (!patchFailed) status = enabled ? "Day cycle running" : "Day cycle off (native frozen clock)";
    MoveSun();
}

static void SetTime(int hour, int minute) {
    pendingSetSeconds = ((hour % 24 + 24) % 24) * 3600 + ((minute % 60 + 60) % 60) * 60;
}

static ULONGLONG lastStatusWrite = 0;
static void WriteStatus() {
    const ULONGLONG tick = GetTickCount64();
    if (tick - lastStatusWrite < 1000 || directory.empty()) return;
    lastStatusWrite = tick;
    FILE* f = std::fopen((directory + "\\day_cycle_status.json").c_str(), "wb");
    if (!f) return;
    const unsigned s = seconds % 86400;
    std::fprintf(f, "{\"enabled\":%s,\"patched\":%s,\"patch_failed\":%s,\"status\":\"%s\",\"clock_valid\":%s,"
                    "\"clock\":\"%02u:%02u:%02u\",\"seconds\":%u,\"rate\":%d,\"rate_setting\":%d,\"original_rate\":%d,"
                    "\"act\":%d,\"tod\":\"%s\",\"swap_state\":%d,\"point_lights\":%d,"
                    "\"sun\":{\"status\":\"%s\",\"object\":\"%08X\",\"native\":[%.3f,%.3f,%.3f],\"now\":[%.3f,%.3f,%.3f],\"engine_rewrites\":%lu,\"objects\":%u,\"finds\":%lu},"
                    "\"shadow\":{\"patched\":%s,\"patch_failed\":%s,\"calls\":%lu,\"native\":[%.3f,%.3f,%.3f],\"now\":[%.3f,%.3f,%.3f]}}\n",
                 enabled ? "true" : "false", patched ? "true" : "false", patchFailed ? "true" : "false", status,
                 clockValid ? "true" : "false", s / 3600, s / 60 % 60, s % 60, seconds, clockRate, rate, originalRate,
                 act, tod, swapState, PointLightCount(),
                 sunStatus, unsigned(sunObject), sunBase[0], sunBase[1], sunBase[2], sunNow[0], sunNow[1], sunNow[2], sunOverwrites, sunCount, sunFinds,
                 shadowPatched ? "true" : "false", shadowPatchFailed ? "true" : "false", shadowCalls,
                 shadowBase[0], shadowBase[1], shadowBase[2], shadowNow[0], shadowNow[1], shadowNow[2]);
    std::fclose(f);
}

} // namespace WosDayCycle
