#pragma once
// Smoke from chimneys.
//
// The game ships chimney props (s_rf_chimneya/b/c, s_rf_smokestacka) and even a
// chimney smoke effect (fx_smk_chimneya), but no entity references the effect,
// and placement data (.ens/.ent) is a pointer dump that cannot be edited. So the
// renderer finds the chimneys itself:
//
// 1. Observe. Every native draw carries the model's position dequantization
//    box in gDecompPosScale/gDecompPosBias and its world matrix in `world`
//    (registers read from the vertex shader's CTAB). The box identifies the
//    model (chimney_emitters.h), the matrix puts its flues in the world.
//    Flues are remembered for a while, so a plume stays when its chimney is
//    culled or hidden behind a parapet.
// 2. Draw. At the native bloom composite the nearest plumes are raymarched,
//    one screen rectangle each, against the scene depth and blended over the
//    linear HDR scene before god rays and fog are added.
//
// Include after native_indirect.h, deferred_volumetric.h and indirect_camera.h.
#include "chimney_emitters.h"

namespace WosChimneySmoke {

static bool enabled = true, faulted = false;
static float density = 1.0f, plumeHeight = 16.0f, stackHeight = 140.0f, spread = 0.16f;
static float windAngle = 60.0f, windStrength = 0.45f, riseSpeed = 1.6f;
static float brightness = 1.0f, maxDistance = 450.0f, stackDistance = 2500.0f, keepSeconds = 120.0f;
static float houseTint[3] = { 0.52f, 0.53f, 0.56f }, stackTint[3] = { 0.78f, 0.78f, 0.80f };
static int maxPlumes = 24, steps = 24;
static float noiseFrequency = 1.0f, noiseStrength = 0.9f, phaseG = 0.35f;
static const char* status = "Chimney smoke waiting for chimneys";
static std::string directory;

// ---------------------------------------------------------------- registry
struct Emitter {
    double p[3] = {};
    float radius = 0, seed = 0;
    int kind = 0, model = 0;
    double born = 0, seen = 0;
};
static std::vector<Emitter> emitters;
static unsigned long matchedDraws = 0, framesDrawn = 0, plumesLastFrame = 0, failures = 0, rejectedWorld = 0;
static unsigned long layoutsParsed = 0, layoutsUsable = 0;
static int renderedFrame = -1;

static double Now() {
    static LARGE_INTEGER frequency = {}, start = {};
    if (!frequency.QuadPart) { QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start); }
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return double(t.QuadPart - start.QuadPart) / double(frequency.QuadPart);
}

// ------------------------------------------------ vertex shader layouts
struct Layout { int world = -1, scale = -1, bias = -1, c2l = -1, c2s = -1; };   // c2l/c2s: entity props (compressed_to_local/screen)
// Keys keep their reference so a freed address can never be reused by
// another shader while its old layout is still cached.
static std::unordered_map<IDirect3DVertexShader9*, Layout> layouts;

static uint32_t U32(const unsigned char* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
static uint16_t U16(const unsigned char* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
// Reads the constant table the HLSL compiler left in the bytecode comments.
static Layout ParseLayout(const std::vector<unsigned char>& code) {
    Layout out;
    const size_t words = code.size() / 4;
    if (words < 2 || code.size() % 4) return out;
    const unsigned char* b = code.data();
    for (size_t i = 1; i < words;) {
        const uint32_t token = U32(b + i * 4), opcode = token & 0xffffu;
        if (opcode == 0xffffu) break;
        if (opcode != 0xfffeu) { i += ((token >> 24) & 0xfu) + 1; continue; }
        const size_t count = (token >> 16) & 0x7fffu;
        if (!count || count > words - i - 1) break;
        if (U32(b + (i + 1) * 4) == 0x42415443u && count >= 8) {   // 'CTAB'
            const unsigned char* t = b + (i + 2) * 4;
            const size_t bytes = (count - 1) * 4;
            const uint32_t entries = U32(t + 12), start = U32(t + 16);
            if (U32(t) != 28 || entries > 1024 || start > bytes || size_t(entries) * 20 > bytes - start) return out;
            for (uint32_t e = 0; e < entries; ++e) {
                const unsigned char* entry = t + start + e * 20;
                const uint32_t nameOffset = U32(entry);
                if (nameOffset >= bytes) continue;
                const unsigned char* end = static_cast<const unsigned char*>(std::memchr(t + nameOffset, 0, bytes - nameOffset));
                if (!end) continue;
                const std::string name(reinterpret_cast<const char*>(t + nameOffset), size_t(end - (t + nameOffset)));
                const unsigned set = U16(entry + 4), index = U16(entry + 6), regs = U16(entry + 8);
                if (set != 2 || index + regs > 256) continue;                  // float4 registers only
                if (name == "world" && regs >= 3) out.world = int(index);
                else if (name == "gDecompPosScale" && regs >= 1) out.scale = int(index);
                else if (name == "gDecompPosBias" && regs >= 1) out.bias = int(index);
                else if (name == "compressed_to_local" && regs >= 3) out.c2l = int(index);
                else if (name == "compressed_to_screen" && regs >= 4) out.c2s = int(index);
            }
            return out;
        }
        i += count + 1;
    }
    return out;
}
static const Layout* LayoutOf(IDirect3DVertexShader9* vs) {
    auto it = layouts.find(vs);
    if (it != layouts.end()) return &it->second;
    Layout layout;
    UINT size = 0;
    if (SUCCEEDED(vs->GetFunction(nullptr, &size)) && size && size < (1u << 20)) {
        std::vector<unsigned char> code(size);
        if (SUCCEEDED(vs->GetFunction(code.data(), &size))) layout = ParseLayout(code);
    }
    try { it = layouts.emplace(vs, layout).first; } catch (...) { return nullptr; }
    vs->AddRef();
    ++layoutsParsed;
    if (layout.world >= 0 && layout.scale >= 0 && layout.bias >= 0) ++layoutsUsable;
    return &it->second;
}

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

static void Remember(const double p[3], const Model& model, int index, float radius, double now) {
    const double merge = model.kind ? 3.0 : 0.6;
    for (auto& e : emitters) {
        const double dx = e.p[0] - p[0], dy = e.p[1] - p[1], dz = e.p[2] - p[2];
        if (dx * dx + dy * dy + dz * dz < merge * merge) {
            e.seen = now;
            return;
        }
    }
    Emitter e;
    std::memcpy(e.p, p, sizeof(e.p));
    e.radius = radius; e.kind = model.kind; e.model = index;
    e.born = now; e.seen = now;
    e.seed = float(std::fmod(std::fabs(p[0] * 0.137 + p[2] * 0.311 + p[1] * 0.071), 1.0));
    if (emitters.size() >= 256) {
        auto oldest = std::min_element(emitters.begin(), emitters.end(), [](const Emitter& a, const Emitter& b) { return a.seen < b.seen; });
        *oldest = e;
    } else {
        emitters.push_back(e);
    }
}

// Other modules that recognise props by the same fingerprint (street_steam.h)
// hook in here, so a draw's layout and box constants are read only once.
static bool (*extraWanted)() = nullptr;
static void (*extraObserver)(IDirect3DDevice9*, int worldRegister, const float scale[4], const float bias[4]) = nullptr;
// Entity props (steam funnels) use the NGL compressed_to_local/screen layout instead.
static void (*extraPropObserver)(IDirect3DDevice9*, int c2lRegister, int c2sRegister) = nullptr;

// Called for every native draw before it is issued; reads state only.
static void Observe(IDirect3DDevice9* d) {
    const bool mine = enabled && !faulted;
    const bool extra = extraObserver && extraWanted && extraWanted();
    if (!mine && !extra) return;
    IDirect3DVertexShader9* vs = nullptr;
    if (FAILED(d->GetVertexShader(&vs)) || !vs) return;
    const Layout* layout = LayoutOf(vs);
    vs->Release();
    if (!layout) return;
    if (layout->world < 0 || layout->scale < 0 || layout->bias < 0) {
        if (extra && extraPropObserver && layout->c2l >= 0 && layout->c2s >= 0) extraPropObserver(d, layout->c2l, layout->c2s);
        return;
    }
    float scale[4], bias[4];
    if (FAILED(d->GetVertexShaderConstantF(layout->scale, scale, 1)) || FAILED(d->GetVertexShaderConstantF(layout->bias, bias, 1))) return;
    if (extra) extraObserver(d, layout->world, scale, bias);
    if (!mine) return;
    const int index = Match(scale, bias);
    if (index < 0) return;
    float w[12];
    if (FAILED(d->GetVertexShaderConstantF(layout->world, w, 3))) return;
    ++matchedDraws;
    // An upright prop: rows are an orthonormal rotation about Y, possibly
    // scaled. Anything else (a debris piece mid-fall) is not a chimney.
    const float sx = std::sqrt(w[0] * w[0] + w[4] * w[4] + w[8] * w[8]);
    const float sy = std::sqrt(w[1] * w[1] + w[5] * w[5] + w[9] * w[9]);
    if (!std::isfinite(sx) || !std::isfinite(sy) || sy < 0.25f || sy > 4.0f || w[5] < 0.95f * sy) { ++rejectedWorld; return; }
    const Model& model = kModels[index];
    const double now = Now();
    for (unsigned f = 0; f < model.flues && f < 2; ++f) {
        const Flue& flue = model.flue[f];
        const float local[4] = { flue.x, flue.y, flue.z, 1.0f };
        double p[3];
        for (int r = 0; r < 3; ++r)
            p[r] = double(w[r * 4]) * local[0] + double(w[r * 4 + 1]) * local[1] + double(w[r * 4 + 2]) * local[2] + double(w[r * 4 + 3]);
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]) || std::fabs(p[0]) > 1e6 || std::fabs(p[2]) > 1e6) { ++rejectedWorld; return; }
        Remember(p, model, index, flue.radius * sy, now);
    }
}

// ------------------------------------------------------------------ drawing
static IDirect3DDevice9* owner = nullptr;
static IDirect3DPixelShader9* shader = nullptr;
static bool shaderTried = false;

static void Reset() {
    if (shader) { shader->Release(); shader = nullptr; }
    owner = nullptr; shaderTried = false; faulted = false; renderedFrame = -1;
}
static void Forget() { emitters.clear(); }

static bool Shader(IDirect3DDevice9* d) {
    if (owner && owner != d) Reset();
    if (shader) return true;
    if (shaderTried) return false;
    shaderTried = true;
    auto bytes = WosCascadeProbe::ReadFile(directory + "\\chimney_smoke.cso");
    uint32_t first = 0;
    if (bytes.size() >= 8 && !(bytes.size() % 4)) std::memcpy(&first, bytes.data(), 4);
    if (first != 0xffff0300u || FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(bytes.data()), &shader))) {
        shader = nullptr; faulted = true; status = "Chimney smoke shader missing in WoS_Deferred\\chimney_smoke";
        return false;
    }
    owner = d;
    return true;
}

static bool Invert(const float in[16], double out[16]) {
    double m[16], inv[16];
    for (int i = 0; i < 16; ++i) m[i] = in[i];
    inv[0] = m[5]*m[10]*m[15]-m[5]*m[11]*m[14]-m[9]*m[6]*m[15]+m[9]*m[7]*m[14]+m[13]*m[6]*m[11]-m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15]+m[4]*m[11]*m[14]+m[8]*m[6]*m[15]-m[8]*m[7]*m[14]-m[12]*m[6]*m[11]+m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15]-m[4]*m[11]*m[13]-m[8]*m[5]*m[15]+m[8]*m[7]*m[13]+m[12]*m[5]*m[11]-m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]+m[4]*m[10]*m[13]+m[8]*m[5]*m[14]-m[8]*m[6]*m[13]-m[12]*m[5]*m[10]+m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15]+m[1]*m[11]*m[14]+m[9]*m[2]*m[15]-m[9]*m[3]*m[14]-m[13]*m[2]*m[11]+m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15]-m[0]*m[11]*m[14]-m[8]*m[2]*m[15]+m[8]*m[3]*m[14]+m[12]*m[2]*m[11]-m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15]+m[0]*m[11]*m[13]+m[8]*m[1]*m[15]-m[8]*m[3]*m[13]-m[12]*m[1]*m[11]+m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14]-m[0]*m[10]*m[13]-m[8]*m[1]*m[14]+m[8]*m[2]*m[13]+m[12]*m[1]*m[10]-m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15]-m[1]*m[7]*m[14]-m[5]*m[2]*m[15]+m[5]*m[3]*m[14]+m[13]*m[2]*m[7]-m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15]+m[0]*m[7]*m[14]+m[4]*m[2]*m[15]-m[4]*m[3]*m[14]-m[12]*m[2]*m[7]+m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15]-m[0]*m[7]*m[13]-m[4]*m[1]*m[15]+m[4]*m[3]*m[13]+m[12]*m[1]*m[7]-m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]+m[0]*m[6]*m[13]+m[4]*m[1]*m[14]-m[4]*m[2]*m[13]-m[12]*m[1]*m[6]+m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11]+m[1]*m[7]*m[10]+m[5]*m[2]*m[11]-m[5]*m[3]*m[10]-m[9]*m[2]*m[7]+m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11]-m[0]*m[7]*m[10]-m[4]*m[2]*m[11]+m[4]*m[3]*m[10]+m[8]*m[2]*m[7]-m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]+m[0]*m[7]*m[9]+m[4]*m[1]*m[11]-m[4]*m[3]*m[9]-m[8]*m[1]*m[7]+m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10]-m[0]*m[6]*m[9]-m[4]*m[1]*m[10]+m[4]*m[2]*m[9]+m[8]*m[1]*m[6]-m[8]*m[2]*m[5];
    const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!std::isfinite(det) || std::fabs(det) < 1e-30) return false;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] / det;
    return true;
}

struct Plume {
    float base[3], radius, drift[3], height, box[2][3], fade, seed, kind;
    double distance;
};

// Screen rectangle of the plume box, or false when it is entirely off screen.
// A box that crosses the camera plane takes the whole screen.
static bool Rect(const double vp[16], const Plume& p, unsigned w, unsigned h, float rect[4]) {
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    bool crosses = false;
    for (int c = 0; c < 8; ++c) {
        const double v[4] = { p.box[c & 1][0], p.box[(c >> 1) & 1][1], p.box[(c >> 2) & 1][2], 1.0 };
        double clip[4];
        for (int r = 0; r < 4; ++r) clip[r] = vp[r * 4] * v[0] + vp[r * 4 + 1] * v[1] + vp[r * 4 + 2] * v[2] + vp[r * 4 + 3] * v[3];
        if (clip[3] < 0.05) { crosses = true; break; }
        const double sx = clip[0] / clip[3], sy = clip[1] / clip[3];
        x0 = (std::min)(x0, sx); x1 = (std::max)(x1, sx); y0 = (std::min)(y0, sy); y1 = (std::max)(y1, sy);
    }
    if (crosses) { rect[0] = 0; rect[1] = 0; rect[2] = float(w); rect[3] = float(h); return true; }
    if (x1 < -1 || x0 > 1 || y1 < -1 || y0 > 1) return false;
    x0 = (std::max)(x0, -1.0); x1 = (std::min)(x1, 1.0); y0 = (std::max)(y0, -1.0); y1 = (std::min)(y1, 1.0);
    rect[0] = float(std::floor((x0 * 0.5 + 0.5) * w)); rect[2] = float(std::ceil((x1 * 0.5 + 0.5) * w));
    rect[1] = float(std::floor((0.5 - y1 * 0.5) * h)); rect[3] = float(std::ceil((0.5 - y0 * 0.5) * h));
    return rect[2] > rect[0] && rect[3] > rect[1];
}
static bool DrawRect(IDirect3DDevice9* d, const float r[4], unsigned w, unsigned h) {
    struct V { float x, y, z, rhw, u, v; };
    const float iw = 1.0f / float(w), ih = 1.0f / float(h);
    const V q[] = {
        { r[0] - .5f, r[1] - .5f, 0, 1, r[0] * iw, r[1] * ih }, { r[2] - .5f, r[1] - .5f, 0, 1, r[2] * iw, r[1] * ih },
        { r[0] - .5f, r[3] - .5f, 0, 1, r[0] * iw, r[3] * ih }, { r[2] - .5f, r[3] - .5f, 0, 1, r[2] * iw, r[3] * ih } };
    return SUCCEEDED(g_original_draw_primitive_up(d, D3DPT_TRIANGLESTRIP, 2, q, sizeof(V)));
}

// Picks the plumes for this frame, nearest first.
static std::vector<Plume> Select(const double origin[3], double now) {
    std::vector<Plume> out;
    const float wind = windAngle * 0.0174532925f;
    const float drift[3] = { std::cos(wind) * windStrength, 0.0f, std::sin(wind) * windStrength };
    for (const auto& e : emitters) {
        const double age = now - e.seen;
        if (age > keepSeconds) continue;
        const float reach = e.kind ? stackDistance : maxDistance;
        Plume p{};
        for (int i = 0; i < 3; ++i) p.base[i] = float(e.p[i] - origin[i]);
        p.distance = std::sqrt(double(p.base[0]) * p.base[0] + double(p.base[1]) * p.base[1] + double(p.base[2]) * p.base[2]);
        if (p.distance > reach) continue;
        p.height = e.kind ? stackHeight : plumeHeight;
        // Plumes differ a little so a row of chimneys does not look stamped.
        p.height *= 0.8f + 0.4f * e.seed;
        p.radius = e.radius;
        for (int i = 0; i < 3; ++i) p.drift[i] = drift[i] * (e.kind ? 0.9f : 1.0f);
        const float grow = e.kind ? spread * 0.9f : spread;
        const float top = p.radius + grow * p.height;
        const float reachX = p.drift[0] * p.height, reachZ = p.drift[2] * p.height;
        p.box[0][0] = p.base[0] + (std::min)(0.0f, reachX) - top; p.box[1][0] = p.base[0] + (std::max)(0.0f, reachX) + top;
        p.box[0][1] = p.base[1] - 0.3f;                         p.box[1][1] = p.base[1] + p.height;
        p.box[0][2] = p.base[2] + (std::min)(0.0f, reachZ) - top; p.box[1][2] = p.base[2] + (std::max)(0.0f, reachZ) + top;
        const double fadeIn = (std::min)(1.0, (now - e.born) / 2.0);
        const double fadeOut = (std::min)(1.0, (keepSeconds - age) / 5.0);
        const double fadeFar = (std::min)(1.0, (reach - p.distance) / (reach * 0.2));
        p.fade = float((std::max)(0.0, fadeIn * fadeOut * fadeFar));
        p.seed = e.seed;
        p.kind = float(e.kind);
        if (p.fade > 0.01f) out.push_back(p);
    }
    std::sort(out.begin(), out.end(), [](const Plume& a, const Plume& b) { return a.distance < b.distance; });
    if (out.size() > size_t((std::max)(1, maxPlumes))) out.resize(size_t((std::max)(1, maxPlumes)));
    return out;
}

// At the native bloom composite. Texture 0 is the finished linear scene.
static void WriteStatus();
static void BloomDraw(IDirect3DDevice9* d, bool bloom) {
    if (!bloom || WosProbe::suppress || WosDeferred::busy || renderedFrame == g_frame_index) return;
    WriteStatus();
    if (!enabled || faulted) return;
    renderedFrame = g_frame_index;
    plumesLastFrame = 0;
    if (emitters.empty()) { status = "Chimney smoke: no chimney seen yet"; return; }
    if (!WosIndirectCamera::frameReady) { status = "Chimney smoke waiting for the engine camera"; return; }
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
              Shader(d) && SUCCEEDED(hdr->GetSurfaceLevel(0, &target));
    double vp[16];
    if (ok && !Invert(camera.relativeInverse, vp)) ok = false;
    const double now = Now();
    std::vector<Plume> plumes;
    if (ok) plumes = Select(camera.worldOrigin, now);
    if (ok && !plumes.empty()) {
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
            check(d->SetVertexShader(nullptr)); check(d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1));
            const D3DRENDERSTATETYPE zeros[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE,
                                                 D3DRS_SCISSORTESTENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE, D3DRS_CLIPPLANEENABLE,
                                                 D3DRS_SEPARATEALPHABLENDENABLE };
            for (auto s : zeros) check(d->SetRenderState(s, 0));
            check(d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE)); check(d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID));
            check(d->SetRenderState(D3DRS_COLORWRITEENABLE, 7)); check(d->SetRenderState(D3DRS_MULTISAMPLEMASK, 0xffffffff));
            // rgb = in-scattered light, a = what the smoke lets through.
            check(d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE)); check(d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD));
            check(d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE)); check(d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCALPHA));
            check(d->SetTexture(0, depth));
            check(d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT)); check(d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
            check(d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE)); check(d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, 0));
            check(d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP)); check(d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
            check(g_original_set_pixel_shader(d, shader));
            check(d->SetPixelShaderConstantF(0, camera.relativeInverse, 4));
            const bool sun = WosVolumetric::sunValid;
            const float time = float(std::fmod(now, 3600.0));
            // Plumes are drawn far to near so the nearer smoke covers the farther.
            for (auto it = plumes.rbegin(); it != plumes.rend() && drawn; ++it) {
                const Plume& p = *it;
                float rect[4];
                if (!Rect(vp, p, desc.Width, desc.Height, rect)) continue;
                const float* tint = p.kind > 0.5f ? stackTint : houseTint;
                const float grow = p.kind > 0.5f ? spread * 0.9f : spread;
                const float c[10][4] = {
                    { p.base[0], p.base[1], p.base[2], p.radius },
                    { p.drift[0], 0, p.drift[2], p.height },
                    { riseSpeed * (p.kind > 0.5f ? 2.5f : 1.0f), time, grow, density * (p.kind > 0.5f ? 0.55f : 1.0f) },
                    { sun ? WosVolumetric::sunDirection[0] : 0, sun ? WosVolumetric::sunDirection[1] : 1, sun ? WosVolumetric::sunDirection[2] : 0, sun ? 1.0f : 0.0f },
                    { WosVolumetric::sunColour[0] * brightness, WosVolumetric::sunColour[1] * brightness, WosVolumetric::sunColour[2] * brightness, p.fade },
                    { WosVolumetric::fogColour[0] * brightness, WosVolumetric::fogColour[1] * brightness, WosVolumetric::fogColour[2] * brightness, float((std::max)(8, (std::min)(64, steps))) },
                    { tint[0], tint[1], tint[2], p.seed },
                    { p.box[0][0], p.box[0][1], p.box[0][2], 0 },
                    { p.box[1][0], p.box[1][1], p.box[1][2], 0 },
                    { noiseFrequency, noiseStrength, p.kind > 0.5f ? 3.0f : 0.6f, phaseG },
                };
                check(d->SetPixelShaderConstantF(4, &c[0][0], 10));
                if (drawn && DrawRect(d, rect, desc.Width, desc.Height)) ++plumesLastFrame;
                else drawn = false;
            }
            check(d->SetTexture(0, nullptr));
        }
        const bool restored = state.valid && state.Restore();
        if (!restored) { faulted = true; status = "Chimney smoke state restore failed; disabled for this session"; }
        else if (!drawn) { ++failures; status = "Chimney smoke pass failed this frame"; }
        else { ++framesDrawn; status = "Chimney smoke ON"; }
    } else if (ok) {
        status = "Chimney smoke: no chimney within range";
    } else {
        status = "Chimney smoke skipped: no scene texture, camera or depth";
    }
    if (target) target->Release();
    if (depth) depth->Release();
    if (hdr) hdr->Release();
    if (bound) bound->Release();
}

static unsigned Known(double now) {
    unsigned n = 0;
    for (const auto& e : emitters) if (now - e.seen <= keepSeconds) ++n;
    return n;
}

// Once a second, for checking the mod without a screenshot.
static ULONGLONG lastStatusWrite = 0;
static void WriteStatus() {
    const ULONGLONG tick = GetTickCount64();
    if (tick - lastStatusWrite < 1000 || directory.empty()) return;
    lastStatusWrite = tick;
    FILE* f = std::fopen((directory + "\\chimney_smoke_status.json").c_str(), "wb");
    if (!f) return;
    const double now = Now();
    std::fprintf(f, "{\"enabled\":%s,\"faulted\":%s,\"status\":\"%s\",\"frames_drawn\":%lu,\"plumes_last_frame\":%lu,\"failures\":%lu,"
                    "\"matched_draws\":%lu,\"rejected_world\":%lu,\"vs_layouts\":%lu,\"vs_layouts_usable\":%lu,\"emitters_known\":%u,\"emitters\":[",
                 enabled ? "true" : "false", faulted ? "true" : "false", status, framesDrawn, plumesLastFrame, failures,
                 matchedDraws, rejectedWorld, layoutsParsed, layoutsUsable, Known(now));
    bool first = true;
    for (const auto& e : emitters) {
        if (now - e.seen > keepSeconds) continue;
        std::fprintf(f, "%s{\"model\":\"%s\",\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,\"radius\":%.2f,\"seen_ago\":%.1f}", first ? "" : ",",
                     kModels[e.model].name, e.p[0], e.p[1], e.p[2], e.radius, now - e.seen);
        first = false;
    }
    std::fprintf(f, "]}\n");
    std::fclose(f);
}

} // namespace WosChimneySmoke
