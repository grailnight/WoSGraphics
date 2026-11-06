#pragma once
// Street steam: Spider-Man 3's drifting city smoke, ported.
//
// SM3 keeps three particle systems around the camera, fx_player_cam_ld/hd
// (low/high camera; the night variants differ only slightly): big faint puffs
// 100..300 m ahead, a middle band within ~100 m and a few puffs within 15 m,
// all blown one way across the city at 2..2.5 m/s. That is the smoke that
// floats between the buildings. Its steam funnels add a small plume each
// (fx_smk_steamfunnel). Both use the texture fx_smk_light (top row of a 4x2
// atlas), drawn as texture * vertex colour with a near fade of 1.5..3 m.
//
// Every number in kCamLow/kCamHigh/kFunnel is SM3's aps data (decoded from
// the .aeps records, WoS_SM3Atmosphere notes): source rate, position box,
// size, life, alpha, velocity box, spin, linear growth, fade in/out, colour.
//
// 1. Observe. WoS steam funnels are recognised at draw time by their model
//    box fingerprint (forwarded by WosChimneySmoke::Observe).
// 2. Simulate on the CPU: the camera systems spawn in a box that turns with
//    the camera's heading, particles then live in world space; funnels emit
//    from their vent.
// 3. Draw at the native bloom composite: camera-facing quads far to near,
//    lit by the scene's ambient and sun, softened against the scene depth.
//
// fx_smk_light.dds is a Spider-Man 3 asset: local use only, never ship it.
// Include after chimney_smoke.h.
#include "street_steam_emitters.h"

namespace WosStreetSteam {

static bool enabled = true, faulted = false, cameraSmoke = true, funnelSmoke = true;
static float density = 1.0f, brightness = 1.0f, ambientShare = 1.0f, sunShare = 0.35f;
static float highAltitude = 40.0f, funnelScale = 4.0f, windAngle = 0.0f, windScale = 1.0f;
static float softRange = 8.0f, maxDistance = 300.0f, keepSeconds = 600.0f;
static int maxParticles = 1500;
static const char* status = "Street steam waiting for the camera";
static std::string directory;

// ------------------------------------------------------------ SM3 systems
struct Spec {
    float rate, pos0[3], pos1[3], size0, size1, life0, life1, alpha0, alpha1;
    float vel0[3], vel1[3], spin, grow, fadeIn, fadeOut, grey0, grey1;
};
static const Spec kCamLow[3] = {   // fx_player_cam_ld
    { 5.0f, { -200, 0, 100 }, { 200, 50, 300 }, 20, 45, 5, 5, 0.25f, 0.5f, { 2, -0.05f, -1.27f }, { 2.54f, 0.05f, 1.27f }, 0.05f, 2.0f, 0.5f, 0.5f, 1, 1 },
    { 3.333f, { -100, -10, -75 }, { 100, 10, 100 }, 2.5f, 10, 15, 15, 0.25f, 0.37f, { 1.27f, -0.05f, -1.27f }, { 2.54f, 0.05f, 1.27f }, 0.15f, 1.05f, 0.5f, 0.5f, 1, 1 },
    { 1.5f, { -15, 0, -15 }, { 15, 15, 15 }, 2.5f, 7.5f, 10, 10, 0.15f, 0.37f, { 1.27f, -0.05f, -1.27f }, { 2.54f, 0.05f, 1.27f }, 0.15f, 1.05f, 0.5f, 0.5f, 1, 1 },
};
static const Spec kCamHigh[3] = {  // fx_player_cam_hd
    { 10.0f, { -200, -50, 100 }, { 200, 50, 300 }, 20, 30, 5, 5, 0.25f, 0.5f, { 2, -0.05f, -1.27f }, { 2.54f, 0.05f, 1.27f }, 0.05f, 3.0f, 0.5f, 0.5f, 1, 1 },
    { 3.333f, { -100, -25, -75 }, { 100, 25, 100 }, 2.5f, 15, 15, 15, 0.25f, 0.35f, { 1.27f, -0.05f, -1.27f }, { 2.54f, 0.05f, 1.27f }, 0.15f, 1.05f, 0.5f, 0.5f, 1, 1 },
    { 2.0f, { -15, -15, -15 }, { 15, 15, 15 }, 2.5f, 10, 10, 10, 0.15f, 0.37f, { 1.27f, -0.05f, -1.27f }, { 2.54f, 0.05f, 1.27f }, 0.15f, 1.05f, 0.5f, 0.5f, 1, 1 },
};
// fx_smk_steamfunnel; lengths are multiplied by funnelScale (SM3 scales the effect per placement).
static const Spec kFunnel = { 3.0f, { 0, 0, 0 }, { 0, 0, 0 }, 0.254f, 0.381f, 8, 10, 0.3f, 0.5f,
    { -0.0762f, 0.204f, -0.0762f }, { 0.0762f, 0.351f, 0.0762f }, 0.35f, 0.0762f, 0.3f, 0.3f, 0.1771f, 0.6745f };

// ---------------------------------------------------------------- state
struct Particle {
    double p[3];
    float v[3], age, life, size0, grow, alpha, angle, spin, grey, fadeIn, fadeOut;
    int frame;
};
struct Emitter {               // a WoS steam funnel
    double p[3] = {};
    float seed = 0;
    int model = 0;
    double born = 0, seen = 0;
    bool active = false;
    float spawn = 0;
    uint32_t rng = 1;
    std::vector<Particle> particles;
};
static std::vector<Emitter> emitters;
static std::vector<Particle> camParticles;
static float camSpawn[3] = {};
static uint32_t camRng = 0x9E3779B9u;
static bool camWarm = false, camHigh = false;
static unsigned long matchedDraws = 0, rejectedWorld = 0, framesDrawn = 0, failures = 0;
static unsigned long particlesLastFrame = 0, emittersActive = 0;
static int renderedFrame = -1;
static double lastSim = -1;

static double Now() { return WosChimneySmoke::Now(); }

static float Random(uint32_t& s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return float(s & 0xffffff) / float(0x1000000);
}
static float Range(uint32_t& s, float a, float b) { return a + (b - a) * Random(s); }

static bool Close(float a, float b) { return std::fabs(a - b) <= 1e-5f * (std::max)(1.0f, std::fabs(b)); }
static int Match(const float scale[4], const float bias[4]) {
    for (int m = 0; m < int(sizeof(kModels) / sizeof(kModels[0])); ++m) {
        const Model& k = kModels[m];
        if (Close(scale[1], k.scale[1]) && Close(bias[1], k.bias[1]) && Close(scale[0], k.scale[0]) &&
            Close(scale[2], k.scale[2]) && Close(bias[0], k.bias[0]) && Close(bias[2], k.bias[2]))
            return m;
    }
    return -1;
}

static void Remember(const double p[3], int model, double now) {
    // The intact funnel and the tube of its breakable twin sit on the same spot.
    for (auto& e : emitters) {
        const double dx = e.p[0] - p[0], dz = e.p[2] - p[2];
        if (dx * dx + dz * dz < 2.5 * 2.5 && std::fabs(e.p[1] - p[1]) < 5.0) {
            e.seen = now;
            return;
        }
    }
    Emitter e;
    std::memcpy(e.p, p, sizeof(e.p));
    e.model = model; e.born = now; e.seen = now;
    e.seed = float(std::fmod(std::fabs(p[0] * 0.137 + p[2] * 0.311 + p[1] * 0.071), 1.0));
    e.rng = uint32_t(e.seed * 4294967000.0) | 1u;
    if (emitters.size() >= 512) {
        auto oldest = std::min_element(emitters.begin(), emitters.end(), [](const Emitter& a, const Emitter& b) { return a.seen < b.seen; });
        *oldest = std::move(e);
    } else {
        emitters.push_back(std::move(e));
    }
}

// Forwarded by WosChimneySmoke::Observe for every draw whose VS has the
// world/gDecompPos layout; reads the world matrix only for a funnel.
static void Observe(IDirect3DDevice9* d, int worldRegister, const float scale[4], const float bias[4]) {
    const int index = Match(scale, bias);
    if (index < 0) return;
    float w[12];
    if (FAILED(d->GetVertexShaderConstantF(worldRegister, w, 3))) return;
    ++matchedDraws;
    const float sy = std::sqrt(w[1] * w[1] + w[5] * w[5] + w[9] * w[9]);
    if (!std::isfinite(sy) || sy < 0.25f || sy > 4.0f || w[5] < 0.95f * sy) { ++rejectedWorld; return; }   // knocked over
    const Model& model = kModels[index];
    double p[3];
    for (int r = 0; r < 3; ++r)
        p[r] = double(w[r * 4]) * model.vent[0] + double(w[r * 4 + 1]) * model.vent[1] + double(w[r * 4 + 2]) * model.vent[2] + double(w[r * 4 + 3]);
    if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]) || std::fabs(p[0]) > 1e6 || std::fabs(p[2]) > 1e6) { ++rejectedWorld; return; }
    Remember(p, index, Now());
}
static bool Wanted() { return enabled && !faulted && funnelSmoke; }

// ------------------------------------------------------------- simulation
// heading: camera forward flattened on the ground (x, z); null for a world-aligned box.
static void Spawn(std::vector<Particle>& out, uint32_t& s, const Spec& k, float scale, const double origin[3],
                  const float* heading, float age) {
    Particle q{};
    float local[3];
    for (int i = 0; i < 3; ++i) local[i] = Range(s, k.pos0[i], k.pos1[i]) * scale;
    if (heading) {   // box x = right, z = forward
        const float fx = heading[0], fz = heading[1];
        q.p[0] = origin[0] + local[0] * fz + local[2] * fx;
        q.p[1] = origin[1] + local[1];
        q.p[2] = origin[2] - local[0] * fx + local[2] * fz;
    } else {
        for (int i = 0; i < 3; ++i) q.p[i] = origin[i] + local[i];
    }
    // Velocities are world-aligned in SM3 (its cam smoke always drifts the same way
    // across the city); WindAngle turns that direction, WindScale its speed.
    float v[3];
    for (int i = 0; i < 3; ++i) v[i] = Range(s, k.vel0[i], k.vel1[i]) * scale;
    const float a = windAngle * 0.0174532925f, ca = std::cos(a), sa = std::sin(a);
    q.v[0] = (v[0] * ca - v[2] * sa) * (heading ? windScale : 1.0f);
    q.v[1] = v[1];
    q.v[2] = (v[0] * sa + v[2] * ca) * (heading ? windScale : 1.0f);
    q.life = Range(s, k.life0, k.life1);
    q.size0 = Range(s, k.size0, k.size1) * scale;
    q.grow = k.grow * scale;
    q.alpha = Range(s, k.alpha0, k.alpha1);
    q.angle = Range(s, 0.0f, 6.2831853f);
    q.spin = Range(s, -k.spin, k.spin);
    q.grey = Range(s, k.grey0, k.grey1);
    q.fadeIn = k.fadeIn; q.fadeOut = k.fadeOut;
    q.frame = (std::min)(3, int(Random(s) * 4.0f));
    q.age = (std::min)(age, q.life * 0.999f);
    for (int i = 0; i < 3; ++i) q.p[i] += q.v[i] * q.age;
    q.angle += q.spin * q.age;
    out.push_back(q);
}

static void Advance(std::vector<Particle>& ps, float dt) {
    for (auto& q : ps) {
        q.age += dt;
        for (int i = 0; i < 3; ++i) q.p[i] += q.v[i] * dt;
        q.angle += q.spin * dt;
    }
    ps.erase(std::remove_if(ps.begin(), ps.end(), [](const Particle& q) { return q.age >= q.life; }), ps.end());
}

static void Simulate(const double origin[3], const float heading[2], double now) {
    const float dt = lastSim < 0 ? 0.0f : float((std::min)(0.1, (std::max)(0.0, now - lastSim)));
    lastSim = now;
    size_t budget = size_t((std::max)(0, maxParticles));
    // Camera systems first: they are the look.
    if (cameraSmoke) {
        camHigh = origin[1] > highAltitude;
        const Spec* set = camHigh ? kCamHigh : kCamLow;
        Advance(camParticles, dt);
        if (!camWarm) {   // start with a populated sky, not an empty one
            camWarm = true;
            for (int s = 0; s < 3; ++s) {
                const int n = int(set[s].rate * set[s].life1);
                for (int i = 0; i < n; ++i) Spawn(camParticles, camRng, set[s], 1.0f, origin, heading, Range(camRng, 0.0f, set[s].life1));
            }
        }
        for (int s = 0; s < 3; ++s) {
            camSpawn[s] += set[s].rate * dt;
            while (camSpawn[s] >= 1.0f) { camSpawn[s] -= 1.0f; Spawn(camParticles, camRng, set[s], 1.0f, origin, heading, 0.0f); }
        }
        if (camParticles.size() > budget) camParticles.erase(camParticles.begin(), camParticles.begin() + (camParticles.size() - budget));
        budget -= camParticles.size();
    } else {
        camParticles.clear(); camWarm = false;
    }
    // Funnels, nearest first.
    std::vector<std::pair<double, Emitter*>> order;
    for (auto& e : emitters) {
        const double dx = e.p[0] - origin[0], dy = e.p[1] - origin[1], dz = e.p[2] - origin[2];
        const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (funnelSmoke && distance <= maxDistance && now - e.seen <= keepSeconds) order.push_back({ distance, &e });
        else if (e.active) { e.active = false; e.particles.clear(); e.spawn = 0; }
    }
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    emittersActive = 0;
    for (auto& item : order) {
        Emitter& e = *item.second;
        if (!e.active) {
            e.active = true; e.particles.clear(); e.spawn = 0;
            const int warm = int(kFunnel.rate * kFunnel.life1);
            for (int i = 0; i < warm; ++i) Spawn(e.particles, e.rng, kFunnel, funnelScale, e.p, nullptr, Range(e.rng, 0.0f, kFunnel.life1));
        }
        Advance(e.particles, dt);
        e.spawn += kFunnel.rate * dt;
        while (e.spawn >= 1.0f) { e.spawn -= 1.0f; Spawn(e.particles, e.rng, kFunnel, funnelScale, e.p, nullptr, 0.0f); }
        if (e.particles.size() > budget) e.particles.resize(budget);
        budget -= e.particles.size();
        ++emittersActive;
    }
}

// ------------------------------------------------------------------ drawing
static IDirect3DDevice9* owner = nullptr;
static IDirect3DVertexShader9* vs = nullptr;
static IDirect3DPixelShader9* ps = nullptr;
static IDirect3DTexture9* texture = nullptr;
static bool resourcesTried = false;

static void Reset() {
    if (vs) { vs->Release(); vs = nullptr; }
    if (ps) { ps->Release(); ps = nullptr; }
    if (texture) { texture->Release(); texture = nullptr; }
    owner = nullptr; resourcesTried = false; faulted = false; renderedFrame = -1;
}

// fx_smk_light: DXT5, 512x256, full mip chain, as SM3 created it.
static IDirect3DTexture9* LoadDds(IDirect3DDevice9* d, const std::string& path) {
    auto bytes = WosCascadeProbe::ReadFile(path);
    if (bytes.size() < 128 || std::memcmp(bytes.data(), "DDS ", 4)) return nullptr;
    const unsigned char* h = bytes.data() + 4;
    uint32_t height, width, mips, fourcc;
    std::memcpy(&height, h + 8, 4); std::memcpy(&width, h + 12, 4); std::memcpy(&mips, h + 24, 4); std::memcpy(&fourcc, h + 80, 4);
    if (fourcc != MAKEFOURCC('D', 'X', 'T', '5') || !width || !height || width > 4096 || height > 4096) return nullptr;
    mips = (std::max)(1u, (std::min)(mips, 13u));
    IDirect3DTexture9* t = nullptr;
    if (FAILED(d->CreateTexture(width, height, mips, 0, D3DFMT_DXT5, D3DPOOL_MANAGED, &t, nullptr))) return nullptr;
    size_t offset = 128;
    for (uint32_t level = 0; level < mips; ++level) {
        const uint32_t w = (std::max)(1u, width >> level), hgt = (std::max)(1u, height >> level);
        const uint32_t rowBytes = (std::max)(1u, (w + 3) / 4) * 16, rows = (std::max)(1u, (hgt + 3) / 4);
        if (offset + size_t(rowBytes) * rows > bytes.size()) { t->Release(); return nullptr; }
        D3DLOCKED_RECT r;
        if (FAILED(t->LockRect(level, &r, nullptr, 0))) { t->Release(); return nullptr; }
        for (uint32_t y = 0; y < rows; ++y)
            std::memcpy(static_cast<unsigned char*>(r.pBits) + size_t(y) * r.Pitch, bytes.data() + offset + size_t(y) * rowBytes, rowBytes);
        t->UnlockRect(level);
        offset += size_t(rowBytes) * rows;
    }
    return t;
}

static bool Resources(IDirect3DDevice9* d) {
    if (owner && owner != d) Reset();
    if (vs && ps && texture) return true;
    if (resourcesTried) return false;
    resourcesTried = true;
    auto v = WosCascadeProbe::ReadFile(directory + "\\street_steam_vs.cso");
    auto p = WosCascadeProbe::ReadFile(directory + "\\street_steam_ps.cso");
    uint32_t vf = 0, pf = 0;
    if (v.size() >= 8 && !(v.size() % 4)) std::memcpy(&vf, v.data(), 4);
    if (p.size() >= 8 && !(p.size() % 4)) std::memcpy(&pf, p.data(), 4);
    if (vf != 0xfffe0300u || pf != 0xffff0300u ||
        FAILED(d->CreateVertexShader(reinterpret_cast<const DWORD*>(v.data()), &vs)) ||
        FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(p.data()), &ps))) {
        Reset(); resourcesTried = true; faulted = true; status = "Street steam shaders missing in WoS_Deferred\\street_steam";
        return false;
    }
    texture = LoadDds(d, directory + "\\fx_smk_light.dds");
    if (!texture) { Reset(); resourcesTried = true; faulted = true; status = "Street steam texture fx_smk_light.dds missing or not DXT5"; return false; }
    owner = d;
    return true;
}

struct Vertex { float x, y, z, u, v, alpha, grey; };
struct Sprite { float depth; Vertex corner[4]; };

static void AddSprites(std::vector<Sprite>& out, const std::vector<Particle>& ps, const double origin[3],
                       const double right[3], const double up[3], const double fwd[3]) {
    for (const auto& q : ps) {
        const float f = q.age / q.life;
        const float in = q.fadeIn > 0 ? f / q.fadeIn : 1.0f, outFade = q.fadeOut > 0 ? (1.0f - f) / q.fadeOut : 1.0f;
        const float alpha = q.alpha * density * (std::min)(1.0f, in) * (std::min)(1.0f, outFade);
        if (alpha <= 0.002f) continue;
        const double c[3] = { q.p[0] - origin[0], q.p[1] - origin[1], q.p[2] - origin[2] };
        const double half = 0.5 * (q.size0 + q.grow * q.age);
        const double depthC = c[0] * fwd[0] + c[1] * fwd[1] + c[2] * fwd[2];
        if (depthC < -half) continue;   // entirely behind the camera
        const double ca = std::cos(q.angle) * half, sa = std::sin(q.angle) * half;
        double ax[3], ay[3];
        for (int i = 0; i < 3; ++i) { ax[i] = right[i] * ca + up[i] * sa; ay[i] = -right[i] * sa + up[i] * ca; }
        const float u0 = q.frame * 0.25f, u1 = u0 + 0.25f;
        const float sx[4] = { -1, 1, -1, 1 }, sy[4] = { 1, 1, -1, -1 };
        const float us[4] = { u0, u1, u0, u1 }, vv[4] = { 0.0f, 0.0f, 0.5f, 0.5f };
        Sprite s;
        s.depth = float(depthC);
        for (int k = 0; k < 4; ++k)
            s.corner[k] = { float(c[0] + ax[0] * sx[k] + ay[0] * sy[k]), float(c[1] + ax[1] * sx[k] + ay[1] * sy[k]),
                            float(c[2] + ax[2] * sx[k] + ay[2] * sy[k]), us[k], vv[k], alpha, q.grey };
        out.push_back(s);
    }
}

static void WriteStatus();
// At the native bloom composite. Texture 0 is the finished linear scene.
static void BloomDraw(IDirect3DDevice9* d, bool bloom) {
    if (!bloom || WosProbe::suppress || WosDeferred::busy || renderedFrame == g_frame_index) return;
    WriteStatus();
    if (!enabled || faulted) return;
    renderedFrame = g_frame_index;
    particlesLastFrame = 0;
    if (!WosIndirectCamera::frameReady) { status = "Street steam waiting for the engine camera"; return; }
    IDirect3DBaseTexture9* bound = nullptr;
    IDirect3DTexture9* hdr = nullptr, * depth = nullptr;
    IDirect3DSurface9* target = nullptr;
    D3DSURFACE_DESC desc = {};
    WosIndirectCamera::Camera camera;
    bool ok = SUCCEEDED(d->GetTexture(0, &bound)) && bound &&
              SUCCEEDED(bound->QueryInterface(IID_IDirect3DTexture9, reinterpret_cast<void**>(&hdr))) &&
              SUCCEEDED(hdr->GetLevelDesc(0, &desc)) && desc.Format == D3DFMT_A16B16G16R16F &&
              desc.Width >= 320 && desc.Height >= 200 && desc.Width <= 4096 && desc.Height <= 4096 &&
              WosIndirectCamera::Acquire(desc.Width, desc.Height, camera, depth, WosIndirectCamera::CurrentScene()) && depth &&
              Resources(d) && SUCCEEDED(hdr->GetSurfaceLevel(0, &target));
    double vp[16];
    if (ok && !WosChimneySmoke::Invert(camera.relativeInverse, vp)) ok = false;
    const double now = Now();
    std::vector<Sprite> sprites;
    float forward[4] = { 0, 0, 1, 0 };
    if (ok) {
        // Camera basis from the projection rows: row 0 ~ right, row 1 ~ up, row 3 ~ view direction.
        double right[3], up[3], fwd[3];
        auto unit = [](const double* row, double* out) {
            const double l = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
            for (int i = 0; i < 3; ++i) out[i] = l > 0 ? row[i] / l : 0;
        };
        unit(vp + 0, right); unit(vp + 4, up); unit(vp + 12, fwd);
        for (int i = 0; i < 3; ++i) forward[i] = float(fwd[i]);
        float heading[2] = { float(fwd[0]), float(fwd[2]) };
        const float hl = std::sqrt(heading[0] * heading[0] + heading[1] * heading[1]);
        if (hl > 1e-3f) { heading[0] /= hl; heading[1] /= hl; } else { heading[0] = 0; heading[1] = 1; }
        Simulate(camera.worldOrigin, heading, now);
        AddSprites(sprites, camParticles, camera.worldOrigin, right, up, fwd);
        for (const auto& e : emitters) if (e.active) AddSprites(sprites, e.particles, camera.worldOrigin, right, up, fwd);
        std::sort(sprites.begin(), sprites.end(), [](const Sprite& a, const Sprite& b) { return a.depth > b.depth; });
    }
    if (ok && !sprites.empty()) {
        std::vector<Vertex> list;
        list.reserve(sprites.size() * 6);
        for (const auto& s : sprites) {
            list.push_back(s.corner[0]); list.push_back(s.corner[1]); list.push_back(s.corner[2]);
            list.push_back(s.corner[2]); list.push_back(s.corner[1]); list.push_back(s.corner[3]);
        }
        WosVolumetric::HostScope busy;
        WosIndirect::State state(d);
        bool drawn = state.valid;
        auto check = [&](HRESULT h) { drawn = SUCCEEDED(h) && drawn; };
        if (drawn) {
            for (unsigned i = 0; i < 16; ++i) check(d->SetTexture(i, nullptr));
            check(d->SetDepthStencilSurface(nullptr));
            for (unsigned i = 1; i < state.slots; ++i) check(d->SetRenderTarget(i, nullptr));
            check(d->SetRenderTarget(0, target));
            const D3DVIEWPORT9 viewport = { 0, 0, desc.Width, desc.Height, 0, 1 };
            check(d->SetViewport(&viewport));
            check(d->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE4(0)));
            check(g_original_set_vertex_shader(d, vs));
            const D3DRENDERSTATETYPE zeros[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE,
                                                 D3DRS_SCISSORTESTENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE, D3DRS_CLIPPLANEENABLE,
                                                 D3DRS_SEPARATEALPHABLENDENABLE };
            for (auto s : zeros) check(d->SetRenderState(s, 0));
            check(d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE)); check(d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID));
            check(d->SetRenderState(D3DRS_COLORWRITEENABLE, 7)); check(d->SetRenderState(D3DRS_MULTISAMPLEMASK, 0xffffffff));
            // Premultiplied: rgb = light the smoke adds, a = what it lets through.
            check(d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE)); check(d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD));
            check(d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE)); check(d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCALPHA));
            check(d->SetTexture(0, texture));
            check(d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR)); check(d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR));
            check(d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR)); check(d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, 1));
            check(d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP)); check(d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
            check(d->SetTexture(1, depth));
            check(d->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_POINT)); check(d->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
            check(d->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_NONE)); check(d->SetSamplerState(1, D3DSAMP_SRGBTEXTURE, 0));
            check(d->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP)); check(d->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
            check(g_original_set_pixel_shader(d, ps));
            float vsc[5][4];
            for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) vsc[r][c] = float(vp[r * 4 + c]);
            std::memcpy(vsc[4], forward, sizeof(forward));
            check(d->SetVertexShaderConstantF(0, &vsc[0][0], 5));
            const bool sun = WosVolumetric::sunValid;
            const float sunLift = sun ? (std::max)(0.15f, WosVolumetric::sunDirection[1]) * sunShare : 0.0f;
            float light[3];
            for (int i = 0; i < 3; ++i)
                light[i] = brightness * (WosVolumetric::fogColour[i] * ambientShare + (sun ? WosVolumetric::sunColour[i] : 0.0f) * sunLift);
            const float psc[3][4] = {
                { 1.0f / desc.Width, 1.0f / desc.Height, (std::max)(0.5f, softRange), 0.6666667f },
                { light[0], light[1], light[2], 0 },
                { forward[0], forward[1], forward[2], 0 } };
            check(d->SetPixelShaderConstantF(0, camera.relativeInverse, 4));
            check(d->SetPixelShaderConstantF(4, &psc[0][0], 3));
            const UINT perCall = 6000;   // vertices per DrawPrimitiveUP
            for (size_t first = 0; drawn && first < list.size(); first += perCall) {
                const UINT n = UINT((std::min)(size_t(perCall), list.size() - first));
                check(g_original_draw_primitive_up(d, D3DPT_TRIANGLELIST, n / 3, list.data() + first, sizeof(Vertex)));
            }
            check(d->SetTexture(0, nullptr)); check(d->SetTexture(1, nullptr));
        }
        const bool restored = state.valid && state.Restore();
        if (!restored) { faulted = true; status = "Street steam state restore failed; disabled for this session"; }
        else if (!drawn) { ++failures; status = "Street steam pass failed this frame"; }
        else { ++framesDrawn; particlesLastFrame = static_cast<unsigned long>(sprites.size()); status = "Street steam ON"; }
    } else if (ok) {
        status = "Street steam: nothing to draw";
    } else if (!faulted) {
        status = "Street steam skipped: no scene texture, camera or depth";
    }
    if (target) target->Release();
    if (depth) depth->Release();
    if (hdr) hdr->Release();
    if (bound) bound->Release();
}

// Once a second, for checking the mod without a screenshot.
static ULONGLONG lastStatusWrite = 0;
static void WriteStatus() {
    const ULONGLONG tick = GetTickCount64();
    if (tick - lastStatusWrite < 1000 || directory.empty()) return;
    lastStatusWrite = tick;
    FILE* f = std::fopen((directory + "\\street_steam_status.json").c_str(), "wb");
    if (!f) return;
    const double now = Now();
    unsigned known = 0;
    for (const auto& e : emitters) if (now - e.seen <= keepSeconds) ++known;
    std::fprintf(f, "{\"enabled\":%s,\"faulted\":%s,\"status\":\"%s\",\"frames_drawn\":%lu,\"particles_last_frame\":%lu,\"failures\":%lu,"
                    "\"camera_particles\":%u,\"camera_set\":\"%s\",\"matched_draws\":%lu,\"rejected_world\":%lu,\"emitters_known\":%u,\"emitters_active\":%lu,\"emitters\":[",
                 enabled ? "true" : "false", faulted ? "true" : "false", status, framesDrawn, particlesLastFrame, failures,
                 unsigned(camParticles.size()), camHigh ? "high (fx_player_cam_hd)" : "low (fx_player_cam_ld)",
                 matchedDraws, rejectedWorld, known, emittersActive);
    bool first = true;
    for (const auto& e : emitters) {
        if (now - e.seen > keepSeconds) continue;
        std::fprintf(f, "%s{\"model\":\"%s\",\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,\"active\":%s,\"particles\":%u,\"seen_ago\":%.1f}",
                     first ? "" : ",", kModels[e.model].name, e.p[0], e.p[1], e.p[2], e.active ? "true" : "false",
                     unsigned(e.particles.size()), now - e.seen);
        first = false;
    }
    std::fprintf(f, "]}\n");
    std::fclose(f);
}

static void Install() {
    WosChimneySmoke::extraWanted = &Wanted;
    WosChimneySmoke::extraObserver = &Observe;
}

} // namespace WosStreetSteam
