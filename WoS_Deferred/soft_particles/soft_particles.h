#pragma once
// Soft particles for the engine's EFX effects.
//
// The EFX system draws every particle and effect decal with shader pairs that
// live inside the EXE (copies in native/): plain (texture * vertex colour),
// z-feathered (the *_zfeather blend modes: alpha fades with the scene depth in
// ZBufferSampler, s1) and decal (projects a box onto the scene depth in s1).
// On PC the engine never gives them real depth: it renders a lit material into
// an R32F target as its "zbuffertexture", or leaves s1 with whatever the last
// material bound, or nothing. A z-feathered particle whose s1 is empty comes out
// with alpha 0.
//
// 1. Depth. At the first EFX draw of a frame on the main view (16F scene target
//    with its INTZ depth), the INTZ is copied into an R32F texture.
// 2. Native fix. Z-feather and decal draws get that copy in s1, and the
//    z-feather pixel shader its HalfPixelOffset (c0). Their own math is kept.
// 3. Plain particles drawn with a blend mode go through a replacement pair that
//    fades them by linear view depth: alpha for SRCALPHA blends, colour and
//    alpha for ONE/ONE and premultiplied, towards white for multiply.
//
// Native shaders are recognised by their exact bytes when the game creates them.
// Include after native_indirect.h, indirect_camera.h and taa_runtime.h.
namespace WosSoftParticles {

// ------------------------------------------------------------------ settings
static bool enabled = true, fixNative = true, softenPlain = true, faulted = false;
static float range = 1.0f, rangePerMetre = 0.01f, strength = 1.0f, featherScale = 1.0f;
static const char* status = "Soft particles waiting for EFX draws";
static std::string directory;

template<class T> static void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ------------------------------------------------------------ native shaders
enum Kind { kNone = 0, kFeather = 1, kPlain = 2, kDecal = 3, kKinds = 4 };
static const char* kindNames[kKinds] = { "none", "zfeather", "plain", "decal" };
struct Known { uint64_t hash; UINT bytes; int kind; bool pixel; };
static const Known kKnown[] = {
    { 0x4CBBC02A539856B6ull, 496, kFeather, true },  { 0x0E8E60760FBF4460ull, 720, kFeather, false },
    { 0xDD8172D2F1DA7131ull, 220, kPlain, true },    { 0xD4530997FFE3C1DFull, 356, kPlain, false },
    { 0x5FA3E1120439FE3Dull, 1052, kDecal, true },   { 0x765741CE0682ACE4ull, 436, kDecal, false },
};
// Held for the process lifetime: a freed address reused by another shader must
// never be mistaken for an EFX one.
static std::vector<std::pair<IUnknown*, int>> pixelShaders, vertexShaders;

// Byte length of D3D9 shader bytecode, walking the tokens to the end token.
static size_t CodeBytes(const DWORD* code) {
    if (!code || (code[0] & 0xFFFE0000u) != 0xFFFE0000u) return 0;
    for (size_t i = 1; i < (1u << 16);) {
        const DWORD token = code[i];
        if (token == 0x0000FFFFu) return (i + 1) * 4;
        i += (token & 0xFFFFu) == 0xFFFEu ? 1 + (token >> 16) : 1 + ((token >> 24) & 0xFu);
    }
    return 0;
}
static uint64_t Fnv(const void* data, size_t bytes) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < bytes; ++i) h = (h ^ static_cast<const unsigned char*>(data)[i]) * 0x100000001B3ull;
    return h;
}
static int Identify(const DWORD* code, bool pixel) {
    const size_t bytes = CodeBytes(code);
    if (!bytes) return kNone;
    uint64_t h = 0;
    for (const Known& k : kKnown) {
        if (k.pixel != pixel || k.bytes != bytes) continue;
        if (!h) h = Fnv(code, bytes);
        if (h == k.hash) return k.kind;
    }
    return kNone;
}
// Called from the CreatePixelShader / CreateVertexShader hooks.
static void Register(IUnknown* shader, const DWORD* code, bool pixel) {
    if (!shader) return;
    const int kind = Identify(code, pixel);
    auto& list = pixel ? pixelShaders : vertexShaders;
    if (!kind || list.size() >= 256) return;
    shader->AddRef();
    list.emplace_back(shader, kind);
}
static int KindOf(const std::vector<std::pair<IUnknown*, int>>& list, IUnknown* shader) {
    if (!shader) return kNone;
    for (const auto& entry : list) if (entry.first == shader) return entry.second;
    return kNone;
}

// ---------------------------------------------------------------- diagnostics
struct Diagnostics {
    unsigned long seen[kKinds] = {}, handled[kKinds] = {};
    unsigned long otherVS = 0, notMain = 0, noCamera = 0, opaque = 0, blendUnsupported = 0, copyFailed = 0, drawFailed = 0;
    unsigned long copies = 0, nativeDepthEmpty = 0, nativeDepthR32F = 0, nativeDepthOther = 0;
};
static Diagnostics diag;
static ULONGLONG lastStatusWrite = 0;
static void WriteStatus() {
    const ULONGLONG now = GetTickCount64();
    if (now - lastStatusWrite < 1000 || directory.empty()) return;
    lastStatusWrite = now;
    const std::string path = directory.substr(0, directory.find_last_of("\\/")) + "\\soft_particles_status.json";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "{\"enabled\":%s,\"faulted\":%s,\"status\":\"%s\",\"registered\":{\"pixel\":%u,\"vertex\":%u},\"kinds\":{",
                 enabled ? "true" : "false", faulted ? "true" : "false", status, unsigned(pixelShaders.size()), unsigned(vertexShaders.size()));
    for (int k = 1; k < kKinds; ++k)
        std::fprintf(f, "%s\"%s\":{\"draws\":%lu,\"handled\":%lu}", k > 1 ? "," : "", kindNames[k], diag.seen[k], diag.handled[k]);
    std::fprintf(f, "},\"skipped\":{\"other_vs\":%lu,\"not_main_view\":%lu,\"no_camera\":%lu,\"opaque\":%lu,\"blend_unsupported\":%lu,"
                    "\"depth_copy_failed\":%lu,\"draw_failed\":%lu},\"depth_copies\":%lu,"
                    "\"engine_zbuffer_in_s1\":{\"empty\":%lu,\"r32f\":%lu,\"other\":%lu}}\n",
                 diag.otherVS, diag.notMain, diag.noCamera, diag.opaque, diag.blendUnsupported, diag.copyFailed, diag.drawFailed,
                 diag.copies, diag.nativeDepthEmpty, diag.nativeDepthR32F, diag.nativeDepthOther);
    std::fclose(f);
}

// ------------------------------------------------------------------ resources
static IDirect3DDevice9* owner = nullptr;
static IDirect3DVertexShader9* softVS = nullptr;
static IDirect3DPixelShader9* softPS = nullptr, * copyPS = nullptr;
static IDirect3DTexture9* depthCopy = nullptr;
static IDirect3DSurface9* depthCopyRT = nullptr;
static UINT width = 0, height = 0;
static bool shadersTried = false;
static int copyFrame = -1;
static bool copyValid = false;
static IUnknown* copySource = nullptr;

static void ReleaseTargets() {
    Release(depthCopyRT); Release(depthCopy); Release(copySource);
    width = height = 0; copyFrame = -1; copyValid = false;
}
static void Reset() {
    ReleaseTargets();
    Release(softVS); Release(softPS); Release(copyPS);
    owner = nullptr; shadersTried = false; faulted = false;
}
static bool Shaders(IDirect3DDevice9* d) {
    if (owner && owner != d) Reset();
    if (softVS && softPS && copyPS) return true;
    if (shadersTried) return false;
    shadersTried = true;
    auto read = [&](const char* name, uint32_t version, std::vector<unsigned char>& bytes) {
        bytes = WosCascadeProbe::ReadFile(directory + "\\" + name);
        uint32_t first = 0;
        if (bytes.size() < 8 || bytes.size() % 4) return false;
        std::memcpy(&first, bytes.data(), 4);
        return first == version;
    };
    std::vector<unsigned char> vs, ps, copy;
    const bool ok = read("soft_particles_vs.cso", 0xfffe0300u, vs) && read("soft_particles_ps.cso", 0xffff0300u, ps) &&
                    read("soft_particles_depth.cso", 0xffff0300u, copy) &&
                    SUCCEEDED(d->CreateVertexShader(reinterpret_cast<const DWORD*>(vs.data()), &softVS)) &&
                    SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(ps.data()), &softPS)) &&
                    SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(copy.data()), &copyPS));
    if (!ok) {
        Release(softVS); Release(softPS); Release(copyPS);
        faulted = true; status = "Soft particle shaders missing in WoS_Deferred/soft_particles";
        return false;
    }
    owner = d;
    return true;
}
static bool Targets(IDirect3DDevice9* d, UINT w, UINT h) {
    if (depthCopy && width == w && height == h) return true;
    ReleaseTargets();
    if (FAILED(d->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &depthCopy, nullptr)) ||
        FAILED(depthCopy->GetSurfaceLevel(0, &depthCopyRT))) {
        ReleaseTargets(); faulted = true; status = "Soft particle depth target allocation failed";
        return false;
    }
    width = w; height = h;
    return true;
}

// ---------------------------------------------------------------- main view
struct View {
    IDirect3DTexture9* depth = nullptr; // INTZ behind the bound depth surface
    IUnknown* identity = nullptr;
    UINT w = 0, h = 0;
    ~View() { Release(depth); Release(identity); }
};
static bool MainView(IDirect3DDevice9* d, View& v) {
    IDirect3DSurface9* rt = nullptr, * ds = nullptr;
    D3DSURFACE_DESC color = {}, depth = {};
    D3DVIEWPORT9 vp = {};
    const bool ok = SUCCEEDED(d->GetRenderTarget(0, &rt)) && rt && SUCCEEDED(rt->GetDesc(&color)) &&
                    color.Format == D3DFMT_A16B16G16R16F && color.MultiSampleType == D3DMULTISAMPLE_NONE &&
                    color.Width >= 640 && color.Height >= 200 && color.Width <= 4096 && color.Height <= 4096 &&
                    SUCCEEDED(d->GetDepthStencilSurface(&ds)) && ds && SUCCEEDED(ds->GetDesc(&depth)) &&
                    depth.Format == D3DFORMAT(MAKEFOURCC('I', 'N', 'T', 'Z')) && depth.Width == color.Width && depth.Height == color.Height &&
                    SUCCEEDED(d->GetViewport(&vp)) && !vp.X && !vp.Y && vp.Width == color.Width && vp.Height == color.Height &&
                    SUCCEEDED(ds->GetContainer(IID_IDirect3DTexture9, reinterpret_cast<void**>(&v.depth))) &&
                    SUCCEEDED(v.depth->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&v.identity)));
    Release(rt); Release(ds);
    v.w = color.Width; v.h = color.Height;
    return ok;
}
// One copy per frame and depth surface. EFX draws come after the opaque scene;
// later depth writes are sorted transparent geometry, which a feather can ignore.
static bool EnsureDepth(IDirect3DDevice9* d, const View& v) {
    if (copyFrame == g_frame_index && copySource == v.identity) return copyValid;
    if (!Targets(d, v.w, v.h)) return false;
    copyFrame = g_frame_index; copyValid = false;
    Release(copySource); copySource = v.identity; copySource->AddRef();
    WosIndirect::State state(d);
    if (!state.valid) { ++diag.copyFailed; return false; }
    bool ok = true;
    auto check = [&](HRESULT h) { ok = SUCCEEDED(h) && ok; };
    {
        check(d->SetDepthStencilSurface(nullptr));
        for (unsigned i = 1; i < state.slots; ++i) check(d->SetRenderTarget(i, nullptr));
        check(d->SetRenderTarget(0, depthCopyRT));
        check(d->SetVertexShader(nullptr)); check(d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1));
        const D3DRENDERSTATETYPE zeros[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE,
                                             D3DRS_SCISSORTESTENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE, D3DRS_CLIPPLANEENABLE, D3DRS_SEPARATEALPHABLENDENABLE };
        for (auto s : zeros) check(d->SetRenderState(s, 0));
        check(d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE)); check(d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID));
        check(d->SetRenderState(D3DRS_COLORWRITEENABLE, 15)); check(d->SetRenderState(D3DRS_MULTISAMPLEMASK, 0xffffffff));
        check(d->SetTexture(0, v.depth));
        check(d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT)); check(d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
        check(d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
        check(d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP)); check(d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
        check(d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, 0));
        check(g_original_set_pixel_shader(d, copyPS));
        if (ok) ok = WosIndirect::Quad(d, v.w, v.h);
        check(d->SetTexture(0, nullptr));
    }
    if (!state.Restore()) { faulted = true; status = "Soft particle depth copy restore failed; disabled for this session"; return false; }
    if (ok) ++diag.copies; else ++diag.copyFailed;
    copyValid = ok;
    return ok;
}

// ------------------------------------------------------------ bound state
// Sampler 1 is borrowed for the depth copy; everything it touches is put back.
struct SamplerSlot {
    static constexpr D3DSAMPLERSTATETYPE kStates[6] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER,
                                                        D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE };
    IDirect3DBaseTexture9* texture = nullptr;
    DWORD values[6] = {};
    bool valid = false;
    bool Save(IDirect3DDevice9* d, DWORD slot) {
        valid = SUCCEEDED(d->GetTexture(slot, &texture));
        for (int i = 0; i < 6 && valid; ++i) valid = SUCCEEDED(d->GetSamplerState(slot, kStates[i], &values[i]));
        return valid;
    }
    static bool Bind(IDirect3DDevice9* d, DWORD slot, IDirect3DBaseTexture9* t) {
        const DWORD point[6] = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, 0 };
        bool ok = SUCCEEDED(d->SetTexture(slot, t));
        for (int i = 0; i < 6; ++i) ok = SUCCEEDED(d->SetSamplerState(slot, kStates[i], point[i])) && ok;
        return ok;
    }
    bool Restore(IDirect3DDevice9* d, DWORD slot) {
        bool ok = SUCCEEDED(d->SetTexture(slot, texture));
        for (int i = 0; i < 6; ++i) ok = SUCCEEDED(d->SetSamplerState(slot, kStates[i], values[i])) && ok;
        return ok;
    }
    ~SamplerSlot() { Release(texture); }
};
static void NoteEngineDepth(IDirect3DBaseTexture9* t) {
    IDirect3DTexture9* texture = nullptr;
    D3DSURFACE_DESC desc = {};
    if (!t) ++diag.nativeDepthEmpty;
    else if (SUCCEEDED(t->QueryInterface(IID_IDirect3DTexture9, reinterpret_cast<void**>(&texture))) &&
             SUCCEEDED(texture->GetLevelDesc(0, &desc)) && desc.Format == D3DFMT_R32F) ++diag.nativeDepthR32F;
    else ++diag.nativeDepthOther;
    Release(texture);
}

// Mode weights for the replacement pixel shader: scale colour, colour towards
// white, scale alpha. False when a feather has no meaning for the blend.
static bool BlendMode(IDirect3DDevice9* d, float mode[4]) {
    DWORD blend = 0, src = 0, dst = 0;
    if (FAILED(d->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend)) || FAILED(d->GetRenderState(D3DRS_SRCBLEND, &src)) ||
        FAILED(d->GetRenderState(D3DRS_DESTBLEND, &dst))) return false;
    mode[0] = mode[1] = mode[2] = mode[3] = 0.0f;
    if (!blend) { ++diag.opaque; return false; }
    if (src == D3DBLEND_SRCALPHA) { mode[2] = 1.0f; return true; }                          // blend, additive, subtractive
    if (src == D3DBLEND_ONE && (dst == D3DBLEND_ONE || dst == D3DBLEND_INVSRCALPHA)) {      // plain add, premultiplied
        mode[0] = mode[2] = 1.0f; return true;
    }
    if ((src == D3DBLEND_ZERO && dst == D3DBLEND_SRCCOLOR) || (src == D3DBLEND_DESTCOLOR && dst == D3DBLEND_ZERO)) {
        mode[1] = 1.0f; return true;                                                        // multiply
    }
    ++diag.blendUnsupported;
    return false;
}

// 1 / view depth is affine in hardware z: 1/w = z * slope + offset. It is the w
// row of the frame camera's inverse view-projection. Without a camera, the
// ViewToScreen the last z-feather draw carried (c10 = z row, c11 = w row) gives
// the same two numbers.
static float learnedSlope = 0.0f, learnedOffset = 0.0f;
static bool ValidMapping(float slope, float offset) {
    return std::isfinite(slope) && std::isfinite(offset) && offset > 0.0f && slope + offset > 0.0f;
}
static void LearnMapping(IDirect3DDevice9* d) {
    float rows[8] = {};
    if (FAILED(d->GetVertexShaderConstantF(10, rows, 2))) return;
    const bool perspective = rows[0] == 0.0f && rows[1] == 0.0f && rows[4] == 0.0f && rows[5] == 0.0f &&
                             rows[6] == 1.0f && rows[7] == 0.0f && rows[3] < 0.0f;
    if (!perspective) return;
    const float slope = 1.0f / rows[3], offset = -rows[2] / rows[3];
    if (ValidMapping(slope, offset)) { learnedSlope = slope; learnedOffset = offset; }
}
static bool DepthMapping(const View& v, float& slope, float& offset) {
    WosIndirectCamera::Camera camera;
    if (WosIndirectCamera::Find(v.depth, v.w, v.h, camera, WosIndirectCamera::CurrentScene()) &&
        ValidMapping(camera.inverse[14], camera.inverse[15])) {
        slope = camera.inverse[14]; offset = camera.inverse[15];
        return true;
    }
    slope = learnedSlope; offset = learnedOffset;
    return ValidMapping(slope, offset);
}

// Native z-feather and decal: real depth in s1, the half-pixel offset the
// z-feather pixel shader was written for, optionally a scaled feather range.
template<class F> static HRESULT DrawNative(IDirect3DDevice9* d, int kind, const View& v, F&& draw) {
    SamplerSlot slot;
    float halfPixel[4] = {}, savedHalf[4] = {}, feather[4] = {}, savedFeather[4] = {};
    bool ok = slot.Save(d, 1);
    if (ok && kind == kFeather) { NoteEngineDepth(slot.texture); LearnMapping(d); }
    const bool scaleFeather = kind == kFeather && featherScale != 1.0f;
    if (ok && kind == kFeather) ok = SUCCEEDED(d->GetPixelShaderConstantF(0, savedHalf, 1));
    if (ok && scaleFeather) ok = SUCCEEDED(d->GetVertexShaderConstantF(12, savedFeather, 1));
    if (!ok) { ++diag.drawFailed; return draw(); }
    bool bound = SamplerSlot::Bind(d, 1, depthCopy);
    if (kind == kFeather) {
        halfPixel[0] = 0.5f / float(v.w); halfPixel[1] = 0.5f / float(v.h);
        bound = SUCCEEDED(d->SetPixelShaderConstantF(0, halfPixel, 1)) && bound;
    }
    if (scaleFeather) {
        std::memcpy(feather, savedFeather, sizeof(feather)); feather[0] *= featherScale;
        bound = SUCCEEDED(d->SetVertexShaderConstantF(12, feather, 1)) && bound;
    }
    const HRESULT hr = draw();
    bool restored = slot.Restore(d, 1);
    if (kind == kFeather) restored = SUCCEEDED(d->SetPixelShaderConstantF(0, savedHalf, 1)) && restored;
    if (scaleFeather) restored = SUCCEEDED(d->SetVertexShaderConstantF(12, savedFeather, 1)) && restored;
    if (!restored) { faulted = true; status = "Soft particle state restore failed; disabled for this session"; }
    if (bound && SUCCEEDED(hr)) ++diag.handled[kind]; else ++diag.drawFailed;
    return hr;
}

// Plain particles through the replacement pair. The bound vertex shader may be
// the TAA twin of the native one; then the same jitter goes into c4.
template<class F> static HRESULT DrawPlain(IDirect3DDevice9* d, IDirect3DVertexShader9* nativeVS, const View& v, F&& draw) {
    float mode[4], slope = 0.0f, offset = 0.0f;
    if (!BlendMode(d, mode)) return draw();
    if (!DepthMapping(v, slope, offset)) { ++diag.noCamera; return draw(); }
    SamplerSlot slot;
    IDirect3DVertexShader9* boundVS = nullptr;
    IDirect3DPixelShader9* boundPS = nullptr;
    float savedPS[12] = {}, savedJitter[4] = {};
    bool ok = slot.Save(d, 1) && SUCCEEDED(d->GetVertexShader(&boundVS)) && SUCCEEDED(d->GetPixelShader(&boundPS)) &&
              SUCCEEDED(d->GetPixelShaderConstantF(0, savedPS, 3)) && SUCCEEDED(d->GetVertexShaderConstantF(4, savedJitter, 1));
    if (!ok) { Release(boundVS); Release(boundPS); ++diag.drawFailed; return draw(); }
    const bool jittered = boundVS && boundVS != nativeVS && WosTaa::frame.active;
    const float jitter[4] = { jittered ? WosTaa::frame.ndc[0] : 0.0f, jittered ? WosTaa::frame.ndc[1] : 0.0f, 0.0f, 0.0f };
    const float constants[12] = {
        1.0f / float(v.w), 1.0f / float(v.h), slope, offset,
        range, rangePerMetre, strength, 0.0f,
        mode[0], mode[1], mode[2], 0.0f,
    };
    bool bound = SamplerSlot::Bind(d, 1, depthCopy) && SUCCEEDED(d->SetPixelShaderConstantF(0, constants, 3)) &&
                 SUCCEEDED(d->SetVertexShaderConstantF(4, jitter, 1)) &&
                 SUCCEEDED(g_original_set_vertex_shader(d, softVS)) && SUCCEEDED(g_original_set_pixel_shader(d, softPS));
    HRESULT hr;
    if (bound) hr = draw();
    else {
        // Put the engine's own pair back before drawing it.
        g_original_set_vertex_shader(d, boundVS); g_original_set_pixel_shader(d, boundPS);
        hr = draw();
    }
    bool restored = SUCCEEDED(g_original_set_vertex_shader(d, boundVS)) && SUCCEEDED(g_original_set_pixel_shader(d, boundPS));
    restored = slot.Restore(d, 1) && restored;
    restored = SUCCEEDED(d->SetPixelShaderConstantF(0, savedPS, 3)) && restored;
    restored = SUCCEEDED(d->SetVertexShaderConstantF(4, savedJitter, 1)) && restored;
    Release(boundVS); Release(boundPS);
    if (!restored) { faulted = true; status = "Soft particle state restore failed; disabled for this session"; }
    if (bound && SUCCEEDED(hr)) ++diag.handled[kPlain]; else ++diag.drawFailed;
    return hr;
}

// Classifies the draw while the engine's own vertex shader is still bound (the
// TAA scope binds its twin later); Draw wraps the innermost device call.
struct Scope {
    IDirect3DDevice9* d = nullptr;
    IDirect3DVertexShader9* nativeVS = nullptr;
    int kind = kNone;
    explicit Scope(IDirect3DDevice9* device) {
        if (!enabled || faulted || WosProbe::suppress || WosDeferred::busy || pixelShaders.empty()) return;
        const int ps = KindOf(pixelShaders, g_current_shader);
        if (!ps) return;
        ++diag.seen[ps];
        WriteStatus();
        if (ps == kPlain ? !softenPlain : !fixNative) return;
        IDirect3DVertexShader9* vs = nullptr;
        if (FAILED(device->GetVertexShader(&vs)) || KindOf(vertexShaders, vs) != ps) { Release(vs); ++diag.otherVS; return; }
        nativeVS = vs; kind = ps; d = device;
    }
    ~Scope() { Release(nativeVS); }
    template<class F> HRESULT Draw(F&& draw) {
        if (!kind || faulted) return draw();
        View v;
        if (!MainView(d, v)) { ++diag.notMain; return draw(); }
        if (!Shaders(d) || !EnsureDepth(d, v)) return draw();
        status = "Soft particles ON";
        return kind == kPlain ? DrawPlain(d, nativeVS, v, draw) : DrawNative(d, kind, v, draw);
    }
};

} // namespace WosSoftParticles
