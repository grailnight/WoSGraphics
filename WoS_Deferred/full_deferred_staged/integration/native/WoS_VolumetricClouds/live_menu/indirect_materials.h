#pragma once
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/indirect_guard_builder.h"

// Partial native material capture. Replacement shaders preserve the original
// oC0 instruction stream and append world-normal / native diffuse-rho outputs.
// This is current-frame geometry data, not a complete G-buffer.
namespace WosIndirectMaterials {
struct Variant {
    std::vector<unsigned char> input;
    IDirect3DPixelShader9* shader = nullptr;
    bool supported = false;
    IDirect3DPixelShader9* guardShader = nullptr;
    bool guardAttempted = false;
};
static std::vector<Variant> variants;
static std::unordered_map<IDirect3DPixelShader9*, int> cache;
static IDirect3DDevice9* owner = nullptr;
static IDirect3DTexture9* normalTexture = nullptr;
static IDirect3DTexture9* rhoTexture = nullptr;
static IDirect3DSurface9* normalSurface = nullptr;
static IDirect3DSurface9* rhoSurface = nullptr;
static IDirect3DSurface9* sceneColor = nullptr;
static IDirect3DSurface9* sceneDepthSurface = nullptr;
static IUnknown* sceneDepthIdentity = nullptr;
static uint32_t sceneId = 0;
static UINT width = 0, height = 0, mrtSlots = 0;
static bool loaded = false, available = false, invalidated = false;
static unsigned draws = 0, guardDraws = 0, replayDraws = 0, unknownDraws = 0, failures = 0;
static unsigned dynamicGuards = 0, dynamicGuardFailures = 0;
static std::string lastGuardReason;
static const char* status = "Native indirect material capture inactive";

static void BeginFrame() {
    if (sceneColor) sceneColor->Release();
    if (sceneDepthSurface) sceneDepthSurface->Release();
    if (sceneDepthIdentity) sceneDepthIdentity->Release();
    sceneColor = sceneDepthSurface = nullptr;
    sceneDepthIdentity = nullptr;
    sceneId = 0;
    available = invalidated = false;
    draws = guardDraws = replayDraws = unknownDraws = failures = 0;
}
static void ReleaseBuffers() {
    if (normalSurface) normalSurface->Release();
    if (rhoSurface) rhoSurface->Release();
    if (normalTexture) normalTexture->Release();
    if (rhoTexture) rhoTexture->Release();
    normalSurface = rhoSurface = nullptr;
    normalTexture = rhoTexture = nullptr;
    width = height = 0;
}
static void Reset() {
    BeginFrame();
    ReleaseBuffers();
    for (auto& entry : cache) entry.first->Release();
    cache.clear();
    for (auto& variant : variants) {
        if (variant.shader) variant.shader->Release();
        if (variant.guardShader) variant.guardShader->Release();
    }
    variants.clear();
    owner = nullptr;
    mrtSlots = 0;
    loaded = false;
    dynamicGuards = dynamicGuardFailures = 0;
    lastGuardReason.clear();
    status = "Native indirect material capture reset";
}
static std::vector<unsigned char> Read(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return {};
    std::fseek(file, 0, SEEK_END);
    long size = std::ftell(file);
    std::rewind(file);
    std::vector<unsigned char> bytes;
    if (size >= 4 && size <= 1024 * 1024 && !(size % 4)) {
        bytes.resize(static_cast<size_t>(size));
        if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) bytes.clear();
    }
    std::fclose(file);
    return bytes;
}
static bool LoadOne(IDirect3DDevice9* device, const std::string& base, bool supported) {
    auto input = Read(base + ".input.cso");
    auto output = Read(base + ".mrt.cso");
    if (input.empty() || output.empty()) return false;
    IDirect3DPixelShader9* shader = nullptr;
    if (FAILED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(output.data()), &shader))) return false;
    variants.push_back({std::move(input), shader, supported});
    return true;
}
static void Load(IDirect3DDevice9* device) {
    if (owner && owner != device) Reset();
    if (loaded) return;
    loaded = true;
    owner = device;
    D3DCAPS9 caps = {};
    if (FAILED(device->GetDeviceCaps(&caps)) || caps.NumSimultaneousRTs < 3) {
        status = "Native indirect materials require three MRT slots";
        return;
    }
    mrtSlots = (std::min)(caps.NumSimultaneousRTs, DWORD(4));
    const std::string directory = WosProbe::Root() + "\\indirect_materials\\";
    unsigned loadFailures = 0;
    for (int id : {9, 11, 12, 13, 17}) for (const char* mode : {"original", "pcss"}) {
        char name[64];
        std::snprintf(name, sizeof(name), "ps_%04d.%s", id, mode);
        if (!LoadOne(device, directory + name, true)) ++loadFailures;
    }
    // Supported exact matches must precede old generic occlusion guards with
    // the same input bytes. These files are independently GPU-validated.
    for (const char* name : {"ps_0078.generic_single_diffuse", "ps_0060.generic_single_diffuse_normal",
                             "ps_0028.generic_dual_diffuse_normal_specular", "ps_0045.skinned_transition",
                             "ps_0119.road_mix_diffuse", "ps_0086.bodycolor_diffuse"}) {
        if (!LoadOne(device, directory + name, true)) ++loadFailures;
    }
    const std::string guardDirectory = directory + "occlusion\\";
    WIN32_FIND_DATAA info = {};
    HANDLE search = FindFirstFileA((guardDirectory + "*.input.cso").c_str(), &info);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::string name(info.cFileName);
            if (name.size() <= 10 || variants.size() >= 512) continue;
            name.resize(name.size() - 10);
            if (!LoadOne(device, guardDirectory + name, false)) ++loadFailures;
        } while (FindNextFileA(search, &info));
        FindClose(search);
    } else {
        ++loadFailures;
    }
    status = loadFailures ? "Native indirect material variants incomplete" : "Native indirect material variants ready";
}
static IDirect3DPixelShader9* CreateGuard(IDirect3DDevice9* device, const std::vector<unsigned char>& input) {
    std::vector<unsigned char> output;
    if (!WosIndirectGuard::Build(input, output, lastGuardReason)) { ++dynamicGuardFailures; return nullptr; }
    IDirect3DPixelShader9* shader = nullptr;
    if (FAILED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(output.data()), &shader))) {
        if (shader) shader->Release();
        ++dynamicGuardFailures;
        lastGuardReason = "runtime guard rejected by Direct3D";
        return nullptr;
    }
    ++dynamicGuards;
    return shader;
}
static IDirect3DPixelShader9* GuardFor(IDirect3DDevice9* device, Variant& variant) {
    if (!variant.supported) return variant.shader;
    if (!variant.guardAttempted) {
        variant.guardAttempted = true;
        variant.guardShader = CreateGuard(device, variant.input);
    }
    return variant.guardShader;
}
static Variant* Find(IDirect3DDevice9* device, IDirect3DPixelShader9* shader) {
    if (!shader) return nullptr;
    Load(device);
    if (mrtSlots < 3) return nullptr;
    auto found = cache.find(shader);
    int index = -1;
    if (found != cache.end()) {
        index = found->second;
    } else {
        // Bound both retained native COM objects and generated variants. A full
        // cache rejects new shaders without changing the shader used by the game.
        if (cache.size() >= 512) { lastGuardReason = "native shader cache limit reached"; return nullptr; }
        UINT size = 0;
        if (SUCCEEDED(shader->GetFunction(nullptr, &size)) && size >= 8 && size <= 1024 * 1024 && !(size % 4)) {
            std::vector<unsigned char> bytes(size);
            if (SUCCEEDED(shader->GetFunction(bytes.data(), &size)) && size <= bytes.size()) {
                bytes.resize(size);
                for (size_t i = 0; i < variants.size(); ++i) {
                    if (bytes == variants[i].input) { index = static_cast<int>(i); break; }
                }
                if (index < 0) {
                    if (variants.size() >= 512) lastGuardReason = "guard variant limit reached";
                    else if (auto guard = CreateGuard(device, bytes)) {
                        index = static_cast<int>(variants.size());
                        variants.push_back({std::move(bytes), guard, false});
                    }
                }
            } else lastGuardReason = "native shader bytecode read failed";
        } else lastGuardReason = "invalid native shader bytecode size";
        // Prevent stale COM-address cache hits if the game destroys a shader.
        shader->AddRef();
        cache.emplace(shader, index);
    }
    return index >= 0 ? &variants[index] : nullptr;
}
static bool EnsureBuffers(IDirect3DDevice9* device, const D3DSURFACE_DESC& desc) {
    if (normalSurface && rhoSurface && width == desc.Width && height == desc.Height) return true;
    ReleaseBuffers();
    if (mrtSlots < 3) return false;
    bool ok = SUCCEEDED(device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET,
        D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &normalTexture, nullptr));
    if (ok) ok = SUCCEEDED(normalTexture->GetSurfaceLevel(0, &normalSurface));
    if (ok) ok = SUCCEEDED(device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET,
        D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &rhoTexture, nullptr));
    if (ok) ok = SUCCEEDED(rhoTexture->GetSurfaceLevel(0, &rhoSurface));
    if (!ok) { ReleaseBuffers(); return false; }
    width = desc.Width;
    height = desc.Height;
    return true;
}
static bool Start(IDirect3DDevice9* device, IDirect3DSurface9* color, IDirect3DSurface9* depth,
                  IUnknown* identity, uint32_t scene, const D3DSURFACE_DESC& desc, const D3DVIEWPORT9& viewport) {
    if (sceneColor) return color == sceneColor && identity == sceneDepthIdentity && scene == sceneId;
    if (!EnsureBuffers(device, desc)) return false;
    // Auxiliary targets were checked empty. Only our two surfaces are cleared;
    // the native HDR/depth contents and viewport survive even a failed clear.
    bool ok = SUCCEEDED(device->SetDepthStencilSurface(nullptr));
    if (ok) ok = SUCCEEDED(device->SetRenderTarget(0, normalSurface));
    if (ok) ok = SUCCEEDED(device->SetRenderTarget(1, rhoSurface));
    D3DVIEWPORT9 full = {0, 0, desc.Width, desc.Height, 0, 1};
    if (ok) ok = SUCCEEDED(device->SetViewport(&full));
    if (ok) ok = SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1, 0));
    ok = SUCCEEDED(device->SetRenderTarget(1, nullptr)) && ok;
    ok = SUCCEEDED(device->SetRenderTarget(0, color)) && ok;
    ok = SUCCEEDED(device->SetDepthStencilSurface(depth)) && ok;
    ok = SUCCEEDED(device->SetViewport(&viewport)) && ok;
    if (!ok) {
        invalidated = true;
        ++failures;
        status = "Native indirect material target initialization failed";
        return false;
    }
    color->AddRef();
    depth->AddRef();
    identity->AddRef();
    sceneColor = color;
    sceneDepthSurface = depth;
    sceneDepthIdentity = identity;
    sceneId = scene;
    return true;
}
static bool Get(IDirect3DTexture9* depth, uint32_t scene, IDirect3DTexture9*& normal, IDirect3DTexture9*& rho) {
    normal = rho = nullptr;
    if (!available || invalidated || !draws || !WosIndirectCamera::frameReady || !depth ||
        !scene || scene != sceneId || !normalTexture || !rhoTexture || !sceneDepthIdentity) return false;
    IUnknown* identity = nullptr;
    if (FAILED(depth->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&identity)))) return false;
    bool same = identity == sceneDepthIdentity;
    identity->Release();
    if (!same) return false;
    normal = normalTexture;
    rho = rhoTexture;
    return true; // Borrowed until BeginFrame / Reset; caller must not Release.
}
// Blend enable alone does not imply transparency. The native late pass keeps
// blending enabled with ONE/ZERO/ADD. Both RGB and alpha must be overwrite.
static bool OverwriteBlend(IDirect3DDevice9* d, DWORD enabled) {
    if (!enabled) return true;
    DWORD src=0,dst=0,op=0,separate=0;
    if (FAILED(d->GetRenderState(D3DRS_SRCBLEND,&src)) || src!=D3DBLEND_ONE ||
        FAILED(d->GetRenderState(D3DRS_DESTBLEND,&dst)) || dst!=D3DBLEND_ZERO ||
        FAILED(d->GetRenderState(D3DRS_BLENDOP,&op)) || op!=D3DBLENDOP_ADD ||
        FAILED(d->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,&separate))) return false;
    return !separate || (SUCCEEDED(d->GetRenderState(D3DRS_SRCBLENDALPHA,&src)) && src==D3DBLEND_ONE &&
        SUCCEEDED(d->GetRenderState(D3DRS_DESTBLENDALPHA,&dst)) && dst==D3DBLEND_ZERO &&
        SUCCEEDED(d->GetRenderState(D3DRS_BLENDOPALPHA,&op)) && op==D3DBLENDOP_ADD);
}
struct DrawScope {
    IDirect3DDevice9* device = nullptr;
    IDirect3DPixelShader9* original = nullptr;
    IDirect3DSurface9* color = nullptr;
    IDirect3DSurface9* depth = nullptr;
    IUnknown* identity = nullptr;
    DWORD write1 = 0, write2 = 0, write0 = 0, savedBlend = 0, savedZWrite = 0, savedZFunc = 0;
    IDirect3DPixelShader9* replayShader = nullptr; // Owned by exact-byte variant cache.
    bool changed = false, deferred = false, replayState = false, executed = false;
    explicit DrawScope(IDirect3DDevice9* d, bool enabled, bool allowReplay = true) {
        if (!enabled || WosProbe::suppress || !WosIndirectCamera::frameReady) return;
        uint32_t scene = WosIndirectCamera::CurrentScene();
        if (!scene || invalidated) return;
        DWORD blend = 0, zwrite = 0, ztest = 0, srgb = 0, colorWrite = 0;
        if (FAILED(d->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite)) || !zwrite ||
            FAILED(d->GetRenderState(D3DRS_ZENABLE, &ztest)) || !ztest) return;
        if (FAILED(d->GetRenderTarget(0, &color)) || !color ||
            FAILED(d->GetDepthStencilSurface(&depth)) || !depth) return;
        D3DSURFACE_DESC desc = {}, depthDesc = {};
        D3DVIEWPORT9 viewport = {};
        if (FAILED(color->GetDesc(&desc)) || desc.Format != D3DFMT_A16B16G16R16F ||
            desc.MultiSampleType != D3DMULTISAMPLE_NONE || desc.Width < 640 || desc.Width > 4096 || desc.Height > 4096 ||
            FAILED(depth->GetDesc(&depthDesc)) || depthDesc.Format != D3DFORMAT(MAKEFOURCC('I','N','T','Z')) ||
            depthDesc.MultiSampleType != D3DMULTISAMPLE_NONE || depthDesc.Width != desc.Width || depthDesc.Height != desc.Height) return;
        IDirect3DTexture9* container = nullptr;
        if (FAILED(depth->GetContainer(IID_IDirect3DTexture9, reinterpret_cast<void**>(&container))) || !container) return;
        HRESULT hr = container->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&identity));
        container->Release();
        if (FAILED(hr) || !identity) return;
        if (sceneColor && (scene != sceneId || identity != sceneDepthIdentity || color != sceneColor)) return;
        auto bypass = [&](const char* reason) {
            if (sceneColor) {
                invalidated = true;
                available = false;
                ++unknownDraws;
                status = reason;
            }
        };
        if (FAILED(d->GetViewport(&viewport)) || viewport.X || viewport.Y || viewport.Width != desc.Width ||
            viewport.Height != desc.Height || viewport.MinZ != 0 || viewport.MaxZ != 1 ||
            FAILED(d->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend)) ||
            FAILED(d->GetRenderState(D3DRS_SRGBWRITEENABLE, &srgb)) || srgb ||
            FAILED(d->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite))) {
            bypass("Native indirect materials skipped: incompatible depth-writing state");
            return;
        }
        // Find reads the actual current PS, including a preceding PCSS scope.
        if (FAILED(d->GetPixelShader(&original)) || !original) {
            bypass("Native indirect materials skipped: unrecognized depth-writing shader");
            return;
        }
        Variant* variant = Find(d, original);
        if (mrtSlots < 3) return;
        for (unsigned i = 1; i < mrtSlots; ++i) {
            IDirect3DSurface9* rt = nullptr;
            HRESULT result = d->GetRenderTarget(i, &rt);
            if (rt) {
                rt->Release();
                bypass("Native indirect materials skipped: auxiliary target owned by another pass");
                return;
            }
            if (FAILED(result) && result != D3DERR_NOTFOUND) {
                bypass("Native indirect materials skipped: auxiliary target query failed");
                return;
            }
        }
        if (!variant) {
            bypass("Native indirect materials skipped: unknown opaque occluder");
            return;
        }
        if (!OverwriteBlend(d,blend)) {
            if (!sceneColor) return; // No prior material coverage to invalidate.
            DWORD stencil=0,depthFunction=0;
            if (!allowReplay ||
                FAILED(d->GetRenderState(D3DRS_STENCILENABLE,&stencil)) || stencil ||
                FAILED(d->GetRenderState(D3DRS_ZFUNC,&depthFunction)) ||
                (depthFunction!=D3DCMP_LESSEQUAL && depthFunction!=D3DCMP_EQUAL &&
                 depthFunction!=D3DCMP_GREATEREQUAL && depthFunction!=D3DCMP_ALWAYS)) {
                bypass("Native indirect materials skipped: unsafe blended depth writer");
                return;
            }
            // The original blended draw must run with its original shader/state.
            // Only its just-written depth is replayed into our coverage targets.
            replayShader=GuardFor(d,*variant);
            if (!replayShader) {
                bypass("Native indirect materials skipped: blended guard unavailable");
                return;
            }
            device=d; deferred=true;
            return;
        }
        // An unsupported shader can only erase coverage already started by a
        // supported visible color draw. Depth-only early scenes cannot start it.
        if (!sceneColor && (!variant->supported || colorWrite != 15)) return;
        if (!Start(d, color, depth, identity, scene, desc, viewport)) return;
        if (FAILED(d->GetRenderState(D3DRS_COLORWRITEENABLE1, &write1)) ||
            FAILED(d->GetRenderState(D3DRS_COLORWRITEENABLE2, &write2))) {
            bypass("Native indirect materials skipped: write mask query failed");
            return;
        }
        device = d;
        changed = true;
        if (FAILED(d->SetRenderTarget(1, normalSurface)) || FAILED(d->SetRenderTarget(2, rhoSurface)) ||
            FAILED(d->SetRenderState(D3DRS_COLORWRITEENABLE1, 15)) ||
            FAILED(d->SetRenderState(D3DRS_COLORWRITEENABLE2, 15)) ||
            FAILED(g_original_set_pixel_shader(d, variant->shader))) {
            invalidated = true;
            available = false;
            ++failures;
            Restore();
            return;
        }
        // Constants such as c220 are local shader DEF literals. No host pixel
        // constants, samplers, original RT0 shader arithmetic or textures change.
        if (variant->supported) ++draws; else ++guardDraws;
        available = draws > 0;
        status = "Native world normals and diffuse rho captured (partial)";
    }
    void Fail(const char* reason) {
        ++failures; invalidated=true; available=false; status=reason;
    }
    template<class Draw> HRESULT Execute(Draw&& draw) {
        executed=true;
        HRESULT nativeResult=draw();
        if (FAILED(nativeResult)) {
            if (changed || deferred) Fail("Native indirect materials skipped: original draw failed");
            deferred=false; Restore(); return nativeResult;
        }
        if (!deferred) return nativeResult;
        deferred=false;
        // DP / DIP preserve bound streams and indices. UP hooks explicitly
        // disable replay because DrawUP clears these bindings after its call.
        if (FAILED(device->GetRenderState(D3DRS_COLORWRITEENABLE,&write0)) ||
            FAILED(device->GetRenderState(D3DRS_COLORWRITEENABLE1,&write1)) ||
            FAILED(device->GetRenderState(D3DRS_COLORWRITEENABLE2,&write2)) ||
            FAILED(device->GetRenderState(D3DRS_ALPHABLENDENABLE,&savedBlend)) ||
            FAILED(device->GetRenderState(D3DRS_ZWRITEENABLE,&savedZWrite)) ||
            FAILED(device->GetRenderState(D3DRS_ZFUNC,&savedZFunc))) {
            Fail("Native indirect materials skipped: guard replay state query failed");
            return nativeResult;
        }
        replayState=true; changed=true;
        bool ok=SUCCEEDED(device->SetRenderTarget(1,normalSurface));
        ok=SUCCEEDED(device->SetRenderTarget(2,rhoSurface))&&ok;
        ok=SUCCEEDED(device->SetRenderState(D3DRS_COLORWRITEENABLE,0))&&ok;
        ok=SUCCEEDED(device->SetRenderState(D3DRS_COLORWRITEENABLE1,15))&&ok;
        ok=SUCCEEDED(device->SetRenderState(D3DRS_COLORWRITEENABLE2,15))&&ok;
        ok=SUCCEEDED(device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE))&&ok;
        ok=SUCCEEDED(device->SetRenderState(D3DRS_ZWRITEENABLE,FALSE))&&ok;
        ok=SUCCEEDED(device->SetRenderState(D3DRS_ZFUNC,D3DCMP_EQUAL))&&ok;
        ok=SUCCEEDED(g_original_set_pixel_shader(device,replayShader))&&ok;
        if (ok) ok=SUCCEEDED(draw());
        if (ok) { ++guardDraws; ++replayDraws; }
        else Fail("Native indirect materials skipped: guard replay failed");
        Restore();
        // The game sees the result of its original draw. A diagnostic replay
        // failure only invalidates our material inputs; it cannot undo HDR.
        return nativeResult;
    }
    void Restore() {
        if (!changed) return;
        bool ok = SUCCEEDED(g_original_set_pixel_shader(device, original));
        ok = SUCCEEDED(device->SetRenderTarget(1, nullptr)) && ok;
        ok = SUCCEEDED(device->SetRenderTarget(2, nullptr)) && ok;
        ok = SUCCEEDED(device->SetRenderState(D3DRS_COLORWRITEENABLE1, write1)) && ok;
        ok = SUCCEEDED(device->SetRenderState(D3DRS_COLORWRITEENABLE2, write2)) && ok;
        if (replayState) {
            ok = SUCCEEDED(device->SetRenderState(D3DRS_COLORWRITEENABLE, write0)) && ok;
            ok = SUCCEEDED(device->SetRenderState(D3DRS_ALPHABLENDENABLE, savedBlend)) && ok;
            ok = SUCCEEDED(device->SetRenderState(D3DRS_ZWRITEENABLE, savedZWrite)) && ok;
            ok = SUCCEEDED(device->SetRenderState(D3DRS_ZFUNC, savedZFunc)) && ok;
            replayState=false;
        }
        changed = false;
        if (!ok) {
            ++failures;
            invalidated = true;
            available = false;
            status = "Native indirect material state restoration failed";
        }
    }
    ~DrawScope() {
        if (deferred && !executed) Fail("Native indirect materials skipped: guard callback not executed");
        Restore();
        if (original) original->Release();
        if (color) color->Release();
        if (depth) depth->Release();
        if (identity) identity->Release();
    }
};
}
namespace WosIndirectMaterials {
using ReceiverProvider=bool(*)(IDirect3DDevice9*,IDirect3DTexture9*,uint32_t,IDirect3DTexture9*&,IDirect3DTexture9*&);
static ReceiverProvider receiverProvider=nullptr;
static void(*receiverProviderReset)()=nullptr;
static void ResetGIProvider(){if(receiverProviderReset)receiverProviderReset();}
static bool ResolveForGI(IDirect3DDevice9*d,IDirect3DTexture9*depth,uint32_t scene,IDirect3DTexture9*&normal,IDirect3DTexture9*&rho){
    if(receiverProvider)return receiverProvider(d,depth,scene,normal,rho);
    return Get(depth,scene,normal,rho);
}
}
