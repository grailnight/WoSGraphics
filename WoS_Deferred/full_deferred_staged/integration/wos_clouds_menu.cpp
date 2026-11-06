#include "../cloud_sun_sync.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/smaa_native/smaa_runtime.h"
#include <windows.h>
#include <intrin.h>
#include <mmsystem.h>
#include <d3d9.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>
#include "B:/Games/Spider Man - Web of Shadows/WoS_VolumetricClouds/tools/imgui/imgui.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_VolumetricClouds/tools/imgui/backends/imgui_impl_win32.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_VolumetricClouds/tools/imgui/backends/imgui_impl_dx9.h"

static HMODULE g_module = nullptr;
static HMODULE g_real_winmm = nullptr;
static HWND g_window = nullptr;
static WNDPROC g_original_wndproc = nullptr;
static bool g_menu_visible = false;
static bool g_imgui_ready = false;
static IDirect3DPixelShader9 *g_current_shader = nullptr;
// The game grabs the mouse through DirectInput in exclusive mode, so the OS cursor
// never moves. We steer our own cursor from the raw DirectInput deltas instead.
static bool g_di_mouse_seen = false;
static float g_cursor_x = 0.0f;
static float g_cursor_y = 0.0f;
static bool g_cursor_placed = false;
static bool g_di_buttons[3] = {};
static float g_di_wheel = 0.0f;
static std::unordered_map<void *, int> g_di_devices;
// Camera tracker (F4 toggles). Records the first eight vertex shader constant
// registers of the first world draw in each frame, so the registers that follow
// the camera can be told apart from the ones that only look like a matrix.
struct CameraSample {
    int frame;
    float registers[32];
};
static std::vector<CameraSample> g_track;
static bool g_recording = false;
static bool g_track_sampled = false;
static int g_frame_index = 0;
static std::unordered_map<IDirect3DPixelShader9 *, int> g_shader_kinds;

// The sky shader still carries the old flat cloud layer, which the volumetric
// march replaced. Every preset already shipped it at zero opacity, so the
// sliders are gone and the constants below keep it switched off for good.

// World-space height fog and sun shafts, evaluated inside the bloom composite.
// Distances are in world units, matching the packed camera position.
struct LightingSettings {
    float fog_density = 0.45f;
    float fog_falloff = 0.012f;      // per world unit
    float fog_base_height = 0.0f;    // absolute world height of the fog base
    float fog_start = 6.0f;          // world units of clear near field
    float fog_max = 0.90f;
    float fog_color[3] = { 0.62f, 0.66f, 0.72f };
    float fog_style = 1.0f;          // share of the fog colour taken from the style's native fog
    float inscatter_color[3] = { 1.00f, 0.84f, 0.62f };
    float inscatter = 0.55f;
    float inscatter_focus = 6.0f;
    float sky_haze = 0.60f;

    float sun_dir[3] = { 0.265f, 0.866f, -0.424f };  // the engine's measured day sun
    float shaft_strength = 0.85f;
    float shaft_length = 0.55f;
    float shaft_threshold = 0.32f;

    float fov = 70.0f;               // vertical FOV in degrees
    bool enabled = true;
    int debug_view = 0;              // 0 off, 1 fog, 2 world height, 3 camera status

    // Tone mapper of the bloom composite, see native/wos_tonemap.hlsli.
    int tone_mode = 1;               // 0 GTA (original chain), 1 AgX, 2 ACES
    float exposure_ev = 0.0f;        // compensation on top of the auto exposure
    float exposure_min_ev = -8.0f;   // how far a bright scene may be stopped down
    float exposure_max_ev = 0.5f;    // how far a dark scene may be opened up
    float agx_punch = 0.0f;
    float grade_amount = 0.5f;       // share of the GTA film grade in AgX and ACES
} g_lighting;

// Captured render camera. The engine keeps it in vertex shader constants c0..c2
// of the world geometry shaders, packed as (axis.xyz, position.component), with
// the third row pointing away from the view direction.
static float g_cam_right[3] = { 1.0f, 0.0f, 0.0f };
static float g_cam_up[3] = { 0.0f, 1.0f, 0.0f };
static float g_cam_forward[3] = { 0.0f, 0.0f, 1.0f };
// The volumetric pass works in engine world units: street level is y = 0 and
// rooftops sit around y = 210, so the layer starts just above the skyline.
struct VolumetricSettings {
    float bottom = 260.0f;
    float thickness = 260.0f;
    float coverage = 0.52f;
    float size = 420.0f;
    float opacity = 0.95f;
    float detail = 0.45f;
    float brightness = 1.00f;
    float wind = 8.0f;
    // Share of the shadow-side light taken from the active sky palette, i.e.
    // from the WoSTweaks style, instead of the stock blue. Presets set it.
    float style_tint = 0.80f;
} g_volumetric;
// The frame's native sky palette: gFogColor, gHorizonColor and gZenithColor
// (c5..c7) of the rvb_skybox draw. WoSTweaks styles write their sky and fog
// colours into the engine, and this is where they surface on the GPU side, so
// capturing them here is what lets clouds and fog follow the active style
// without knowing which style it is. Held across frames without a sky draw.
struct StylePalette {
    float fog[3] = {};
    float horizon[3] = {};
    float zenith[3] = {};
    bool valid = false;
} g_style_palette;
// Refreshed once per frame by WosCascadeShadowBridge::Arm. The lighting
// constants below are built long before any deferred header is included, so
// the cloud attenuation reaches them through this value rather than a call.
static float g_cloud_sun_transmittance = 1.0f;
// Set by the draw that uploads the cloud layer constants and consumed once per
// presented frame by the frame-end bridge. Neither of the engine's own frame
// flags can be used for this: g_volumetric_seen is reset by every EndScene, and
// g_frame_index counts EndScene calls rather than presented frames, so both
// read false or stale by the time the bridge runs.
static bool g_cloud_params_seen = false;
// The exact c20/c21 payload of the draw above. The cloud shadow evaluates the
// same density field, so it must march the same layer, not a copy of the menu
// values that may have moved since.
static float g_cloud_layer[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
static float g_cloud_frame_wind = 0.0f;
static float g_cloud_shape[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
// One-frame dump (F6) of the post-processing chain: which pixel shaders the
// engine actually runs, and what is bound to their sampler stages. Needed
// because the depth-of-field pass this mod targeted turned out never to run.
static FILE *g_dump_file = nullptr;
static ULONGLONG g_dump_until = 0;
static std::unordered_map<IDirect3DPixelShader9 *, int> g_dumped_shaders;

static bool g_volumetric_seen = false;
static bool g_volumetric_last = false;

static float g_cam_pos[3] = {};
static bool g_cam_valid = false;
static bool g_cam_captured_this_frame = false;

// Depth texture and projection constants, borrowed from the depth-of-field blur
// pass. That pass is the only place the engine binds both together, and it runs
// before the bloom composite, so the bloom shader can reuse them.
static IDirect3DBaseTexture9 *g_depth_texture = nullptr;
static bool g_depth_captured_this_frame = false;
static float g_far_plane = 0.0f;
static float g_tan_half_x = 0.0f;
static float g_tan_half_y = 0.0f;
static bool g_frustum_valid = false;
static float g_proj_z[4] = {};
static bool g_depth_valid = false;

using Direct3DCreate9Fn = IDirect3D9 *(WINAPI *)(UINT);
using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
using ResetFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
using EndSceneFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *);
using SetPixelShaderFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, IDirect3DPixelShader9 *);
using DrawPrimitiveFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitiveFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawPrimitiveUPFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *, UINT);
using DrawIndexedPrimitiveUPFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT, UINT,
                                                              const void *, D3DFORMAT, const void *, UINT);

static Direct3DCreate9Fn g_original_d3d_create9 = nullptr;
static CreateDeviceFn g_original_create_device = nullptr;
static ResetFn g_original_reset = nullptr;
static EndSceneFn g_original_end_scene = nullptr;
static EndSceneFn g_original_begin_scene = nullptr;
static SetPixelShaderFn g_original_set_pixel_shader = nullptr;
using SetVertexShaderFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,IDirect3DVertexShader9*);
using SetTextureFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,DWORD,IDirect3DBaseTexture9*);
using CreatePixelShaderFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,const DWORD*,IDirect3DPixelShader9**);
using CreateVertexShaderFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,const DWORD*,IDirect3DVertexShader9**);
using CreateTextureFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,UINT,UINT,UINT,DWORD,D3DFORMAT,D3DPOOL,IDirect3DTexture9**,HANDLE*);
static SetVertexShaderFn g_original_set_vertex_shader=nullptr;
static SetTextureFn g_original_set_texture=nullptr;
static CreatePixelShaderFn g_original_create_pixel_shader=nullptr;
static CreateVertexShaderFn g_original_create_vertex_shader=nullptr;
static CreateTextureFn g_original_create_texture=nullptr;
static DrawPrimitiveFn g_original_draw_primitive = nullptr;
static DrawIndexedPrimitiveFn g_original_draw_indexed_primitive = nullptr;
static DrawPrimitiveUPFn g_original_draw_primitive_up = nullptr;
static DrawIndexedPrimitiveUPFn g_original_draw_indexed_primitive_up = nullptr;
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/renderer_probe.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/native_clear_probe.h"

static std::string ConfigPath()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(g_module, path, MAX_PATH);
    std::string result(path);
    const size_t slash = result.find_last_of("\\/");
    result.resize(slash == std::string::npos ? 0 : slash + 1);
    result += "WoS_VolumetricClouds\\clouds_menu.ini";
    return result;
}

static float ReadFloatIn(const char *section, const char *name, float fallback)
{
    char fallback_text[32] = {}, value[32] = {};
    std::snprintf(fallback_text, sizeof(fallback_text), "%.6f", fallback);
    GetPrivateProfileStringA(section, name, fallback_text, value, sizeof(value), ConfigPath().c_str());
    return static_cast<float>(std::atof(value));
}

static float ReadFloat(const char *name, float fallback)
{
    return ReadFloatIn("Clouds", name, fallback);
}

static void WriteFloatIn(const char *section, const char *name, float value)
{
    char text[32] = {};
    std::snprintf(text, sizeof(text), "%.6f", value);
    WritePrivateProfileStringA(section, name, text, ConfigPath().c_str());
}

static void ReadColour(const char *name, float *rgb)
{
    char key[64] = {};
    for (int channel = 0; channel < 3; ++channel) {
        std::snprintf(key, sizeof(key), "%s%d", name, channel);
        rgb[channel] = ReadFloatIn("Lighting", key, rgb[channel]);
    }
}

static void WriteColour(const char *name, const float *rgb)
{
    char key[64] = {};
    for (int channel = 0; channel < 3; ++channel) {
        std::snprintf(key, sizeof(key), "%s%d", name, channel);
        WriteFloatIn("Lighting", key, rgb[channel]);
    }
}

static void LoadLighting()
{
    g_lighting.fog_density = ReadFloatIn("Lighting", "FogDensity", g_lighting.fog_density);
    g_lighting.fog_falloff = ReadFloatIn("Lighting", "FogFalloff", g_lighting.fog_falloff);
    g_lighting.fog_base_height = ReadFloatIn("Lighting", "FogBaseHeight", g_lighting.fog_base_height);
    g_lighting.fog_start = ReadFloatIn("Lighting", "FogStart", g_lighting.fog_start);
    g_lighting.fog_max = ReadFloatIn("Lighting", "FogMax", g_lighting.fog_max);
    g_lighting.inscatter = ReadFloatIn("Lighting", "Inscatter", g_lighting.inscatter);
    g_lighting.inscatter_focus = ReadFloatIn("Lighting", "InscatterFocus", g_lighting.inscatter_focus);
    g_lighting.sky_haze = ReadFloatIn("Lighting", "SkyHaze", g_lighting.sky_haze);
    g_lighting.fog_style = ReadFloatIn("Lighting", "FogStyle", g_lighting.fog_style);
    g_lighting.shaft_strength = ReadFloatIn("Lighting", "ShaftStrength", g_lighting.shaft_strength);
    g_lighting.shaft_length = ReadFloatIn("Lighting", "ShaftLength", g_lighting.shaft_length);
    g_lighting.shaft_threshold = ReadFloatIn("Lighting", "ShaftThreshold", g_lighting.shaft_threshold);
    g_lighting.fov = ReadFloatIn("Lighting", "Fov", g_lighting.fov);
    g_lighting.enabled = ReadFloatIn("Lighting", "Enabled", g_lighting.enabled ? 1.0f : 0.0f) > 0.5f;
    for (int axis = 0; axis < 3; ++axis) {
        char key[64] = {};
        std::snprintf(key, sizeof(key), "SunDir%d", axis);
        g_lighting.sun_dir[axis] = ReadFloatIn("Lighting", key, g_lighting.sun_dir[axis]);
    }
    ReadColour("FogColor", g_lighting.fog_color);
    ReadColour("InscatterColor", g_lighting.inscatter_color);
    g_lighting.tone_mode = static_cast<int>(ReadFloatIn("Tonemap", "Mode", static_cast<float>(g_lighting.tone_mode)) + 0.5f);
    if (g_lighting.tone_mode < 0 || g_lighting.tone_mode > 2) g_lighting.tone_mode = 1;
    g_lighting.exposure_ev = ReadFloatIn("Tonemap", "ExposureEV", g_lighting.exposure_ev);
    g_lighting.exposure_min_ev = ReadFloatIn("Tonemap", "MinEV", g_lighting.exposure_min_ev);
    g_lighting.exposure_max_ev = ReadFloatIn("Tonemap", "MaxEV", g_lighting.exposure_max_ev);
    g_lighting.agx_punch = ReadFloatIn("Tonemap", "AgxPunch", g_lighting.agx_punch);
    g_lighting.grade_amount = ReadFloatIn("Tonemap", "GradeAmount", g_lighting.grade_amount);
}

static void SaveLighting()
{
    WriteFloatIn("Lighting", "FogDensity", g_lighting.fog_density);
    WriteFloatIn("Lighting", "FogFalloff", g_lighting.fog_falloff);
    WriteFloatIn("Lighting", "FogBaseHeight", g_lighting.fog_base_height);
    WriteFloatIn("Lighting", "FogStart", g_lighting.fog_start);
    WriteFloatIn("Lighting", "FogMax", g_lighting.fog_max);
    WriteFloatIn("Lighting", "Inscatter", g_lighting.inscatter);
    WriteFloatIn("Lighting", "InscatterFocus", g_lighting.inscatter_focus);
    WriteFloatIn("Lighting", "SkyHaze", g_lighting.sky_haze);
    WriteFloatIn("Lighting", "FogStyle", g_lighting.fog_style);
    WriteFloatIn("Lighting", "ShaftStrength", g_lighting.shaft_strength);
    WriteFloatIn("Lighting", "ShaftLength", g_lighting.shaft_length);
    WriteFloatIn("Lighting", "ShaftThreshold", g_lighting.shaft_threshold);
    WriteFloatIn("Lighting", "Fov", g_lighting.fov);
    WriteFloatIn("Lighting", "Enabled", g_lighting.enabled ? 1.0f : 0.0f);
    for (int axis = 0; axis < 3; ++axis) {
        char key[64] = {};
        std::snprintf(key, sizeof(key), "SunDir%d", axis);
        WriteFloatIn("Lighting", key, g_lighting.sun_dir[axis]);
    }
    WriteColour("FogColor", g_lighting.fog_color);
    WriteColour("InscatterColor", g_lighting.inscatter_color);
    WriteFloatIn("Tonemap", "Mode", static_cast<float>(g_lighting.tone_mode));
    WriteFloatIn("Tonemap", "ExposureEV", g_lighting.exposure_ev);
    WriteFloatIn("Tonemap", "MinEV", g_lighting.exposure_min_ev);
    WriteFloatIn("Tonemap", "MaxEV", g_lighting.exposure_max_ev);
    WriteFloatIn("Tonemap", "AgxPunch", g_lighting.agx_punch);
    WriteFloatIn("Tonemap", "GradeAmount", g_lighting.grade_amount);
}

static void LoadSettings()
{
    g_volumetric.bottom = ReadFloat("VolumeBottom", g_volumetric.bottom);
    g_volumetric.thickness = ReadFloat("VolumeThickness", g_volumetric.thickness);
    g_volumetric.coverage = ReadFloat("VolumeCoverage", g_volumetric.coverage);
    g_volumetric.size = ReadFloat("VolumeSize", g_volumetric.size);
    g_volumetric.opacity = ReadFloat("VolumeOpacity", g_volumetric.opacity);
    g_volumetric.detail = ReadFloat("VolumeDetail", g_volumetric.detail);
    g_volumetric.brightness = ReadFloat("VolumeBrightness", g_volumetric.brightness);
    g_volumetric.wind = ReadFloat("VolumeWind", g_volumetric.wind);
    g_volumetric.style_tint = ReadFloat("StyleTint", g_volumetric.style_tint);
    LoadLighting();
}

static void WriteFloat(const char *name, float value)
{
    char text[32] = {};
    std::snprintf(text, sizeof(text), "%.6f", value);
    WritePrivateProfileStringA("Clouds", name, text, ConfigPath().c_str());
}

static void SaveSettings()
{
    WriteFloat("VolumeBottom", g_volumetric.bottom);
    WriteFloat("VolumeThickness", g_volumetric.thickness);
    WriteFloat("VolumeCoverage", g_volumetric.coverage);
    WriteFloat("VolumeSize", g_volumetric.size);
    WriteFloat("VolumeOpacity", g_volumetric.opacity);
    WriteFloat("VolumeDetail", g_volumetric.detail);
    WriteFloat("VolumeBrightness", g_volumetric.brightness);
    WriteFloat("VolumeWind", g_volumetric.wind);
    WriteFloat("StyleTint", g_volumetric.style_tint);
    SaveLighting();
}

// --- presets -----------------------------------------------------------------
// Built-in presets are compiled in (embedded_presets.h, generated from
// WoS_VolumetricClouds/presets_builtin). Presets the player saves are ini files
// in WoS_VolumetricClouds\presets in the same format, listed after them.
// A preset only ever names cloud keys: lighting, tone mapper and the deferred
// renderer keep whatever the player tuned.
#include "embedded_presets.h"

struct SettingKey {
    const char *section;
    const char *name;
    float *value;
};

static std::vector<SettingKey> SettingKeys()
{
    return {
        { "Clouds", "VolumeBottom", &g_volumetric.bottom },
        { "Clouds", "VolumeThickness", &g_volumetric.thickness },
        { "Clouds", "VolumeCoverage", &g_volumetric.coverage },
        { "Clouds", "VolumeSize", &g_volumetric.size },
        { "Clouds", "VolumeOpacity", &g_volumetric.opacity },
        { "Clouds", "VolumeDetail", &g_volumetric.detail },
        { "Clouds", "VolumeBrightness", &g_volumetric.brightness },
        { "Clouds", "VolumeWind", &g_volumetric.wind },
        { "Clouds", "StyleTint", &g_volumetric.style_tint },
    };
}

struct PresetEntry {
    std::string name;
    std::string description;
    std::string text;
    bool builtin = false;
};
static std::vector<PresetEntry> g_presets;
static int g_selected_preset = -1;
static char g_preset_name[64] = "Мой пресет";
static std::string g_preset_status;

static std::string PresetDirectory()
{
    std::string path = ConfigPath();
    path.resize(path.find_last_of("\\/") + 1);
    return path + "presets\\";
}

// Minimal ini reader for preset text: "section.key" -> value.
static std::unordered_map<std::string, std::string> ParseIni(const std::string &text)
{
    std::unordered_map<std::string, std::string> values;
    std::string section;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == ';' || line[first] == '#') continue;
        line.erase(0, first);
        if (line[0] == '[') {
            section = line.substr(1, line.find(']') == std::string::npos ? std::string::npos : line.find(']') - 1);
            for (char &c : section) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            continue;
        }
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        std::string key = line.substr(0, equals);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        for (char &c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        values[section + "." + key] = line.substr(equals + 1);
    }
    return values;
}

static std::string IniValue(const std::unordered_map<std::string, std::string> &values, const char *key,
                            const std::string &fallback)
{
    const auto found = values.find(key);
    return found == values.end() ? fallback : found->second;
}

static void AddPreset(const std::string &text, const std::string &fallback_name, bool builtin)
{
    const auto values = ParseIni(text);
    PresetEntry entry;
    entry.name = IniValue(values, "preset.name", fallback_name);
    entry.description = IniValue(values, "preset.description", "");
    entry.text = text;
    entry.builtin = builtin;
    g_presets.push_back(entry);
}

static void RefreshPresets()
{
    g_presets.clear();
    for (const EmbeddedPreset &preset : kEmbeddedPresets)
        AddPreset(preset.text, preset.file, true);

    std::vector<std::string> files;
    const std::string directory = PresetDirectory();
    WIN32_FIND_DATAA found = {};
    HANDLE search = FindFirstFileA((directory + "*.ini").c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(found.cFileName);
        } while (FindNextFileA(search, &found));
        FindClose(search);
    }
    std::sort(files.begin(), files.end(),
              [](const std::string &a, const std::string &b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
    for (const std::string &file : files) {
        HANDLE handle = CreateFileA((directory + file).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) continue;
        std::string text;
        char buffer[4096];
        DWORD read = 0;
        while (ReadFile(handle, buffer, sizeof(buffer), &read, nullptr) && read > 0 && text.size() < 65536)
            text.append(buffer, read);
        CloseHandle(handle);
        AddPreset(text, file.substr(0, file.size() - 4), false);
    }
    if (g_selected_preset >= static_cast<int>(g_presets.size())) g_selected_preset = -1;
}

static void ApplyPreset(const PresetEntry &preset)
{
    // Cloud keys the preset leaves out fall back to the defaults, not to
    // whatever the previous preset set, so a preset always looks the same no
    // matter which one preceded it.
    g_volumetric = VolumetricSettings{};
    const auto values = ParseIni(preset.text);
    for (const SettingKey &key : SettingKeys()) {
        std::string name = std::string(key.section) + "." + key.name;
        for (char &c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const auto found = values.find(name);
        if (found == values.end()) continue;
        const float parsed = static_cast<float>(std::atof(found->second.c_str()));
        if (std::isfinite(parsed)) *key.value = parsed;
    }
    SaveSettings();
    g_preset_status = "Применён: " + preset.name;
}

static void SaveCurrentAsPreset()
{
    // Cyrillic names reach this as UTF-8 and are stored byte for byte; the
    // list reads the file back raw, so they come out of the file as typed.
    std::string file;
    for (const char *c = g_preset_name; *c; ++c) {
        const unsigned char ch = static_cast<unsigned char>(*c);
        file.push_back(std::isalnum(ch) || ch >= 0x80 || ch == '-' || ch == '_' ? static_cast<char>(ch) : '_');
    }
    if (file.empty()) {
        g_preset_status = "Сначала введите имя пресета.";
        return;
    }
    const std::string path = PresetDirectory() + file + ".ini";
    for (size_t slash = path.find_first_of("\\/", 3); slash != std::string::npos;
         slash = path.find_first_of("\\/", slash + 1))
        CreateDirectoryA(path.substr(0, slash).c_str(), nullptr);
    for (const SettingKey &key : SettingKeys()) {
        char text[32] = {};
        std::snprintf(text, sizeof(text), "%.6f", *key.value);
        WritePrivateProfileStringA(key.section, key.name, text, path.c_str());
    }
    WritePrivateProfileStringA("Preset", "Name", g_preset_name, path.c_str());
    WritePrivateProfileStringA("Preset", "Description", "Сохранено из меню игры.", path.c_str());
    RefreshPresets();
    g_preset_status = "Сохранено: WoS_VolumetricClouds\\presets\\" + file + ".ini";
}


static bool ContainsMarker(const void *data, size_t size, const char *marker)
{
    const char *bytes = static_cast<const char *>(data);
    const size_t marker_size = std::strlen(marker);
    if (size < marker_size) return false;
    for (size_t i = 0; i + marker_size <= size; ++i)
        if (std::memcmp(bytes + i, marker, marker_size) == 0) return true;
    return false;
}

// Shaders are identified by the constant names left in their CTAB rather than by
// a bytecode hash, so rebuilding any of them does not silently break the hook.
enum ShaderKind {
    kShaderOther = 0,
    kShaderCloud,   // skybox variants carrying the live cloud constants
    kShaderDepth,   // depth-of-field blur: binds the depth texture and gProjMatrixZ
    kShaderBloom,   // our bloom composite, which wants camera, depth and fog
    kShaderVolumetric, // depth-of-field slot rebuilt as the world-space cloud march
};

// Set once any of our exWoS shaders is created; every effect hangs off them.
static bool g_mod_shaders_seen = false;

static int ClassifyShader(IDirect3DPixelShader9 *shader)
{
    if (!shader) return kShaderOther;
    // A draw asks about the same shader several times over; remember the last.
    static IDirect3DPixelShader9 *last_shader = nullptr;
    static int last_kind = kShaderOther;
    if (shader == last_shader) return last_kind;
    const auto found = g_shader_kinds.find(shader);
    if (found != g_shader_kinds.end()) { last_shader = shader; last_kind = found->second; return last_kind; }

    UINT size = 0;
    int kind = kShaderOther;
    if (SUCCEEDED(shader->GetFunction(nullptr, &size)) && size > 0) {
        std::vector<unsigned char> bytecode(size);
        if (SUCCEEDED(shader->GetFunction(bytecode.data(), &size))) {
            const void *code = bytecode.data();
            // Checked first: the volumetric pass carries the cloud constants too,
            // and it still has to feed the depth texture to the bloom composite.
            if (ContainsMarker(code, size, "gCloudCamera"))
                kind = kShaderVolumetric;
            // Bloom also has cloud constants: match its specific marker first.
            else if (ContainsMarker(code, size, "gWoSFogParams"))
                kind = kShaderBloom;
            else if (ContainsMarker(code, size, "gCloudParams") &&
                     ContainsMarker(code, size, "gCloudShape"))
                kind = kShaderCloud;
            else if (ContainsMarker(code, size, "gDepthSampler") &&
                     ContainsMarker(code, size, "gProjMatrixZ"))
                kind = kShaderDepth;
        }
    }
    g_shader_kinds.emplace(shader, kind);
    if (kind != kShaderOther) g_mod_shaders_seen = true;
    last_shader = shader; last_kind = kind;
    return kind;
}

static bool IsCloudShader(IDirect3DPixelShader9 *shader)
{
    return ClassifyShader(shader) == kShaderCloud;
}

// Only the gradient sky (rvb_skybox) declares the palette; the cube-map skybox
// is a cloud shader too but leaves c5..c7 to whatever drew before it.
static std::unordered_map<IDirect3DPixelShader9 *, bool> g_sky_palette_shaders;

static bool HasSkyPalette(IDirect3DPixelShader9 *shader)
{
    if (!shader) return false;
    const auto found = g_sky_palette_shaders.find(shader);
    if (found != g_sky_palette_shaders.end()) return found->second;
    bool has = false;
    UINT size = 0;
    if (SUCCEEDED(shader->GetFunction(nullptr, &size)) && size > 0) {
        std::vector<unsigned char> bytecode(size);
        if (SUCCEEDED(shader->GetFunction(bytecode.data(), &size)))
            has = ContainsMarker(bytecode.data(), size, "gHorizonColor") &&
                  ContainsMarker(bytecode.data(), size, "gZenithColor") &&
                  ContainsMarker(bytecode.data(), size, "gFogColor");
    }
    g_sky_palette_shaders.emplace(shader, has);
    return has;
}

static void CaptureStylePalette(IDirect3DDevice9 *device)
{
    if (!HasSkyPalette(g_current_shader)) return;
    float c[12] = {};
    if (FAILED(device->GetPixelShaderConstantF(5, c, 3))) return;
    for (int i = 0; i < 12; ++i)
        if (!std::isfinite(c[i]) || c[i] > 1000.0f) return;
    for (int i = 0; i < 3; ++i) {
        g_style_palette.fog[i] = (std::max)(c[i], 0.0f);
        g_style_palette.horizon[i] = (std::max)(c[4 + i], 0.0f);
        g_style_palette.zenith[i] = (std::max)(c[8 + i], 0.0f);
    }
    g_style_palette.valid = true;
}

// The sky shader's own chroma expansion (SkyPalette in rvb_skybox_gradient.hlsl),
// so the clouds pick up the hue the player actually sees in the sky.
static void ExpandSkyPalette(const float *color, float chroma, float scale, float *out)
{
    const float peak = (std::max)((std::max)(color[0], color[1]), color[2]);
    for (int i = 0; i < 3; ++i)
        out[i] = peak > 1e-5f ? std::pow(color[i] / peak, chroma) * peak * scale : 0.0f;
}

// Shadow-side cloud light for c17. The stock blue and the style colour meet at
// the same brightness on the engine's default day sky (horizon ~0.55/0.65/0.65,
// zenith ~0.68/0.76/0.86 gives about 0.22/0.34/0.42), so a slider at 1.0 on an
// unstyled day barely moves; a sunset or night style moves it a lot.
static void CloudAmbientConstant(float *out)
{
    const float stock[3] = { 0.24f, 0.30f, 0.40f };
    const float amount = (std::min)((std::max)(g_volumetric.style_tint, 0.0f), 1.0f);
    for (int i = 0; i < 3; ++i) out[i] = stock[i];
    out[3] = 0.0f;
    if (!g_style_palette.valid || amount <= 0.0f) return;
    float horizon[3], zenith[3];
    ExpandSkyPalette(g_style_palette.horizon, 2.4f, 1.0f, horizon);
    ExpandSkyPalette(g_style_palette.zenith, 4.0f, 0.68f, zenith);
    for (int i = 0; i < 3; ++i) {
        const float style = 0.7f * (0.4f * horizon[i] + 0.6f * zenith[i]);
        out[i] = stock[i] + (style - stock[i]) * amount;
    }
    out[3] = 11.0f;
}

static std::string TrackPath()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(g_module, path, MAX_PATH);
    std::string result(path);
    const size_t slash = result.find_last_of("\\/");
    result.resize(slash == std::string::npos ? 0 : slash + 1);
    result += "WoS_VolumetricClouds\\camera_track.csv";
    return result;
}

static void WriteTrack()
{
    FILE *file = std::fopen(TrackPath().c_str(), "w");
    if (!file) return;
    std::fprintf(file, "frame");
    for (int reg = 0; reg < 8; ++reg)
        std::fprintf(file, ",c%dx,c%dy,c%dz,c%dw", reg, reg, reg, reg);
    std::fprintf(file, "\n");
    for (const CameraSample &sample : g_track) {
        std::fprintf(file, "%d", sample.frame);
        for (int index = 0; index < 32; ++index)
            std::fprintf(file, ",%.5f", sample.registers[index]);
        std::fprintf(file, "\n");
    }
    std::fclose(file);
}

static void ToggleRecording()
{
    g_recording = !g_recording;
    if (g_recording) {
        g_track.clear();
        g_track.reserve(2048);
    } else {
        WriteTrack();
    }
}

static void ProbeDraw(IDirect3DDevice9 *device, UINT primitives)
{
    if (!g_recording || g_track_sampled || primitives < 300) return;
    if (g_track.size() >= 2048) {
        g_recording = false;
        WriteTrack();
        return;
    }

    CameraSample sample = {};
    sample.frame = g_frame_index;
    if (FAILED(device->GetVertexShaderConstantF(0, sample.registers, 8))) return;
    g_track.push_back(sample);
    g_track_sampled = true;
}

static float ElapsedSeconds()
{
    static const ULONGLONG start = GetTickCount64();
    return static_cast<float>(GetTickCount64() - start) * 0.001f;
}

// Feeds the world-space march: layer geometry in c20/c21 and the camera the
// engine never exposes to pixel shaders in c22.
static void ApplyVolumetricConstants(IDirect3DDevice9 *device)
{
    g_volumetric_seen = true;

    const float size = g_volumetric.size < 20.0f ? 20.0f : g_volumetric.size;
    const float wind = g_cloud_frame_wind;
    const float params[4] = { g_volumetric.bottom, g_volumetric.coverage, size, g_volumetric.opacity };
    const float shape[4] = { g_volumetric.thickness, g_volumetric.detail, g_volumetric.brightness, wind };
    const float camera[4] = { g_cam_pos[0], g_cam_pos[1], g_cam_pos[2], g_cam_valid ? 1.0f : 0.0f };
    device->SetPixelShaderConstantF(20, params, 1);
    device->SetPixelShaderConstantF(21, shape, 1);
    device->SetPixelShaderConstantF(22, camera, 1);
}

// gCloudParams/gCloudShape drive the retired flat layer inside the sky shader.
// The fourth component of gCloudParams is its opacity: zero, so the layer never
// contributes, and the registers are never left to whatever drew before us.
static void ApplyCloudConstants(IDirect3DDevice9 *device)
{
    CaptureStylePalette(device);
    const float params[4] = { 0.030f, 0.66f, 0.66f, 0.0f };
    const float shape[4] = { 0.090f, 0.52f, 1.0f, 0.0f };
    device->SetPixelShaderConstantF(20, params, 1);
    device->SetPixelShaderConstantF(21, shape, 1);
}

// Only an orthonormal basis is accepted, which rejects the lightmap and particle
// matrices that otherwise occupy the same registers.
static bool IsOrthonormal(const float *rows)
{
    for (int row = 0; row < 3; ++row) {
        const float *r = rows + row * 4;
        const float length = r[0] * r[0] + r[1] * r[1] + r[2] * r[2];
        if (std::fabs(length - 1.0f) > 0.01f) return false;
    }
    for (int first = 0; first < 3; ++first) {
        const float *a = rows + first * 4;
        const float *b = rows + ((first + 1) % 3) * 4;
        if (std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) > 0.01f) return false;
    }
    return true;
}

// Locate the named pixel constant in CTAB. Vertex c0..c2 can be a skinning
// palette (the old tracker was following the player skeleton, not the camera).
template<typename Shader> static int FindCameraMatrixRegister(Shader *shader, const char *nameToFind, UINT minimumRegisters)
{
    static std::unordered_map<Shader *, int> cache;
    auto found = cache.find(shader);
    if (found != cache.end()) return found->second;
    UINT size = 0;
    int result = -1;
    if (!shader || FAILED(shader->GetFunction(nullptr, &size))) return -1;
    std::vector<unsigned char> code(size);
    if (FAILED(shader->GetFunction(code.data(), &size))) return -1;
    for (size_t at = 0; at + 32 <= size; at += 4) {
        if (std::memcmp(code.data() + at, "CTAB", 4)) continue;
        const size_t base = at + 4, available = size - base;
        auto u32 = [&](size_t offset) { unsigned value; std::memcpy(&value, code.data() + base + offset, 4); return value; };
        auto u16 = [&](size_t offset) { unsigned short value; std::memcpy(&value, code.data() + base + offset, 2); return value; };
        const size_t count = u32(12), info = u32(16);
        if (info > available || count > (available - info) / 20) break;
        for (size_t i = 0; i < count; ++i) {
            const size_t entry = info + i * 20, name = u32(entry);
            if (name > available || available - name < (std::strlen(nameToFind) + 1)) continue;
            if (std::memcmp(code.data() + base + name, nameToFind, (std::strlen(nameToFind) + 1))) continue;
            // Float registers; three column-major registers contain camera XYZ in W.
            const size_t type = u32(entry + 12);
            if (type > available || available - type < 8) continue;
            if (u16(entry + 4) == 2 && u16(entry + 8) >= minimumRegisters &&
                u16(type) == 3 && u16(type + 4) == 4 && u16(type + 6) == 4 && u16(entry + 6) <= 221)
                result = u16(entry + 6);
        }
        break;
    }
    cache.emplace(shader, result);
    return result;
}

static float g_inverse_view_projection[16] = {};
static bool g_inverse_projection_valid = false;

static bool CaptureInverseProjection(IDirect3DDevice9 *device)
{
    IDirect3DVertexShader9 *shader = nullptr;
    if (FAILED(device->GetVertexShader(&shader)) || !shader) return false;
    int reg = FindCameraMatrixRegister(shader, "viewProj", 4);
    shader->Release();
    if (reg < 0) return false;
    float packed[16];
    if (FAILED(device->GetVertexShaderConstantF(reg, packed, 4))) return false;
    // Column-major HLSL constants are rows of the column-vector world->clip map.
    double a[4][8] = {};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) a[r][c] = packed[r * 4 + c];
        a[r][r + 4] = 1.0;
    }
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
        if (std::fabs(a[pivot][c]) < 1e-10) return false;
        for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[pivot][k]);
        const double scale = a[c][c];
        for (int k = 0; k < 8; ++k) a[c][k] /= scale;
        for (int r = 0; r < 4; ++r) if (r != c) {
            const double factor = a[r][c];
            for (int k = 0; k < 8; ++k) a[r][k] -= factor * a[c][k];
        }
    }
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) {
        if (!std::isfinite(a[r][c + 4])) return false;
        g_inverse_view_projection[r * 4 + c] = static_cast<float>(a[r][c + 4]);
    }
    return true;
}
static void CaptureCamera(IDirect3DDevice9 *device, UINT primitives)
{
    // The sky shader supplies the actual view basis, independently of the player.
    if (ClassifyShader(g_current_shader) == kShaderCloud) {
        float axes[12] = {};
        if (SUCCEEDED(device->GetPixelShaderConstantF(0, axes, 3)) && IsOrthonormal(axes)) {
            for (int i = 0; i < 3; ++i) {
                g_cam_right[i] = axes[i];
                g_cam_up[i] = axes[4 + i];
                g_cam_forward[i] = axes[8 + i];
            }
            g_cam_valid = g_cam_captured_this_frame;
        }
        return;
    }
    if (g_cam_captured_this_frame || primitives <= 2) return;
    const int reg = FindCameraMatrixRegister(g_current_shader, "viewInverse", 3);
    if (reg < 0) return;
    float columns[12] = {};
    if (FAILED(device->GetPixelShaderConstantF(reg, columns, 3))) return;
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(columns[i * 4 + 3])) return;
    for (int i = 0; i < 3; ++i) g_cam_pos[i] = columns[i * 4 + 3];
    g_inverse_projection_valid = CaptureInverseProjection(device);
    g_cam_captured_this_frame = g_inverse_projection_valid;
}
// The depth-of-field blur pass is the only place the engine binds the depth
// texture together with the projection constants needed to linearise it. Both
// are borrowed here and handed to the bloom composite later in the frame.
// The engine resolves the scene depth in a fullscreen pass that binds the INTZ
// depth texture at stage 0. The same pass carries the frustum corners in c3/c4
// and the depth ratio in c11.x, which is everything the world reconstruction
// needs - and it runs before the bloom composite that consumes them.
//
//   c3 = (min.x, min.y, z, 1), c4 = (max.x, max.y, z, 1)  frustum rectangle at z
//   c11.x = near / (far - near)
//
// The depth-of-field pass this data used to come from is never executed by the
// engine, which is why the fog was running blind.
static const D3DFORMAT kFormatINTZ = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));

static void CaptureSceneDepth(IDirect3DDevice9 *device, UINT primitives)
{
    if (g_depth_captured_this_frame || primitives > 2) return;

    D3DVIEWPORT9 viewport = {};
    if (FAILED(device->GetViewport(&viewport)) || viewport.Width < 640) return;

    IDirect3DBaseTexture9 *base = nullptr;
    if (FAILED(device->GetTexture(0, &base)) || !base) return;

    // Reject the 2048x2048 shadow map, which is INTZ as well.
    bool matches = false;
    IDirect3DTexture9 *texture = nullptr;
    if (SUCCEEDED(base->QueryInterface(IID_IDirect3DTexture9, reinterpret_cast<void **>(&texture)))) {
        D3DSURFACE_DESC desc = {};
        if (SUCCEEDED(texture->GetLevelDesc(0, &desc)))
            matches = desc.Format == kFormatINTZ && desc.Width == viewport.Width && desc.Height == viewport.Height;
        texture->Release();
    }
    if (!matches) {
        base->Release();
        return;
    }

    float bounds[8] = {};
    if (FAILED(device->GetPixelShaderConstantF(3, bounds, 2)) || bounds[2] <= 1.0f) {
        base->Release();
        return;
    }

    float ratio[4] = {};
    device->GetPixelShaderConstantF(11, ratio, 1); // Legacy value; no longer used for world depth.

    if (base != g_depth_texture) {
        if (g_depth_texture) g_depth_texture->Release();
        g_depth_texture = base; // GetTexture already added a reference
    } else {
        base->Release();
    }

    g_far_plane = bounds[2];
    g_tan_half_x = bounds[4] / bounds[2];
    // The projection is very slightly off centre vertically, so the half height
    // is the average of the two corners rather than either one.
    g_tan_half_y = 0.5f * (std::fabs(bounds[1]) + std::fabs(bounds[5])) / bounds[2];
    g_frustum_valid = true;

    // viewZ = projZ.w / ((1 - raw) - projZ.z), matching wos_world.hlsli.
    g_proj_z[2] = -ratio[0];
    g_proj_z[3] = ratio[0] * g_far_plane;

    g_depth_valid = true;
    g_depth_captured_this_frame = true;
}

static void CaptureDepthResources(IDirect3DDevice9 *device)
{
    IDirect3DBaseTexture9 *texture = nullptr;
    if (FAILED(device->GetTexture(1, &texture)) || !texture) return;

    if (texture != g_depth_texture) {
        if (g_depth_texture) g_depth_texture->Release();
        g_depth_texture = texture; // GetTexture already added a reference
    } else {
        texture->Release();
    }

    if (SUCCEEDED(device->GetPixelShaderConstantF(6, g_proj_z, 1)))
        g_depth_valid = true;
}

static void ReleaseDepthResources()
{
    if (g_depth_texture) {
        g_depth_texture->Release();
        g_depth_texture = nullptr;
    }
    g_depth_valid = false;
}

static void ApplyLightingConstants(IDirect3DDevice9 *device)
{
    D3DVIEWPORT9 viewport = {};
    const bool have_viewport = SUCCEEDED(device->GetViewport(&viewport)) && viewport.Height > 0;
    const float aspect = have_viewport
        ? static_cast<float>(viewport.Width) / static_cast<float>(viewport.Height)
        : 16.0f / 9.0f;
    // Measured from the engine's own frustum corners when available; the slider
    // is only a fallback for the frames before the depth pass is seen.
    float tan_y = std::tan(g_lighting.fov * 0.5f * 3.14159265f / 180.0f);
    float tan_x = tan_y * aspect;
    if (g_frustum_valid) {
        tan_x = g_tan_half_x;
        tan_y = g_tan_half_y;
    }

    const float right[4] = { g_cam_right[0], g_cam_right[1], g_cam_right[2], g_cam_pos[0] };
    const float up[4] = { g_cam_up[0], g_cam_up[1], g_cam_up[2], g_cam_pos[1] };
    const float forward[4] = { g_cam_forward[0], g_cam_forward[1], g_cam_forward[2], g_cam_pos[2] };
    const float projection[4] = { g_proj_z[2], g_proj_z[3], tan_x, tan_y };
    const float fog[4] = { g_lighting.fog_density, g_lighting.fog_falloff,
                           g_lighting.fog_base_height, g_lighting.fog_start };
    // The style's native fog replaces the menu colour by fog_style; the menu
    // colour stays the fallback until a gradient sky has been drawn.
    float fog_rgb[3] = { g_lighting.fog_color[0], g_lighting.fog_color[1], g_lighting.fog_color[2] };
    if (g_style_palette.valid) {
        const float amount = (std::min)((std::max)(g_lighting.fog_style, 0.0f), 1.0f);
        for (int i = 0; i < 3; ++i) fog_rgb[i] += (g_style_palette.fog[i] - fog_rgb[i]) * amount;
    }
    const float fog_color[4] = { fog_rgb[0], fog_rgb[1], fog_rgb[2], g_lighting.fog_max };
    const float cloudSunTransmittance = g_cloud_sun_transmittance;
    const float inscatter[4] = { g_lighting.inscatter_color[0], g_lighting.inscatter_color[1],
                                 g_lighting.inscatter_color[2], g_lighting.inscatter * cloudSunTransmittance };
    float sun[4], cloudRadiance[4];
    WosCloudSunSync::Constants(g_lighting.sun_dir,g_lighting.inscatter_focus,sun,cloudRadiance);
    device->SetPixelShaderConstantF(16,cloudRadiance,1);
    float cloudAmbient[4];
    CloudAmbientConstant(cloudAmbient);
    device->SetPixelShaderConstantF(17, cloudAmbient, 1);
    const float shafts[4] = { g_lighting.shaft_strength * cloudSunTransmittance, g_lighting.shaft_length,
                              g_lighting.shaft_threshold, g_lighting.sky_haze };

    const bool ready = g_lighting.enabled && g_cam_valid && g_depth_valid && g_inverse_projection_valid;
    const float status[4] = { ready ? 1.0f : 0.0f,
                              static_cast<float>(g_lighting.debug_view),
                              g_cam_valid ? 1.0f : 0.0f,
                              g_depth_valid ? 1.0f : 0.0f };

    device->SetPixelShaderConstantF(22, right, 1);
    device->SetPixelShaderConstantF(23, up, 1);
    device->SetPixelShaderConstantF(24, forward, 1);
    device->SetPixelShaderConstantF(25, projection, 1);
    device->SetPixelShaderConstantF(26, fog, 1);
    device->SetPixelShaderConstantF(27, fog_color, 1);
    device->SetPixelShaderConstantF(28, inscatter, 1);
    device->SetPixelShaderConstantF(29, sun, 1);
    device->SetPixelShaderConstantF(30, shafts, 1);
    device->SetPixelShaderConstantF(31, status, 1);
    device->SetPixelShaderConstantF(32, g_inverse_view_projection, 4);

    // w = 7 marks the registers as written by this hook; the shader falls back
    // to the GTA chain on anything else.
    const float tone[4] = { static_cast<float>(g_lighting.tone_mode), g_lighting.exposure_ev,
                            g_lighting.grade_amount, 7.0f };
    const float exposure[4] = { g_lighting.exposure_min_ev, g_lighting.exposure_max_ev,
                                g_lighting.agx_punch, 0.0f };
    device->SetPixelShaderConstantF(14, tone, 1);
    device->SetPixelShaderConstantF(15, exposure, 1);

    const float size = g_volumetric.size < 20.0f ? 20.0f : g_volumetric.size;
    const float wind = g_cloud_frame_wind;
    const float cloud_params[4] = { g_volumetric.bottom, g_volumetric.coverage, size, g_volumetric.opacity };
    const float cloud_shape[4] = { g_volumetric.thickness, g_volumetric.detail, g_volumetric.brightness, wind };
    device->SetPixelShaderConstantF(20, cloud_params, 1);
    device->SetPixelShaderConstantF(21, cloud_shape, 1);
    g_volumetric_seen = true;
    g_cloud_params_seen = true;
    for (int i = 0; i < 4; ++i) { g_cloud_layer[i] = cloud_params[i]; g_cloud_shape[i] = cloud_shape[i]; }

    if (g_depth_texture) {
        device->SetTexture(3, g_depth_texture);
        // The depth surface must not be filtered or wrapped, and the engine's
        // state for this stage belongs to whatever used it last.
        device->SetSamplerState(3, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(3, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        device->SetSamplerState(3, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(3, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(3, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
}

// Dispatches per-draw work according to which shader is currently bound.
static std::string DumpPath()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(g_module, path, MAX_PATH);
    std::string result(path);
    const size_t slash = result.find_last_of("\\/");
    result.resize(slash == std::string::npos ? 0 : slash + 1);
    result += "WoS_VolumetricClouds\\post_chain.txt";
    return result;
}

// Constant and sampler names survive in the shader's CTAB, so printing the
// readable strings identifies a pass without needing the engine's symbols.
static void DumpShaderNames(IDirect3DPixelShader9 *shader)
{
    UINT size = 0;
    if (FAILED(shader->GetFunction(nullptr, &size)) || size == 0) return;
    std::vector<char> bytecode(size);
    if (FAILED(shader->GetFunction(bytecode.data(), &size))) return;

    std::fprintf(g_dump_file, "    names:");
    std::string word;
    int printed = 0;
    for (UINT i = 0; i < size && printed < 24; ++i) {
        const char c = bytecode[i];
        if (c >= 0x20 && c < 0x7F) {
            word.push_back(c);
            continue;
        }
        if (word.size() >= 4 && (std::isalpha(static_cast<unsigned char>(word[0])) || word[0] == '_')) {
            std::fprintf(g_dump_file, " %s", word.c_str());
            ++printed;
        }
        word.clear();
    }
    std::fprintf(g_dump_file, "\n");
}

static void DumpBoundTextures(IDirect3DDevice9 *device)
{
    for (DWORD stage = 0; stage < 8; ++stage) {
        IDirect3DBaseTexture9 *base = nullptr;
        if (FAILED(device->GetTexture(stage, &base)) || !base) continue;

        IDirect3DTexture9 *texture = nullptr;
        if (SUCCEEDED(base->QueryInterface(IID_IDirect3DTexture9, reinterpret_cast<void **>(&texture)))) {
            D3DSURFACE_DESC desc = {};
            if (SUCCEEDED(texture->GetLevelDesc(0, &desc))) {
                const DWORD format = static_cast<DWORD>(desc.Format);
                char fourcc[5] = {};
                for (int i = 0; i < 4; ++i) {
                    const char c = static_cast<char>((format >> (i * 8)) & 0xFF);
                    fourcc[i] = (c >= 0x20 && c < 0x7F) ? c : '.';
                }
                std::fprintf(g_dump_file, "    s%lu  %ux%u  format %lu (%s)\n",
                             stage, desc.Width, desc.Height, format, fourcc);
            }
            texture->Release();
        } else {
            std::fprintf(g_dump_file, "    s%lu  (not a 2D texture)\n", stage);
        }
        base->Release();
    }
}

// Fullscreen passes are drawn as one or two triangles, which keeps the dump to
// the post-processing chain instead of every piece of city geometry.
static void DumpConstants(IDirect3DDevice9 *device, bool vertex, int registers)
{
    std::vector<float> values(static_cast<size_t>(registers) * 4, 0.0f);
    const HRESULT result = vertex
        ? device->GetVertexShaderConstantF(0, values.data(), registers)
        : device->GetPixelShaderConstantF(0, values.data(), registers);
    if (FAILED(result)) return;

    std::fprintf(g_dump_file, "    %s constants:\n", vertex ? "vertex" : "pixel");
    for (int reg = 0; reg < registers; ++reg) {
        const float *v = &values[static_cast<size_t>(reg) * 4];
        // Skip empty registers so the interesting rows stay findable.
        if (v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f && v[3] == 0.0f) continue;
        std::fprintf(g_dump_file, "      c%-3d % 12.5f % 12.5f % 12.5f % 12.5f\n",
                     reg, v[0], v[1], v[2], v[3]);
    }
}

static void DumpPostDraw(IDirect3DDevice9 *device, UINT primitives)
{
    if (!g_dump_file) return;
    // One entry per distinct pixel shader keeps the frame readable while still
    // covering every pass, whatever primitive count it happens to use.
    if (g_dumped_shaders.find(g_current_shader) != g_dumped_shaders.end()) return;

    const int index = static_cast<int>(g_dumped_shaders.size());
    g_dumped_shaders.emplace(g_current_shader, index);

    std::fprintf(g_dump_file, "pass %d  shader %p  %u primitives\n", index, (void *)g_current_shader, primitives);
    if (g_current_shader) DumpShaderNames(g_current_shader);
    DumpBoundTextures(device);
    // Fullscreen passes carry the post-processing constants, including whatever
    // the engine uses to linearise the depth buffer.
    if (primitives <= 2) DumpConstants(device, false, 16);
    if (primitives > 500 && g_dumped_shaders.size() < 4) DumpConstants(device, true, 96);
    std::fprintf(g_dump_file, "\n");
}

static void BeginDump()
{
    if (g_dump_file) return;
    g_dumped_shaders.clear();
    g_dump_file = std::fopen(DumpPath().c_str(), "w");
    g_dump_until = GetTickCount64() + 400;
}

static void MaybeEndDump()
{
    if (!g_dump_file) return;
    if (GetTickCount64() < g_dump_until && g_dumped_shaders.size() < 250) return;
    std::fprintf(g_dump_file, "%d distinct pixel shaders\n", static_cast<int>(g_dumped_shaders.size()));
    std::fclose(g_dump_file);
    g_dump_file = nullptr;
}

static void ApplyShaderConstants(IDirect3DDevice9 *device, UINT primitives)
{
    CaptureCamera(device, primitives);

    DumpPostDraw(device, primitives);
    CaptureSceneDepth(device, primitives);

    switch (ClassifyShader(g_current_shader)) {
    case kShaderCloud: ApplyCloudConstants(device); break;
    case kShaderDepth: CaptureDepthResources(device); break;
    case kShaderVolumetric:
        CaptureDepthResources(device);
        ApplyVolumetricConstants(device);
        break;
    case kShaderBloom: ApplyLightingConstants(device); break;
    default: break;
    }
}

// Swatches of the live sky palette, which is where a WoSTweaks style shows up.
// HDR values are clamped only for display; the hover text has the real ones.
static void DrawStylePalette()
{
    if (!g_style_palette.valid) {
        ImGui::TextDisabled("Стиль WoSTweaks: небо ещё не отрисовано, облака в стандартных цветах.");
        return;
    }
    const auto swatch = [](const char *label, const float *rgb) {
        const float peak = (std::max)((std::max)(rgb[0], rgb[1]), (std::max)(rgb[2], 1.0f));
        ImGui::ColorButton(label, ImVec4(rgb[0] / peak, rgb[1] / peak, rgb[2] / peak, 1.0f),
                           ImGuiColorEditFlags_NoTooltip, ImVec2(18.0f, 18.0f));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s: %.3f  %.3f  %.3f", label, rgb[0], rgb[1], rgb[2]);
        ImGui::SameLine();
    };
    float ambient[4];
    CloudAmbientConstant(ambient);
    ImGui::TextUnformatted("Стиль WoSTweaks:");
    ImGui::SameLine();
    swatch("Горизонт", g_style_palette.horizon);
    swatch("Зенит", g_style_palette.zenith);
    swatch("Туман", g_style_palette.fog);
    const float sun[3] = { WosCloudSunSync::color[0], WosCloudSunSync::color[1], WosCloudSunSync::color[2] };
    swatch("Солнце", sun);
    ImGui::TextUnformatted("->");
    ImGui::SameLine();
    swatch("Тень облаков", ambient);
    ImGui::NewLine();
}

static void DrawPresetTab(bool &changed)
{
    ImGui::TextWrapped("Выберите пресет и нажмите «Применить». Пресет меняет только облака — "
                       "освещение, тонмаппинг и остальной рендер остаются как настроено.");
    ImGui::TextWrapped("Цвета берутся из активного стиля WoSTweaks (Environment RGB): солнечная сторона — "
                       "из его света, теневая — из его неба. Сколько неба брать, задаёт пресет "
                       "(«Цвет от стиля» на вкладке облаков). Стиль можно менять в любой момент, пресет "
                       "применять заново не нужно.");
    DrawStylePalette();
    ImGui::Spacing();

    const float list_height = ImGui::GetTextLineHeightWithSpacing() * 9.0f;
    if (ImGui::BeginListBox("##presets", ImVec2(-1.0f, list_height))) {
        for (int index = 0; index < static_cast<int>(g_presets.size()); ++index) {
            ImGui::PushID(index);
            const bool selected = index == g_selected_preset;
            if (ImGui::Selectable(g_presets[index].name.c_str(), selected,
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                g_selected_preset = index;
                if (ImGui::IsMouseDoubleClicked(0)) { ApplyPreset(g_presets[index]); changed = true; }
            }
            ImGui::PopID();
        }
        ImGui::EndListBox();
    }

    const bool have_selection = g_selected_preset >= 0 && g_selected_preset < static_cast<int>(g_presets.size());
    if (have_selection && !g_presets[g_selected_preset].description.empty())
        ImGui::TextWrapped("%s", g_presets[g_selected_preset].description.c_str());

    ImGui::BeginDisabled(!have_selection);
    if (ImGui::Button("Применить")) { ApplyPreset(g_presets[g_selected_preset]); changed = true; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Обновить список")) RefreshPresets();
    ImGui::SameLine();
    if (ImGui::Button("Сбросить облака")) {
        g_volumetric = VolumetricSettings{};
        changed = true;
        g_preset_status = "Возвращены стандартные облака.";
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("Сохранить текущие настройки облаков");
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("##preset_name", g_preset_name, sizeof(g_preset_name));
    ImGui::SameLine();
    if (ImGui::Button("Сохранить как пресет")) SaveCurrentAsPreset();
    if (!g_preset_status.empty()) ImGui::TextWrapped("%s", g_preset_status.c_str());
}

static void DrawCloudLowResSection();

static void DrawVolumeTab(bool &changed)
{
    if (!g_volumetric_last)
        ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.35f, 1.0f),
                           "Пасс depth_of_field_blur не вызывается движком");
    else if (!g_cam_valid)
        ImGui::TextColored(ImVec4(1.0f, 0.70f, 0.20f, 1.0f), "Камера не найдена");
    else
        ImGui::TextColored(ImVec4(0.45f, 0.90f, 0.45f, 1.0f), "Камера: %.0f  %.0f  %.0f",
                           g_cam_pos[0], g_cam_pos[1], g_cam_pos[2]);
    ImGui::Separator();

    changed |= ImGui::SliderFloat("Низ слоя", &g_volumetric.bottom, 0.0f, 3000.0f, "%.0f");
    changed |= ImGui::SliderFloat("Толщина", &g_volumetric.thickness, 20.0f, 1500.0f, "%.0f");
    changed |= ImGui::SliderFloat("Покрытие", &g_volumetric.coverage, 0.0f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Размер облаков", &g_volumetric.size, 80.0f, 3000.0f, "%.0f");
    changed |= ImGui::SliderFloat("Непрозрачность", &g_volumetric.opacity, 0.0f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Мелкие детали", &g_volumetric.detail, 0.0f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Яркость", &g_volumetric.brightness, 0.45f, 1.55f, "%.2f");
    changed |= ImGui::SliderFloat("Ветер", &g_volumetric.wind, 0.0f, 120.0f, "%.1f");
    changed |= ImGui::SliderFloat("Цвет от стиля", &g_volumetric.style_tint, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("0 — стандартная голубая тень, 1 — тень целиком в цветах неба стиля WoSTweaks.");
    DrawStylePalette();

    ImGui::Spacing();
    if (ImGui::Button("У крыш")) {
        g_volumetric.bottom = 230.0f;
        g_volumetric.thickness = 200.0f;
        g_volumetric.coverage = 0.58f;
        g_volumetric.size = 340.0f;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Высоко")) {
        g_volumetric.bottom = 520.0f;
        g_volumetric.thickness = 320.0f;
        g_volumetric.coverage = 0.45f;
        g_volumetric.size = 620.0f;
        changed = true;
    }
    ImGui::TextWrapped("Слой задаётся в мировых единицах: улица — 0, крыши — около 210.");
    DrawCloudLowResSection();
}

static void DrawLightingTab(bool &changed)
{
    // The fog needs both the camera and the borrowed depth texture. Say plainly
    // which one is missing rather than leaving a dead slider panel.
    if (!g_cam_valid || !g_depth_valid) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "Нет данных: %s%s",
                           g_cam_valid ? "" : "камера ",
                           g_depth_valid ? "" : "глубина");
        ImGui::TextWrapped("Глубина берётся с пасса depth-of-field. Если её нет, "
                           "включите DOF в настройках игры.");
    } else {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Камера: %.0f %.0f %.0f",
                           g_cam_pos[0], g_cam_pos[1], g_cam_pos[2]);
    }
    ImGui::Separator();

    changed |= ImGui::Checkbox("Включить дымку и лучи", &g_lighting.enabled);
    ImGui::Spacing();

    ImGui::TextDisabled("Дымка");
    changed |= ImGui::SliderFloat("Плотность дымки", &g_lighting.fog_density, 0.0f, 3.0f, "%.2f");
    changed |= ImGui::SliderFloat("Спад по высоте", &g_lighting.fog_falloff, 0.0005f, 0.08f, "%.4f");
    changed |= ImGui::SliderFloat("Опорная высота дымки", &g_lighting.fog_base_height, -200.0f, 400.0f, "%.0f");
    ImGui::TextWrapped("Ниже опорной высоты дымка гуще. Это не нижняя граница облаков; высота облаков задаётся на вкладке объёма.");
    changed |= ImGui::SliderFloat("Чистая зона", &g_lighting.fog_start, 0.0f, 120.0f, "%.0f");
    changed |= ImGui::SliderFloat("Максимум", &g_lighting.fog_max, 0.0f, 1.0f, "%.2f");
    changed |= ImGui::ColorEdit3("Цвет дымки", g_lighting.fog_color);
    changed |= ImGui::SliderFloat("Дымка по стилю", &g_lighting.fog_style, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("1 — цвет дымки берётся из тумана стиля WoSTweaks, 0 — из цвета выше.");
    changed |= ImGui::SliderFloat("Дымка на небе", &g_lighting.sky_haze, 0.0f, 1.0f, "%.2f");

    ImGui::Spacing();
    ImGui::TextDisabled("Солнце");
    ImGui::TextUnformatted(WosCloudSunSync::valid ? "Sun: synchronized with native directional light"
                           : WosCloudSunSync::held ? "Sun: holding the last native directional light"
                           : "Sun: manual fallback (no native directional light seen yet)");
    changed |= ImGui::SliderFloat3("Направление (резерв)", g_lighting.sun_dir, -1.0f, 1.0f, "%.2f");
    changed |= ImGui::ColorEdit3("Цвет подсвета", g_lighting.inscatter_color);
    changed |= ImGui::SliderFloat("Подсвет", &g_lighting.inscatter, 0.0f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Фокус подсвета", &g_lighting.inscatter_focus, 1.0f, 24.0f, "%.1f");

    ImGui::Spacing();
    ImGui::TextDisabled("Лучи");
    changed |= ImGui::SliderFloat("Сила лучей", &g_lighting.shaft_strength, 0.0f, 3.0f, "%.2f");
    changed |= ImGui::SliderFloat("Длина лучей", &g_lighting.shaft_length, 0.05f, 1.20f, "%.2f");
    changed |= ImGui::SliderFloat("Порог лучей", &g_lighting.shaft_threshold, 0.0f, 1.0f, "%.2f");

    ImGui::Spacing();
    ImGui::TextDisabled("Тонмаппинг и экспозиция");
    changed |= ImGui::Combo("Тонмаппер", &g_lighting.tone_mode, "GTA (исходный)\0AgX\0ACES\0\0");
    if (g_lighting.tone_mode == 0) {
        ImGui::TextWrapped("Исходная цепочка: ванильная экспозиция и Reinhard. Ползунки ниже в этом режиме не действуют.");
    } else {
        changed |= ImGui::SliderFloat("Компенсация экспозиции, EV", &g_lighting.exposure_ev, -3.0f, 3.0f, "%.2f");
        changed |= ImGui::SliderFloat("Предел затемнения, EV", &g_lighting.exposure_min_ev, -10.0f, 0.0f, "%.1f");
        changed |= ImGui::SliderFloat("Предел осветления, EV", &g_lighting.exposure_max_ev, 0.0f, 4.0f, "%.1f");
        ImGui::TextWrapped("Осветление выше нуля открывает тёмные сцены сильнее, чем игра. Ночь станет светлее.");
        if (g_lighting.tone_mode == 1)
            changed |= ImGui::SliderFloat("Контраст AgX (punchy)", &g_lighting.agx_punch, 0.0f, 1.0f, "%.2f");
        changed |= ImGui::SliderFloat("Доля GTA-грейда", &g_lighting.grade_amount, 0.0f, 1.0f, "%.2f");
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Служебное");
    changed |= ImGui::SliderFloat("Вертикальный FOV", &g_lighting.fov, 45.0f, 100.0f, "%.1f");
    ImGui::Combo("Отладка", &g_lighting.debug_view,
                 "Обычный вид Плотность дымки Мировая высота Линейная глубина ");
    if (ImGui::Button("Сбросить освещение")) {
        g_lighting = LightingSettings{};
        changed = true;
    }
}
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/native_pcss.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/native_normals.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/cascade_probe.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/cascade_material.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/native_indirect.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/native_material_context.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/rsm_geometry_capture.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/native_rsm.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/shader_constant_cache.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/deferred_runtime.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/deferred_control.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/primary_gi_provider.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/deferred_gi_control.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/deferred_local_shadow_replay.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/taa_native/taa_runtime.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/dlss_native/dlss_runtime.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Water/ssr/water_ssr.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/chimney_smoke/chimney_smoke.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/street_steam/street_steam.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/soft_particles/soft_particles.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/cloud_lowres/cloud_lowres.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/night_reflection/night_reflection.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/day_cycle/day_cycle.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/streaming_tune.h"
namespace WosCascadeShadowBridge {
static bool Prepare(IDirect3DDevice9*d,unsigned request){
    const double eye[3]={WosCascadeProbe::cameraPosition.x,WosCascadeProbe::cameraPosition.y,WosCascadeProbe::cameraPosition.z};
    return WosCascadeShadow::Prepare(d,request,WosCascadeProbe::plan,WosCascadeProbe::mainScene,eye,WosCascadeProbe::updateMask);
}
static const WosCascadeProbe::Sink kSink={Prepare,WosCascadeShadow::BeginReplay,WosCascadeShadow::SelectTile,WosCascadeShadow::EndReplay};
static const WosCascadeProbe::HeightSink kHeightSink={WosSkyHeight::Prepare,WosSkyHeight::Select};
static void Arm(){
    WosLocalShadow::FrameEnd();
    WosShadowWork::EndFrame(WosCascadeProbe::prepareCpuMs,WosCascadeProbe::replayCpuMs);WosHookProfile::EndFrame();
    WosCascadeShadow::sourceDraws=WosCascadeProbe::replayed;
    WosCascadeShadow::probeStatus=WosCascadeProbe::status;
    WosCascadeShadow::probeReady=WosCascadeProbe::ready?1u:0u;
    WosCascadeShadow::probeFailed=WosCascadeProbe::failed;
    WosCascadeShadow::probeCalls=WosCascadeProbe::prepareCalls;
    WosCascadeShadow::probeEntries=WosCascadeProbe::prepareEntries;
    WosCascadeShadow::skippedShader=WosCascadeProbe::skippedShader;
    WosCascadeShadow::skippedInstance=WosCascadeProbe::skippedInstance;
    WosCascadeShadow::staggerUpdated=WosCascadeProbe::staggerUpdated;
    WosCascadeShadow::staggerForced=WosCascadeProbe::staggerForced;
    WosCloudLight::coverage=g_volumetric.coverage;
    WosCloudLight::opacity=g_volumetric.opacity;
    WosCloudLight::sourceValid=g_cloud_params_seen;
    // Freeze wind once for the next frame; bloom and city read this same phase.
    g_cloud_frame_wind=g_volumetric.wind*ElapsedSeconds()/(std::max)(g_volumetric.size,20.f);
    WosCloudShadow::bottom=g_volumetric.bottom;
    WosCloudShadow::coverage=g_volumetric.coverage;
    WosCloudShadow::size=g_volumetric.size;
    WosCloudShadow::opacity=g_volumetric.opacity;
    WosCloudShadow::thickness=g_volumetric.thickness;
    WosCloudShadow::detail=g_volumetric.detail;
    WosCloudShadow::wind=g_cloud_frame_wind;
    WosCloudShadow::sourceValid=g_cloud_params_seen;
    g_cloud_params_seen=false;
    g_cloud_sun_transmittance=WosCloudLight::Transmittance();
    // The resolve scales the same radiance a second time, so it has to know
    // what the table lost already; without this the two darken the sun twice
    // and a covered sky goes black.
    WosCloudShadow::uniformTransmittance=g_cloud_sun_transmittance;
    if(WosCascadeShadow::Armed()){
        WosCascadeProbe::sinkResolution=WosCascadeShadow::TileRequest();
        WosCascadeProbe::sinkDistance=WosCascadeShadow::distance;
        WosCascadeProbe::sinkStagger=WosCascadeShadow::stagger;
        WosCascadeProbe::saCasters=WosCascadeShadow::saCasters;
        WosCascadeProbe::sink=&kSink;WosCascadeProbe::heightSink=&kHeightSink;
    }
    else if(WosCascadeProbe::sink==&kSink){
        WosCascadeProbe::sink=nullptr;WosCascadeProbe::heightSink=nullptr;WosCascadeProbe::Reset();WosCascadeShadow::ReleaseAtlas();WosSkyHeight::Release();
    }
}
}

namespace WosVolumetricBridge {
static int lastFrame=-1;
// One call per frame, at the native bloom composite: the scene HDR target is
// complete and still linear there, and the classifier has already proven that
// this draw happens exactly once in a frame. A second bloom draw in some other
// scene would otherwise add a second set of shafts, so the frame index gates it.
static void Draw(IDirect3DDevice9*d,bool bloom){
    if(!bloom||WosProbe::suppress||WosDeferred::busy||(!WosVolumetric::Armed()&&!WosDeferred::rainEnabled))return;
    if(lastFrame==g_frame_index)return;
    if(!WosIndirectCamera::frameReady)return;
    // Volumetric and rain bind their shaders through the hooked SetPixelShader but
    // restore through a state block, so g_current_shader would be left on their
    // composite and SSGI, which tests the bloom shader after this, would skip.
    struct ShaderKeep{IDirect3DPixelShader9*s=g_current_shader;~ShaderKeep(){g_current_shader=s;}}keep;
    IDirect3DBaseTexture9*bound=nullptr;
    if(FAILED(d->GetTexture(0,&bound))||!bound)return;
    IDirect3DTexture9*hdr=nullptr;
    const bool isTexture=SUCCEEDED(bound->QueryInterface(IID_IDirect3DTexture9,reinterpret_cast<void**>(&hdr)));
    bound->Release();
    if(!isTexture||!hdr)return;
    D3DSURFACE_DESC desc={};
    IDirect3DTexture9*depth=nullptr;
    WosIndirectCamera::Camera camera;
    if(SUCCEEDED(hdr->GetLevelDesc(0,&desc))&&desc.Format==D3DFMT_A16B16G16R16F&&
       desc.Width>=320&&desc.Height>=200&&desc.Width<=4096&&desc.Height<=4096&&
       WosIndirectCamera::Acquire(desc.Width,desc.Height,camera,depth,WosIndirectCamera::CurrentScene())&&depth){
        bool rendered=WosVolumetric::Render(d,hdr,depth,camera.relativeInverse,camera.worldOrigin,camera.scene);
        if(WosDeferred::rainEnabled)
            rendered=WosRain::Render(d,hdr,depth,camera.relativeInverse,camera.worldOrigin,WosDeferred::Directory(),WosDeferred::rainfallIntensity)||rendered;
        if(rendered)lastFrame=g_frame_index;
    }
    if(depth)depth->Release();
    hdr->Release();
}
}
static void RendererFrameEnd(){WosSmaa::FrameEnd();WosTaa::FrameEnd();WosDlss::FrameEnd();WosNativeClear::BeginIfActive();WosNativeClear::Finish("PRESENT");WosNormals::FrameEnd();WosPCSS::FrameEnd();WosCascadeProbe::FrameEnd();WosCascadeMaterial::FrameEnd();WosRSMGeometry::FrameEnd();WosIndirect::FrameEnd();WosNativeMaterialContext::Finish("PRESENT");WosNativeRSM::FrameEnd();WosDeferred::FrameEnd();WosCascadeShadowBridge::Arm();WosCloudSunSync::Reset();}


// Constructed before the other draw scopes, destroyed after they restore state.
// The recognized native postprocess has already drawn; HUD draws come later.
struct NativeSmaaScope {
    IDirect3DDevice9*d; bool candidate=false,success=false;
    NativeSmaaScope(IDirect3DDevice9*dev):d(dev){
        candidate=((WosSmaa::enabled&&!WosSmaa::done&&!WosSmaa::faulted)||(WosTaa::enabled&&!WosTaa::faulted&&!WosTaa::attemptedThisFrame))
            &&!WosProbe::suppress&&!WosDeferred::busy&&!WosDeferredShadow::recording
            &&ClassifyShader(g_current_shader)==kShaderBloom;
    }
    ~NativeSmaaScope(){
        if(!candidate||!success)return;
        const bool suppress=WosProbe::suppress,busy=WosDeferred::busy;
        auto*shader=g_current_shader;
        WosProbe::suppress=true;WosDeferred::busy=true;
        if(WosSmaa::enabled&&!WosSmaa::done&&!WosSmaa::faulted)WosSmaa::Render(d);
        if(WosTaa::enabled&&!WosTaa::faulted&&!WosTaa::attemptedThisFrame){
            IDirect3DSurface9*rt=nullptr;D3DSURFACE_DESC desc={};
            if(SUCCEEDED(d->GetRenderTarget(0,&rt))&&rt&&SUCCEEDED(rt->GetDesc(&desc))){
                WosIndirectCamera::Camera camera;IDirect3DTexture9*depth=nullptr;
                if(WosIndirectCamera::Acquire(desc.Width,desc.Height,camera,depth,WosIndirectCamera::CurrentScene())){
                    if(WosDlss::enabled&&WosDlss::Resolve(d,depth,camera.relativeInverse,camera.worldOrigin,WosTaa::frame.active,WosTaa::frame.pixel,WosTaa::frame.ndc)){
                        // DLAA took this frame: TAA keeps jittering and restarts its history if DLAA stops.
                        WosTaa::attemptedThisFrame=WosTaa::resolvedThisFrame=true;WosTaa::lastWidth=desc.Width;WosTaa::lastHeight=desc.Height;
                        WosTaa::historyValid=false;WosTaa::status="TAA idle: DLAA resolved this frame";
                    }
                    else WosTaa::Resolve(d,depth,camera.relativeInverse,camera.worldOrigin);
                }
                else{WosTaa::attemptedThisFrame=true;++WosTaa::skipped;WosTaa::historyValid=false;WosTaa::status="TAA waiting for the engine camera and scene depth";}
                if(depth)depth->Release();
            }
            if(rt)rt->Release();
        }
        g_current_shader=shader;
        WosDeferred::busy=busy;WosProbe::suppress=suppress;
        WosDeferredShadow::Invalidate();
    }
};

// Each tab body is drawn inside a scrolling child so the window keeps a fixed
// size no matter how many sections a tab unfolds.
static void DrawAntiAliasTab()
{
    ImGui::SeparatorText("SMAA");
    if (ImGui::Checkbox("SMAA 1x High (native)", &WosSmaa::enabled)) {
        if (WosSmaa::enabled && WosDlss::enabled) { WosDlss::enabled = false; WriteFloatIn("DLSS", "Enabled", 0.0f); } // DLAA wants the raw image
        WriteFloatIn("SMAA", "Enabled", WosSmaa::enabled ? 1.0f : 0.0f);
    }
    ImGui::TextDisabled("%s | frames %lu | failures %lu", WosSmaa::status, WosSmaa::frames, WosSmaa::failures);

    ImGui::SeparatorText("TAA");
    bool taaChanged = ImGui::Checkbox("TAA (temporal anti-aliasing)", &WosTaa::enabled);
    if (WosTaa::enabled) {
        taaChanged |= ImGui::SliderFloat("TAA history weight", &WosTaa::feedbackMax, 0.80f, 0.98f, "%.3f");
        taaChanged |= ImGui::SliderFloat("TAA sharpen", &WosTaa::sharpen, 0.0f, 1.0f, "%.2f");
        taaChanged |= ImGui::SliderFloat("TAA jitter", &WosTaa::jitterScale, 0.0f, 1.0f, "%.2f");
        taaChanged |= ImGui::SliderFloat("TAA anti-ghosting box", &WosTaa::clipGamma, 0.75f, 3.0f, "%.2f");
        ImGui::Combo("TAA view", &WosTaa::debugView, "Normal\0Motion\0History weight\0Raw history\0\0");
    }
    if (ImGui::TreeNode("TAA: подробности")) {
        ImGui::TextWrapped("%s | frames %lu | resets %lu | skipped %lu | failures %lu", WosTaa::status, WosTaa::frames, WosTaa::historyResets, WosTaa::skipped, WosTaa::failures);
        ImGui::TextWrapped("draws jittered %lu, unjittered %lu | shaders %lu patched, %lu rejected", WosTaa::lastJitteredDraws, WosTaa::lastUnjitteredDraws, WosTaa::patchedShaders, WosTaa::rejectedShaders);
        ImGui::TreePop();
    }
    if (taaChanged) {
        if (!WosTaa::enabled && WosDlss::enabled) { WosDlss::enabled = false; WriteFloatIn("DLSS", "Enabled", 0.0f); } // DLAA needs TAA's jitter
        if (WosTaa::feedbackMin > WosTaa::feedbackMax) WosTaa::feedbackMin = WosTaa::feedbackMax;
        WriteFloatIn("TAA", "Enabled", WosTaa::enabled ? 1.0f : 0.0f);
        WriteFloatIn("TAA", "HistoryWeight", WosTaa::feedbackMax);
        WriteFloatIn("TAA", "Sharpen", WosTaa::sharpen);
        WriteFloatIn("TAA", "Jitter", WosTaa::jitterScale);
        WriteFloatIn("TAA", "ClipGamma", WosTaa::clipGamma);
    }

    ImGui::SeparatorText("DLAA (NVIDIA DLSS)");
    if (ImGui::Checkbox("DLAA (DXVK)", &WosDlss::enabled)) {
        // TAA supplies the jitter and stands in whenever DLAA cannot run.
        if (WosDlss::enabled) { WosTaa::enabled = true; WosSmaa::enabled = false; }
        WriteFloatIn("DLSS", "Enabled", WosDlss::enabled ? 1.0f : 0.0f);
        WriteFloatIn("TAA", "Enabled", WosTaa::enabled ? 1.0f : 0.0f);
        WriteFloatIn("SMAA", "Enabled", WosSmaa::enabled ? 1.0f : 0.0f);
    }
    if (WosDlss::enabled) {
        ImGui::Combo("DLAA view", &WosDlss::debugView, "DLAA\0Input (no AA)\0Motion vectors\0\0");
        ImGui::TextWrapped("%s", WosDlss::status);
        if (WosDlss::detail[0]) ImGui::TextDisabled("%s", WosDlss::detail);
    }
    if (ImGui::TreeNode("DLAA: подробности")) {
        ImGui::TextWrapped("preset %d (ini [DLSS] Preset, 11 = K) | frames %lu | resets %lu | skipped %lu | failures %lu | generations %lu",
                           WosDlss::preset, WosDlss::frames, WosDlss::resets, WosDlss::skipped, WosDlss::failures, WosDlss::generations);
        if (WosDlss::b.shared)
            ImGui::TextWrapped("helper: %s | GPU %.2f ms | evaluate failures %u | DXVK flush %.2f ms", WosDlss::b.shared->status, WosDlss::b.shared->gpuMs,
                               WosDlss::b.shared->evaluateFailures, WosDlss::flushMs);
        ImGui::TextDisabled("Only under DXVK (Play - DXVK Test.cmd). Logs: WoS_DLSS\\bin\\dlaa_game.log, host.log");
        ImGui::TreePop();
    }
}

static void DrawWaterTab()
{
    bool waterChanged = ImGui::Checkbox("Water reflections (shaderpack style)", &WosWaterSSR::enabled);
    if (WosWaterSSR::enabled) {
        ImGui::Spacing();
        waterChanged |= ImGui::SliderFloat("Water reflection strength", &WosWaterSSR::strength, 0.0f, 2.0f, "%.2f");
        waterChanged |= ImGui::SliderFloat("Water ripples", &WosWaterSSR::rippleStrength, 0.0f, 1.0f, "%.2f");
        waterChanged |= ImGui::SliderFloat("Water F0 (base reflectance)", &WosWaterSSR::f0, 0.0f, 0.2f, "%.3f");
        waterChanged |= ImGui::SliderFloat("Water sun glint", &WosWaterSSR::glint, 0.0f, 2.0f, "%.2f");
        waterChanged |= ImGui::SliderFloat("Water glint roughness", &WosWaterSSR::roughness, 0.03f, 0.4f, "%.3f");
        waterChanged |= ImGui::SliderFloat("Water ray length", &WosWaterSSR::maxDistance, 50.0f, 800.0f, "%.0f");
        ImGui::Combo("Water view", &WosWaterSSR::debugView, "Normal\0Ripple normal\0Reflection only\0Confidence / hit / sky\0\0");
    }
    ImGui::TextDisabled("%s | frames %lu | mask draws %lu | skipped %lu | failures %lu", WosWaterSSR::status, WosWaterSSR::frames, WosWaterSSR::lastMaskDraws, WosWaterSSR::skipped, WosWaterSSR::failures + WosWaterSSR::replayFailures);
    if (waterChanged) {
        WriteFloatIn("WaterSSR", "Enabled", WosWaterSSR::enabled ? 1.0f : 0.0f);
        WriteFloatIn("WaterSSR", "Strength", WosWaterSSR::strength);
        WriteFloatIn("WaterSSR", "Ripples", WosWaterSSR::rippleStrength);
        WriteFloatIn("WaterSSR", "F0", WosWaterSSR::f0);
        WriteFloatIn("WaterSSR", "Roughness", WosWaterSSR::roughness);
        WriteFloatIn("WaterSSR", "Glint", WosWaterSSR::glint);
        WriteFloatIn("WaterSSR", "MaxDistance", WosWaterSSR::maxDistance);
    }
}

// Smoke from chimneys: settings live in [ChimneySmoke] of clouds_menu.ini.
static void SaveChimneySmoke()
{
    WriteFloatIn("ChimneySmoke", "Enabled", WosChimneySmoke::enabled ? 1.0f : 0.0f);
    WriteFloatIn("ChimneySmoke", "Density", WosChimneySmoke::density);
    WriteFloatIn("ChimneySmoke", "Brightness", WosChimneySmoke::brightness);
    WriteFloatIn("ChimneySmoke", "PlumeHeight", WosChimneySmoke::plumeHeight);
    WriteFloatIn("ChimneySmoke", "StackHeight", WosChimneySmoke::stackHeight);
    WriteFloatIn("ChimneySmoke", "Spread", WosChimneySmoke::spread);
    WriteFloatIn("ChimneySmoke", "WindAngle", WosChimneySmoke::windAngle);
    WriteFloatIn("ChimneySmoke", "WindStrength", WosChimneySmoke::windStrength);
    WriteFloatIn("ChimneySmoke", "RiseSpeed", WosChimneySmoke::riseSpeed);
    WriteFloatIn("ChimneySmoke", "Turbulence", WosChimneySmoke::noiseStrength);
    WriteFloatIn("ChimneySmoke", "Detail", WosChimneySmoke::noiseFrequency);
    WriteFloatIn("ChimneySmoke", "MaxDistance", WosChimneySmoke::maxDistance);
    WriteFloatIn("ChimneySmoke", "StackDistance", WosChimneySmoke::stackDistance);
    WriteFloatIn("ChimneySmoke", "MaxPlumes", float(WosChimneySmoke::maxPlumes));
    WriteFloatIn("ChimneySmoke", "Steps", float(WosChimneySmoke::steps));
    for (int i = 0; i < 3; ++i) {
        char key[32];
        std::snprintf(key, sizeof(key), "HouseTint%d", i); WriteFloatIn("ChimneySmoke", key, WosChimneySmoke::houseTint[i]);
        std::snprintf(key, sizeof(key), "StackTint%d", i); WriteFloatIn("ChimneySmoke", key, WosChimneySmoke::stackTint[i]);
    }
}
static void LoadChimneySmoke()
{
    using namespace WosChimneySmoke;
    auto clamp = [](float v, float lo, float hi) { return std::isfinite(v) ? (std::min)(hi, (std::max)(lo, v)) : lo; };
    enabled = ReadFloatIn("ChimneySmoke", "Enabled", 1.0f) > 0.5f;
    density = clamp(ReadFloatIn("ChimneySmoke", "Density", density), 0.0f, 4.0f);
    brightness = clamp(ReadFloatIn("ChimneySmoke", "Brightness", brightness), 0.0f, 4.0f);
    plumeHeight = clamp(ReadFloatIn("ChimneySmoke", "PlumeHeight", plumeHeight), 3.0f, 60.0f);
    stackHeight = clamp(ReadFloatIn("ChimneySmoke", "StackHeight", stackHeight), 20.0f, 400.0f);
    spread = clamp(ReadFloatIn("ChimneySmoke", "Spread", spread), 0.02f, 0.5f);
    windAngle = clamp(ReadFloatIn("ChimneySmoke", "WindAngle", windAngle), 0.0f, 360.0f);
    windStrength = clamp(ReadFloatIn("ChimneySmoke", "WindStrength", windStrength), 0.0f, 2.0f);
    riseSpeed = clamp(ReadFloatIn("ChimneySmoke", "RiseSpeed", riseSpeed), 0.0f, 8.0f);
    noiseStrength = clamp(ReadFloatIn("ChimneySmoke", "Turbulence", noiseStrength), 0.0f, 2.0f);
    noiseFrequency = clamp(ReadFloatIn("ChimneySmoke", "Detail", noiseFrequency), 0.2f, 4.0f);
    maxDistance = clamp(ReadFloatIn("ChimneySmoke", "MaxDistance", maxDistance), 50.0f, 2000.0f);
    stackDistance = clamp(ReadFloatIn("ChimneySmoke", "StackDistance", stackDistance), 200.0f, 6000.0f);
    maxPlumes = int(clamp(ReadFloatIn("ChimneySmoke", "MaxPlumes", float(maxPlumes)), 1.0f, 64.0f));
    steps = int(clamp(ReadFloatIn("ChimneySmoke", "Steps", float(steps)), 8.0f, 64.0f));
    for (int i = 0; i < 3; ++i) {
        char key[32];
        std::snprintf(key, sizeof(key), "HouseTint%d", i); houseTint[i] = clamp(ReadFloatIn("ChimneySmoke", key, houseTint[i]), 0.0f, 1.0f);
        std::snprintf(key, sizeof(key), "StackTint%d", i); stackTint[i] = clamp(ReadFloatIn("ChimneySmoke", key, stackTint[i]), 0.0f, 1.0f);
    }
}
// Street steam (SM3 port): settings live in [StreetSteam] of clouds_menu.ini.
static void LoadStreetSteam()
{
    using namespace WosStreetSteam;
    auto clamp = [](float v, float lo, float hi) { return std::isfinite(v) ? (std::min)(hi, (std::max)(lo, v)) : lo; };
    enabled = ReadFloatIn("StreetSteam", "Enabled", 1.0f) > 0.5f;
    cameraSmoke = ReadFloatIn("StreetSteam", "CameraSmoke", 1.0f) > 0.5f;
    funnelSmoke = ReadFloatIn("StreetSteam", "FunnelSmoke", 1.0f) > 0.5f;
    density = clamp(ReadFloatIn("StreetSteam", "Density", density), 0.0f, 4.0f);
    brightness = clamp(ReadFloatIn("StreetSteam", "Brightness", brightness), 0.0f, 8.0f);
    ambientShare = clamp(ReadFloatIn("StreetSteam", "Ambient", ambientShare), 0.0f, 4.0f);
    sunShare = clamp(ReadFloatIn("StreetSteam", "Sun", sunShare), 0.0f, 4.0f);
    highAltitude = clamp(ReadFloatIn("StreetSteam", "HighAltitude", highAltitude), 0.0f, 1000.0f);
    funnelScale = clamp(ReadFloatIn("StreetSteam", "FunnelScale", funnelScale), 0.1f, 100.0f);
    windAngle = clamp(ReadFloatIn("StreetSteam", "WindAngle", windAngle), 0.0f, 360.0f);
    windScale = clamp(ReadFloatIn("StreetSteam", "WindScale", windScale), 0.0f, 10.0f);
    softRange = clamp(ReadFloatIn("StreetSteam", "SoftRange", softRange), 0.5f, 50.0f);
    maxDistance = clamp(ReadFloatIn("StreetSteam", "MaxDistance", maxDistance), 50.0f, 2000.0f);
    maxParticles = int(clamp(ReadFloatIn("StreetSteam", "MaxParticles", float(maxParticles)), 0.0f, 6000.0f));
}
// Cloud march resolution: settings live in [CloudLowRes] of clouds_menu.ini.
static void SaveCloudLowRes()
{
    WriteFloatIn("CloudLowRes", "Divisor", float(WosCloudLowRes::divisor));
    WriteFloatIn("CloudLowRes", "MarchedBlend", WosCloudLowRes::marchedBlend);
    WriteFloatIn("CloudLowRes", "SpatialMix", WosCloudLowRes::spatialMix);
    WriteFloatIn("CloudLowRes", "ClampExpand", WosCloudLowRes::clampExpand);
}
static void LoadCloudLowRes()
{
    using namespace WosCloudLowRes;
    auto clamp = [](float v, float lo, float hi) { return std::isfinite(v) ? (std::min)(hi, (std::max)(lo, v)) : lo; };
    const int stored = int(ReadFloatIn("CloudLowRes", "Divisor", float(divisor)) + 0.5f);
    divisor = stored >= 4 ? 4 : stored >= 2 ? 2 : 1;
    marchedBlend = clamp(ReadFloatIn("CloudLowRes", "MarchedBlend", marchedBlend), 0.0f, 0.9f);
    spatialMix = clamp(ReadFloatIn("CloudLowRes", "SpatialMix", spatialMix), 0.0f, 1.0f);
    clampExpand = clamp(ReadFloatIn("CloudLowRes", "ClampExpand", clampExpand), 0.0f, 4.0f);
}
static void DrawCloudLowResSection()
{
    using namespace WosCloudLowRes;
    ImGui::SeparatorText("Разрешение облаков");
    int choice = divisor >= 4 ? 2 : divisor >= 2 ? 1 : 0;
    bool changed = ImGui::Combo("Марш облаков", &choice, "Каждый пиксель (как раньше)\0Половина + временное накопление\0Четверть + временное накопление\0\0");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Половина: за кадр маршится 1/16 пикселей экрана, остальное берётся из прошлых кадров.\n"
                          "Четверть: 1/64. Обрезка облаков зданиями всё равно считается в полном разрешении.");
    if (changed) divisor = choice == 2 ? 4 : choice == 1 ? 2 : 1;
    if (divisor >= 2) {
        changed |= ImGui::SliderFloat("Накопление", &marchedBlend, 0.0f, 0.9f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Сколько истории сохраняет пиксель, промаршированный в этом кадре.\n"
                              "Больше — глаже и меньше зерна, но медленнее реакция на смену освещения.");
        changed |= ImGui::SliderFloat("Сглаживание ожидающих", &spatialMix, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Насколько пиксели, ждущие своей очереди, тянутся к свежей интерполяции. 0 — чисто история.");
        changed |= ImGui::SliderFloat("Допуск истории", &clampExpand, 0.0f, 4.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Насколько история может выходить за диапазон свежих соседей. Меньше — меньше шлейфов, но видны блоки 2x2.");
    }
    ImGui::TextDisabled("%s | буфер %ux%u | марш %ux%u | кадров %lu | сбросов %lu | ошибок %lu",
                        status, bufferWidth, bufferHeight, marchWidth, marchHeight, framesRendered, historyResets, failures);
    if (changed) SaveCloudLowRes();
}

static void DrawSmokeTab()
{
    using namespace WosChimneySmoke;
    bool changed = ImGui::Checkbox("Chimney smoke", &enabled);
    if (enabled) {
        ImGui::SeparatorText("Дым");
        changed |= ImGui::SliderFloat("Плотность", &density, 0.0f, 4.0f, "%.2f");
        changed |= ImGui::SliderFloat("Яркость", &brightness, 0.0f, 4.0f, "%.2f");
        changed |= ImGui::SliderFloat("Высота шлейфа (дома)", &plumeHeight, 3.0f, 60.0f, "%.0f м");
        changed |= ImGui::SliderFloat("Высота шлейфа (заводская труба)", &stackHeight, 20.0f, 400.0f, "%.0f м");
        changed |= ImGui::SliderFloat("Расширение", &spread, 0.02f, 0.5f, "%.2f");
        changed |= ImGui::SliderFloat("Клубы", &noiseStrength, 0.0f, 2.0f, "%.2f");
        changed |= ImGui::SliderFloat("Детали клубов", &noiseFrequency, 0.2f, 4.0f, "%.2f");
        changed |= ImGui::ColorEdit3("Цвет дыма (дома)", houseTint);
        changed |= ImGui::ColorEdit3("Цвет дыма (заводская труба)", stackTint);
        ImGui::SeparatorText("Ветер");
        changed |= ImGui::SliderFloat("Направление", &windAngle, 0.0f, 360.0f, "%.0f°");
        changed |= ImGui::SliderFloat("Снос ветром", &windStrength, 0.0f, 2.0f, "%.2f");
        changed |= ImGui::SliderFloat("Скорость подъёма", &riseSpeed, 0.0f, 8.0f, "%.1f м/с");
        ImGui::SeparatorText("Качество");
        changed |= ImGui::SliderFloat("Дальность (дома)", &maxDistance, 50.0f, 2000.0f, "%.0f м");
        changed |= ImGui::SliderFloat("Дальность (заводская труба)", &stackDistance, 200.0f, 6000.0f, "%.0f м");
        changed |= ImGui::SliderInt("Шлейфов в кадре", &maxPlumes, 1, 64);
        changed |= ImGui::SliderInt("Шагов луча", &steps, 8, 64);
        if (ImGui::Button("Забыть найденные трубы")) Forget();
    }
    if (faulted && ImGui::Button("Повторить загрузку шейдера")) Reset();
    ImGui::TextDisabled("%s", status);
    ImGui::TextDisabled("труб известно %u | шлейфов в кадре %lu | совпадений %lu | VS с world %lu/%lu",
                        Known(Now()), plumesLastFrame, matchedDraws, layoutsUsable, layoutsParsed);
    if (changed) SaveChimneySmoke();
}

// Soft EFX particles: settings live in [SoftParticles] of clouds_menu.ini.
static void SaveSoftParticles()
{
    using namespace WosSoftParticles;
    WriteFloatIn("SoftParticles", "Enabled", enabled ? 1.0f : 0.0f);
    WriteFloatIn("SoftParticles", "FixNative", fixNative ? 1.0f : 0.0f);
    WriteFloatIn("SoftParticles", "SoftenPlain", softenPlain ? 1.0f : 0.0f);
    WriteFloatIn("SoftParticles", "Range", range);
    WriteFloatIn("SoftParticles", "RangePerMetre", rangePerMetre);
    WriteFloatIn("SoftParticles", "Strength", strength);
    WriteFloatIn("SoftParticles", "FeatherScale", featherScale);
}
static void LoadSoftParticles()
{
    using namespace WosSoftParticles;
    auto clamp = [](float v, float lo, float hi) { return std::isfinite(v) ? (std::min)(hi, (std::max)(lo, v)) : lo; };
    enabled = ReadFloatIn("SoftParticles", "Enabled", 1.0f) > 0.5f;
    fixNative = ReadFloatIn("SoftParticles", "FixNative", 1.0f) > 0.5f;
    softenPlain = ReadFloatIn("SoftParticles", "SoftenPlain", 1.0f) > 0.5f;
    range = clamp(ReadFloatIn("SoftParticles", "Range", range), 0.05f, 8.0f);
    rangePerMetre = clamp(ReadFloatIn("SoftParticles", "RangePerMetre", rangePerMetre), 0.0f, 0.1f);
    strength = clamp(ReadFloatIn("SoftParticles", "Strength", strength), 0.0f, 1.0f);
    featherScale = clamp(ReadFloatIn("SoftParticles", "FeatherScale", featherScale), 0.25f, 8.0f);
}
// Night reflections (SM3 cube): settings live in [NightReflection] of clouds_menu.ini.
static void SaveNightReflection()
{
    using namespace WosNightReflection;
    WriteFloatIn("NightReflection", "Mode", float(mode));
    WriteFloatIn("NightReflection", "NightZenith", nightZenith);
    WriteFloatIn("NightReflection", "DayZenith", dayZenith);
}
// Day/night cycle (native clock tick restored): [DayCycle] of clouds_menu.ini.
static void SaveDayCycle()
{
    WriteFloatIn("DayCycle", "Enabled", WosDayCycle::enabled ? 1.0f : 0.0f);
    WriteFloatIn("DayCycle", "Rate", float(WosDayCycle::rate));
    WriteFloatIn("DayCycle", "MoveSun", WosDayCycle::moveSun ? 1.0f : 0.0f);
}
static void LoadDayCycle()
{
    WosDayCycle::enabled = ReadFloatIn("DayCycle", "Enabled", WosDayCycle::enabled ? 1.0f : 0.0f) > 0.5f;
    WosDayCycle::moveSun = ReadFloatIn("DayCycle", "MoveSun", WosDayCycle::moveSun ? 1.0f : 0.0f) > 0.5f;
    WosDayCycle::rate = int((std::min)((std::max)(ReadFloatIn("DayCycle", "Rate", float(WosDayCycle::rate)), 1.0f), 3600.0f) + 0.5f);
}
// Night street lamps: [NightLamps] of clouds_menu.ini.
static void SaveNightLamps()
{
    using namespace WosNightLamps;
    WriteFloatIn("NightLamps", "Mode", float(mode));
    WriteFloatIn("NightLamps", "MaxLights", float(maxLights));
    WriteFloatIn("NightLamps", "Intensity", intensity);
    WriteFloatIn("NightLamps", "RangeTall", rangeTall);
    WriteFloatIn("NightLamps", "RangeShort", rangeShort);
    WriteFloatIn("NightLamps", "MaxDistance", maxDistance);
    WriteFloatIn("NightLamps", "ShaftScale", shaftScale);
    WriteFloatIn("NightLamps", "ColorR", color[0]);
    WriteFloatIn("NightLamps", "ColorG", color[1]);
    WriteFloatIn("NightLamps", "ColorB", color[2]);
}
static void LoadNightLamps()
{
    using namespace WosNightLamps;
    auto rd = [](const char* key, float v, float lo, float hi) { return (std::min)((std::max)(ReadFloatIn("NightLamps", key, v), lo), hi); };
    mode = int(rd("Mode", float(mode), 0.0f, 2.0f) + 0.5f);
    maxLights = int(rd("MaxLights", float(maxLights), 0.0f, 8.0f) + 0.5f);
    intensity = rd("Intensity", intensity, 0.0f, 50.0f);
    rangeTall = rd("RangeTall", rangeTall, 1.0f, 60.0f);
    rangeShort = rd("RangeShort", rangeShort, 1.0f, 60.0f);
    maxDistance = rd("MaxDistance", maxDistance, 10.0f, 500.0f);
    shaftScale = rd("ShaftScale", shaftScale, 0.0f, 2.0f);
    color[0] = rd("ColorR", color[0], 0.0f, 4.0f);
    color[1] = rd("ColorG", color[1], 0.0f, 4.0f);
    color[2] = rd("ColorB", color[2], 0.0f, 4.0f);
}
static void LoadNightReflection()
{
    using namespace WosNightReflection;
    auto clamp = [](float v, float lo, float hi) { return std::isfinite(v) ? (std::min)(hi, (std::max)(lo, v)) : lo; };
    mode = int(clamp(ReadFloatIn("NightReflection", "Mode", float(mode)), 0.0f, 2.0f) + 0.5f);
    nightZenith = clamp(ReadFloatIn("NightReflection", "NightZenith", nightZenith), 0.0f, 10.0f);
    dayZenith = (std::max)(nightZenith, clamp(ReadFloatIn("NightReflection", "DayZenith", dayZenith), 0.0f, 10.0f));
}
static void DrawParticlesTab()
{
    using namespace WosSoftParticles;
    bool changed = ImGui::Checkbox("Soft particles", &enabled);
    if (enabled) {
        ImGui::SeparatorText("Эффекты игры с мягкими краями (zfeather) и декали");
        changed |= ImGui::Checkbox("Дать им настоящую глубину сцены", &fixNative);
        ImGui::TextDisabled("На PC игра подсовывает им мусор вместо глубины: такие частицы пропадают.");
        if (fixNative) changed |= ImGui::SliderFloat("Ширина края (множитель)", &featherScale, 0.25f, 8.0f, "%.2fx");
        ImGui::SeparatorText("Обычные частицы (дым, пыль, искры)");
        changed |= ImGui::Checkbox("Смягчать пересечения с геометрией", &softenPlain);
        if (softenPlain) {
            changed |= ImGui::SliderFloat("Ширина края", &range, 0.05f, 8.0f, "%.2f м");
            changed |= ImGui::SliderFloat("Прибавка с расстоянием", &rangePerMetre, 0.0f, 0.1f, "%.3f м/м");
            changed |= ImGui::SliderFloat("Сила", &strength, 0.0f, 1.0f, "%.2f");
        }
    }
    if (faulted && ImGui::Button("Повторить")) Reset();
    ImGui::TextDisabled("%s", status);
    ImGui::TextDisabled("zfeather %lu/%lu | обычные %lu/%lu | декали %lu/%lu | копий глубины %lu",
                        diag.handled[kFeather], diag.seen[kFeather], diag.handled[kPlain], diag.seen[kPlain],
                        diag.handled[kDecal], diag.seen[kDecal], diag.copies);
    ImGui::TextDisabled("пропущено: не основной вид %lu, нет камеры %lu, непрозрачные %lu, другой VS %lu",
                        diag.notMain, diag.noCamera, diag.opaque, diag.otherVS);
    if (changed) SaveSoftParticles();
}

// The deferred tabs all live behind the one master switch on the Свет tab.
// Say so instead of showing a dead panel.
static bool DeferredGate()
{
    if (WosDeferred::enabled) return true;
    ImGui::TextColored(ImVec4(1.0f, 0.70f, 0.20f, 1.0f), "Отложенное освещение выключено");
    ImGui::TextWrapped("Включите «Deferred direct lighting» на вкладке «Свет» — без него эти настройки не действуют.");
    return false;
}

static void DrawDeferredLightTab(bool &deferredChanged)
{
    deferredChanged |= ImGui::Checkbox("Deferred direct lighting", &WosDeferred::enabled);
    ImGui::TextDisabled("%s", WosDeferred::status);
    if (!WosDeferred::enabled) return;

    ImGui::Spacing();
    deferredChanged |= ImGui::SliderFloat("Direct light intensity", &WosDeferred::intensity, 0.f, 3.f);
    deferredChanged |= ImGui::SliderFloat("Direct specular intensity", &WosDeferred::specularIntensity, 0.f, 2.f);
    deferredChanged |= ImGui::Combo("Deferred view", &WosDeferred::debugView, "Normal\0Direct light\0Surface coverage\0Normals\0Contact shadow\0Cascade shadow\0Cascade index\0Cloud shadow\0Ambient visibility\0");
    if (WosDeferred::debugView != 0)
        ImGui::TextColored(ImVec4(1.f, .7f, .2f, 1.f), "DIAGNOSTIC VIEW - choose Normal for gameplay");

    ImGui::SeparatorText("Ambient occlusion");
    ImGui::TextWrapped("Multiplies the engine environment term only. Sun and point lights are separate quantities in the resolve and are never touched.");
    deferredChanged |= ImGui::SliderFloat("Ambient occlusion", &WosDeferred::ambientStrength, 0.f, 1.f);
    if (WosDeferred::ambientStrength > 0.f) {
        deferredChanged |= ImGui::SliderFloat("Occlusion radius", &WosDeferred::ambientRadius, .5f, 32.f);
        deferredChanged |= ImGui::SliderFloat("Occlusion distance", &WosDeferred::ambientDistance, 20.f, 600.f);
        deferredChanged |= ImGui::SliderFloat("Occlusion bias", &WosDeferred::ambientBias, 0.f, .5f);
        deferredChanged |= ImGui::Checkbox("Sky height map (off-screen occluders)", &WosSkyHeight::enabled);
        if (WosSkyHeight::enabled) {
            deferredChanged |= ImGui::SliderFloat("Sky ray length", &WosSkyHeight::rayLength, 4.f, 120.f);
            deferredChanged |= ImGui::SliderFloat("Height map extent", &WosSkyHeight::extent, 64.f, 1024.f);
            deferredChanged |= ImGui::SliderInt("Height map interval", &WosSkyHeight::interval, 1, 30);
            ImGui::TextDisabled("%s (renders %u, binds %u)", WosSkyHeight::status, WosSkyHeight::renders, WosSkyHeight::binds);
        }
    }

    ImGui::SeparatorText("Soft lamp light");
    deferredChanged |= ImGui::SliderFloat("Lamp falloff smoothness", &WosDeferred::lampSmooth, 0.f, 1.f);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = the game's linear falloff, 1 = smoothstep: soft edges, no kink at the range.");
    deferredChanged |= ImGui::SliderFloat("Lamp highlight reduction", &WosDeferred::lampSpecularCut, 0.f, 1.f);
    deferredChanged |= ImGui::SliderFloat("Lamp brightness reduction", &WosDeferred::lampDim, 0.f, .9f);
    deferredChanged |= ImGui::SliderFloat("Lamp radius scale", &WosDeferred::lampRadiusScale, 1.f, 2.f);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Wider pools. Above 1 a pool can end at a mesh edge: the game picks each draw's lights by their native range.");

    ImGui::SeparatorText("Specular antialiasing");
    deferredChanged |= ImGui::SliderFloat("Specular AA strength", &WosDeferred::specularAA, 0.f, 4.f);
    if (WosDeferred::specularAA > 0) {
        deferredChanged |= ImGui::SliderFloat("Specular AA clamp", &WosDeferred::specularAAClamp, 0.f, 1.f);
        deferredChanged |= ImGui::SliderFloat("Specular AA edge", &WosDeferred::specularAAEdge, .005f, 1.f);
    }
    ImGui::TextDisabled("0 = off. Most visible with a low RoughnessFloor.");

    ImGui::SeparatorText("Material classes");
    deferredChanged |= ImGui::SliderFloat("Material class strength", &WosMaterialClasses::strength, 0.f, 1.f);
    if (WosMaterialClasses::strength > 0) {
        if (!WosMaterialClasses::loaded)
            ImGui::TextDisabled("texture_classes.bin not loaded: every surface uses the mask estimate.");
        else
            ImGui::TextDisabled("%u of %u primary draws classified, %u facades (windows as glass)",
                                WosMaterialClasses::lastClassified, WosMaterialClasses::lastSeen,
                                WosMaterialClasses::lastFacadeDraws);
        deferredChanged |= ImGui::SliderFloat("Rain smooths classes", &WosMaterialClasses::rainPull, 0.f, 1.f);
        if (ImGui::TreeNode("Class table (floor, ceiling, metal, reflectance)")) {
            for (unsigned c = 1; c < WosMaterialClasses::ClassCount; ++c) {
                if (!WosMaterialClasses::IsRow(c)) continue;
                ImGui::PushID(int(c));
                char label[64];
                std::snprintf(label, sizeof(label), "%s (%u)", WosMaterialClasses::Names[c], WosMaterialClasses::lastClassDraws[c]);
                deferredChanged |= ImGui::SliderFloat4(label, WosMaterialClasses::table[c], 0.f, 1.f, "%.2f");
                ImGui::PopID();
            }
            if (ImGui::Button("Reset class table")) {
                std::memcpy(WosMaterialClasses::table, WosMaterialClasses::defaults, sizeof(WosMaterialClasses::table));
                deferredChanged = true;
            }
            ImGui::TreePop();
        }
    }
    ImGui::TextDisabled("0 = off. Facade walls stay on the estimate; their windows use glass.");
    ImGui::TextDisabled("symbiote: goo reflects in its own violet specular colour.");

    ImGui::SeparatorText("PBR sky lighting");
    deferredChanged |= ImGui::Checkbox("Sky lighting", &WosSkyIBL::enabled);
    if (WosSkyIBL::enabled) {
        deferredChanged |= ImGui::SliderFloat("Sky diffuse", &WosSkyIBL::diffuseStrength, 0.f, 2.f);
        deferredChanged |= ImGui::SliderFloat("Sky reflections", &WosSkyIBL::specularStrength, 0.f, 3.f);
        ImGui::TextDisabled("Sky captures: %u, draws: %u", WosSkyIBL::captures, WosSkyIBL::binds);
        if (ImGui::TreeNode("Материалы (roughness и металл)")) {
            deferredChanged |= ImGui::SliderFloat("RoughnessFloor", &WosSkyIBL::material[0], float(0.05f), float(1));
            deferredChanged |= ImGui::SliderFloat("RoughnessCeiling", &WosSkyIBL::material[1], float(.05f), float(1));
            deferredChanged |= ImGui::SliderFloat("RoughnessVariance", &WosSkyIBL::material[2], float(0), float(1));
            deferredChanged |= ImGui::SliderFloat("DielectricF0", &WosSkyIBL::material[3], float(.001f), float(.16f));
            deferredChanged |= ImGui::SliderFloat("MetalStrength", &WosSkyIBL::material[4], float(0), float(1));
            deferredChanged |= ImGui::SliderFloat("MetalThreshold", &WosSkyIBL::material[5], float(0), float(1));
            deferredChanged |= ImGui::SliderFloat("MetalWidth", &WosSkyIBL::material[6], float(.01f), float(1));
            deferredChanged |= ImGui::SliderFloat("MetalTint", &WosSkyIBL::material[7], float(0), float(1));
            ImGui::TreePop();
        }
    }
}

static void DrawShadowTab(bool &deferredChanged)
{
    if (!DeferredGate()) return;

    ImGui::SeparatorText("Каскадные тени солнца");
    deferredChanged |= ImGui::Checkbox("Four dynamic cascades (PCSS)", &WosCascadeShadow::enabled);
    ImGui::TextDisabled("%s", WosCascadeShadow::status);
    if (WosCascadeShadow::enabled) {
        deferredChanged |= ImGui::Combo("Cascade mode", &WosCascadeShadow::mode, "Off\0Combine with native\0Replace native\0");
        int cascadeResolution = WosCascadeShadow::resolution >= 2048 ? 2 : (WosCascadeShadow::resolution >= 1024 ? 1 : 0);
        if (ImGui::Combo("Cascade resolution", &cascadeResolution, "512\0" "1024\0" "2048\0")) {
            WosCascadeShadow::resolution = cascadeResolution == 2 ? 2048 : (cascadeResolution == 1 ? 1024 : 512);
            deferredChanged = true;
        }
        deferredChanged |= ImGui::SliderFloat("Cascade distance", &WosCascadeShadow::distance, 20.f, 400.f);
        deferredChanged |= ImGui::Combo("Cascade stagger", &WosCascadeShadow::stagger, "Off (all every frame)\0Far pair alternates\0Aggressive (1 of 2, far 1 of 4)\0");
        ImGui::TextDisabled("Cascades refreshed last frame: %u of 4, forced early: %u", WosCascadeShadow::staggerUpdated, WosCascadeShadow::staggerForced);
#ifndef WOS_RELEASE_UI
        deferredChanged |= ImGui::Checkbox("SA district shadow casters", &WosCascadeShadow::saCasters);
        ImGui::TextDisabled("SA casters: %u available, %u draws, %u culled", WosShadowWork::cpuLast.saAvailable, WosShadowWork::cpuLast.saDraws, WosShadowWork::cpuLast.saCulled);
#endif
        deferredChanged |= ImGui::SliderFloat("Sun angular size", &WosCascadeShadow::sunAngle, 0.f, .2f);
        deferredChanged |= ImGui::SliderFloat("Blocker search distance", &WosCascadeShadow::searchDistance, 1.f, 120.f);
        deferredChanged |= ImGui::SliderFloat("Cascade depth bias", &WosCascadeShadow::depthBias, 0.f, 2.f);
        deferredChanged |= ImGui::SliderFloat("Cascade normal bias", &WosCascadeShadow::normalBias, 0.f, 8.f);
        deferredChanged |= ImGui::SliderFloat("Cascade strength", &WosCascadeShadow::strength, 0.f, 1.f);
    }

    ImGui::SeparatorText("Тени локальных источников");
    deferredChanged |= ImGui::Checkbox("Local light shadow maps", &WosLocalShadow::enabled);
    ImGui::TextDisabled("%s", WosLocalShadow::status);
    if (WosLocalShadow::enabled) {
        deferredChanged |= ImGui::SliderFloat("Local emitter radius", &WosLocalShadow::emitterRadius, 0.f, 1.f);
        deferredChanged |= ImGui::SliderFloat("Local shadow bias", &WosLocalShadow::bias, .001f, .2f);
        ImGui::TextDisabled("Local maps: %u lights, %u draws, %u skipped, %u failures", WosLocalShadow::count, WosLocalShadow::draws, WosLocalShadow::skipped, WosLocalShadow::failures);
    }

    ImGui::SeparatorText("Контактные тени");
    deferredChanged |= ImGui::Checkbox("Contact shadows (screen space)", &WosDeferred::contactEnabled);
    if (WosDeferred::contactEnabled) {
        deferredChanged |= ImGui::SliderFloat("Contact length", &WosDeferred::contactLength, .05f, 10.f);
        deferredChanged |= ImGui::SliderFloat("Contact thickness", &WosDeferred::contactThickness, .01f, 10.f);
        deferredChanged |= ImGui::SliderInt("Contact steps", &WosDeferred::contactSteps, 2, 32);
        deferredChanged |= ImGui::SliderFloat("Contact strength", &WosDeferred::contactStrength, 0.f, 1.f);
        deferredChanged |= ImGui::SliderFloat("Contact bias", &WosDeferred::contactBias, 0.f, 1.f);
        deferredChanged |= ImGui::SliderFloat("Contact max pixels", &WosDeferred::contactPixels, 4.f, 256.f);
        deferredChanged |= ImGui::SliderFloat("Contact softness", &WosDeferred::contactSoftness, 0.f, 1.f);
        deferredChanged |= ImGui::SliderFloat("Contact thickness growth", &WosDeferred::contactThicknessGrowth, 0.f, .1f);
        deferredChanged |= ImGui::Checkbox("Contact temporal noise (needs TAA)", &WosDeferred::contactTemporal);
    }

    ImGui::SeparatorText("Облака и солнце");
    deferredChanged |= ImGui::Checkbox("Cloud cover blocks the sun", &WosCloudLight::enabled);
    ImGui::TextDisabled("%s", WosCloudLight::Status());
    if (WosCloudLight::enabled) {
        deferredChanged |= ImGui::SliderFloat("Cloud sun blocking", &WosCloudLight::blocking, 0.f, 1.f);
        ImGui::TextDisabled("Sun transmittance now: %.2f", WosCloudLight::Transmittance());
    }
    deferredChanged |= ImGui::Checkbox("Cloud shadows on the city", &WosCloudShadow::enabled);
    ImGui::TextDisabled("%s", WosCloudShadow::Status());
    if (WosCloudShadow::enabled) {
        deferredChanged |= ImGui::SliderFloat("Cloud shadow strength", &WosCloudShadow::strength, 0.f, 1.f);
        deferredChanged |= ImGui::SliderInt("Cloud shadow samples", &WosCloudShadow::steps, 1, 16);
        deferredChanged |= ImGui::SliderFloat("Least sun under a solid deck", &WosCloudShadow::minimum, 0.f, 1.f);
        deferredChanged |= ImGui::Checkbox("Compensate the uniform blocking", &WosCloudShadow::compensate);
        ImGui::TextWrapped("With compensation on the march alone decides the direct sun; the uniform blocking above still scales the shafts and the sky inscatter.");
    }
}

static void DrawVolumetricTab(bool &deferredChanged)
{
    if (!DeferredGate()) return;

    deferredChanged |= ImGui::Checkbox("Volumetric sun shafts", &WosVolumetric::enabled);
    ImGui::TextDisabled("%s", WosVolumetric::status);

    ImGui::SeparatorText("Свет фонарей в воздухе");
    deferredChanged |= ImGui::Checkbox("Lamp light shafts", &WosVolumetric::localEnabled);
    if (WosVolumetric::localEnabled) {
        ImGui::TextDisabled("Lights: %u seen, %u drawn, %u through cube shadows", WosVolumetric::lastShaftObserved, WosVolumetric::lastShaftCount, WosVolumetric::lastShaftShadowed);
        deferredChanged |= ImGui::SliderFloat("Lamp density", &WosVolumetric::localDensity, 0.f, .5f, "%.3f");
        deferredChanged |= ImGui::SliderFloat("Lamp strength", &WosVolumetric::localStrength, 0.f, 8.f);
        deferredChanged |= ImGui::SliderFloat("Lamp core radius", &WosVolumetric::localCore, .1f, 10.f);
        deferredChanged |= ImGui::SliderFloat("Lamp radius scale", &WosVolumetric::localRadiusScale, .25f, 3.f);
        deferredChanged |= ImGui::SliderInt("Lamp max lights", &WosVolumetric::localMaxLights, 1, 24);
        deferredChanged |= ImGui::SliderInt("Lamp shadow samples", &WosVolumetric::localShadowSteps, 0, 48);
        deferredChanged |= ImGui::SliderFloat("Lamp max distance", &WosVolumetric::localMaxDistance, 20.f, 1000.f);
        if (!WosLocalShadow::enabled)
            ImGui::TextWrapped("Beams cut by geometry need \"Local light shadow maps\" on; without them every lamp is a soft glow.");
        ImGui::TextWrapped("\"Shaft view: Shafts only\" below shows the lamps alone too.");
    }
    if (!WosVolumetric::enabled && !WosVolumetric::localEnabled) return;

    ImGui::SeparatorText("Лучи");
    deferredChanged |= ImGui::SliderInt("Shaft samples", &WosVolumetric::steps, 4, 64);
    deferredChanged |= ImGui::SliderFloat("Shaft distance", &WosVolumetric::distance, 20.f, 600.f);
    deferredChanged |= ImGui::SliderFloat("Shaft density", &WosVolumetric::density, 0.f, .2f);
    deferredChanged |= ImGui::SliderFloat("Shaft strength", &WosVolumetric::strength, 0.f, 4.f);
    deferredChanged |= ImGui::SliderFloat("Shaft forward scattering", &WosVolumetric::anisotropy, 0.f, .95f);
    deferredChanged |= ImGui::SliderFloat("Shaft edge rejection", &WosVolumetric::rejection, 0.f, 4.f);
    deferredChanged |= ImGui::Combo("Shaft view", &WosVolumetric::debugView, "Normal\0Shafts only\0");

    ImGui::SeparatorText("Атмосферный туман");
    deferredChanged |= ImGui::Checkbox("Atmospheric fog", &WosVolumetric::fogEnabled);
    if (WosVolumetric::fogEnabled) {
        deferredChanged |= ImGui::SliderFloat("Fog density", &WosVolumetric::fogDensity, 0.f, .01f, "%.5f");
        deferredChanged |= ImGui::SliderFloat("Fog start", &WosVolumetric::fogStart, 0.f, 500.f);
        deferredChanged |= ImGui::SliderFloat("Fog distance", &WosVolumetric::fogDistance, 20.f, 2000.f);
        deferredChanged |= ImGui::ColorEdit3("Fog ambient colour", WosVolumetric::fogColour);
        deferredChanged |= ImGui::Checkbox("Height fog", &WosVolumetric::fogHeightEnabled);
        if (WosVolumetric::fogHeightEnabled) {
            deferredChanged |= ImGui::DragFloat("Fog base height", &WosVolumetric::fogBaseHeight, 1.f, -10000.f, 10000.f, "%.1f");
            deferredChanged |= ImGui::SliderFloat("Fog height falloff", &WosVolumetric::fogHeightFalloff, 0.f, .2f, "%.4f");
            ImGui::TextWrapped("World height: full density below base, thinning above. Zero falloff gives uniform fog.");
        }
    }
}

static void DrawRainTab(bool &deferredChanged)
{
    if (!DeferredGate()) return;

    deferredChanged |= ImGui::Checkbox("Wet roads and suit (experimental)", &WosDeferred::rainEnabled);
    if (!WosDeferred::rainEnabled) return;
    ImGui::Spacing();
    deferredChanged |= ImGui::SliderFloat("Rainfall", &WosDeferred::rainfallIntensity, 0, 1);
    deferredChanged |= ImGui::SliderFloat("Wetness", &WosDeferred::rainWetness, 0, 1);
    deferredChanged |= ImGui::SliderFloat("Puddle coverage", &WosDeferred::rainPuddles, 0, 1);
    deferredChanged |= ImGui::SliderFloat("Puddle size", &WosDeferred::rainSize, .2f, 30);
    deferredChanged |= ImGui::SliderFloat("Rain ripples", &WosDeferred::rainRipples, 0, 1);
    deferredChanged |= ImGui::SliderFloat("Puddle reflections", &WosDeferred::rainReflections, 0, 1);
}

static void DrawIndirectTab()
{
    ImGui::TextDisabled("%s", WosIndirect::status);
    if (!WosIndirect::enabled) {
        ImGui::TextWrapped("SSGI сейчас неактивен — ползунки появятся, когда проход включится.");
        return;
    }
    ImGui::Spacing();
    if (!WosIndirect::V2::loaded) WosIndirect::V2::Load();
    {
        auto &v = WosIndirect::V2::s;
        bool v2 = v.version == 2;
        bool changed = ImGui::Checkbox("SSGI v2 (visibility mask, Proper Shaders scheme)", &v2);
        v.version = v2 ? 2 : 1;
        if (v2) {
            int res = v.divisor >= 4 ? 1 : 0;
            if (ImGui::Combo("SSGI v2 trace resolution", &res, "Half\0Quarter\0\0")) { v.divisor = res ? 4 : 2; changed = true; }
            changed |= ImGui::SliderFloat("SSGI v2 intensity", &v.intensity, 0.f, 8.f);
            changed |= ImGui::SliderFloat("SSGI v2 radius", &v.radius, .5f, 20.f);
            changed |= ImGui::SliderInt("SSGI v2 slices", &v.slices, 1, 8);
            changed |= ImGui::SliderInt("SSGI v2 steps", &v.steps, 4, 16);
            changed |= ImGui::SliderFloat("SSGI v2 thickness", &v.thickness, 0.f, 3.f);
            changed |= ImGui::SliderFloat("SSGI v2 saturation", &v.saturation, 0.f, 2.f);
            changed |= ImGui::SliderFloat("SSGI v2 max indirect", &v.maxIndirect, .1f, 20.f);
            changed |= ImGui::SliderFloat("SSGI v2 multi-bounce", &v.multiBounce, 0.f, 1.f);
            changed |= ImGui::SliderFloat("SSGI v2 emissive boost", &v.emissiveStrength, 0.f, 4.f);
            changed |= ImGui::SliderFloat("SSGI v2 history weight", &v.historyWeight, 0.f, .98f);
            changed |= ImGui::SliderFloat("SSGI v2 anti-ghosting clamp", &v.clamp, 0.f, 4.f);
            ImGui::SliderFloat("SSGI max distance", &WosIndirect::maxDistance, 40.f, 400.f);
            ImGui::Combo("SSGI v2 view", &v.debugView, "Normal\0Added light x8\0Visibility (AO)\0Gathered light x8\0\0");
            if (changed) WosIndirect::V2::Save();
            ImGui::TextDisabled("Trace %ux%u, %u mip levels%s | history %u reprojected / %u restarted",
                                WosIndirect::V2::tw, WosIndirect::V2::th, WosIndirect::V2::levels,
                                WosIndirect::V2::mipsOk ? "" : " (mips off)", WosIndirect::historyFrames, WosIndirect::historyResets);
            return;
        }
        if (changed) WosIndirect::V2::Save();
    }
    ImGui::SliderFloat("SSGI bounce strength", &WosIndirect::bounce, 0.f, 1.5f);
    ImGui::SliderFloat("SSGI ray length", &WosIndirect::radius, 2.f, 20.f);
    ImGui::SliderFloat("SSGI max distance", &WosIndirect::maxDistance, 40.f, 400.f);
    bool ssgiChanged = ImGui::SliderFloat("SSGI history frames", &WosIndirect::ssgiHistory, 4.f, 48.f, "%.0f");
    ssgiChanged |= ImGui::SliderFloat("SSGI anti-ghosting", &WosIndirect::ssgiAntiGhost, 0.f, 1.f);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Higher: stale bounce light fades faster, slightly more noise. 0 = off.");
    ssgiChanged |= ImGui::SliderFloat("SSGI multi-bounce", &WosIndirect::ssgiMultiBounce, 0.f, 1.f);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Light already bounced once is bounced again next frame (PTGI-like). 0 = single bounce.");
    if (ssgiChanged) WosIndirect::SaveSSGISettings();
    ImGui::Combo("SSGI view", &WosIndirect::debugView, "Normal\0Added light x8\0History length\0");
    if (WosIndirect::debugView != 0)
        ImGui::TextColored(ImVec4(1.f, .7f, .2f, 1.f), "DIAGNOSTIC VIEW - choose Normal for gameplay");
    ImGui::TextDisabled("SSGI history: %u reprojected / %u restarted", WosIndirect::historyFrames, WosIndirect::historyResets);
}

static void DrawDiagnosticsTab(bool &deferredChanged)
{
    ImGui::SeparatorText("Проходы движка");
    ImGui::TextWrapped("%s", WosProbe::status);
    ImGui::TextWrapped("%s", WosPCSS::status);
    ImGui::TextWrapped("PCSS draws last frame: %u", WosPCSS::lastHits);
    ImGui::TextWrapped("%s", WosNormals::status);
    ImGui::TextWrapped("%s", WosCascadeProbe::status);
    ImGui::TextWrapped("%s | draws: %u", WosCascadeMaterial::status, WosCascadeMaterial::lastHits);

    ImGui::SeparatorText("Профилирование");
    deferredChanged |= ImGui::Checkbox("GPU profiler (timestamps)", &WosDeferredGpu::enabled);
    ImGui::TextWrapped("%s", WosDeferredGpu::status);
    if (ImGui::Checkbox("Eligibility state cache", &WosDeferredShadow::requested)) {
        WosDeferredShadow::SetEnabled(WosDeferredShadow::requested);
        deferredChanged = true;
    }
    ImGui::TextWrapped("%s", WosDeferredShadow::status);

    ImGui::SeparatorText("Streaming poll");
    if (WosStreaming::active) {
        int ms = WosStreaming::pollMs;
        if (ImGui::SliderInt("Пауза опроса стримера, мс", &ms, 1, 5)) WosStreaming::Apply(ms);
        ImGui::TextDisabled("1 — быстрее подгрузка районов и текстур, 5 — как в оригинале");
    }
    ImGui::TextWrapped("%s", WosStreaming::status);

    ImGui::SeparatorText("Трек камеры");
    if (g_recording)
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f), "F4: идёт запись, кадров: %d", static_cast<int>(g_track.size()));
    else
        ImGui::TextDisabled("F4: записать трек камеры");
}

// --- simple page ---------------------------------------------------------------
// One page for players: a quality level, a switch per effect, the weather and
// the tone mapper. Everything it touches is the same variable the advanced tabs
// edit, saved to the same ini keys, so the two views never disagree.
#ifdef WOS_RELEASE_UI
#define WOS_UI(ru, en) en
#else
#define WOS_UI(ru, en) ru
#endif

static bool g_menu_advanced = false;

struct QualityLevel {
    const char *name;
    bool cascades; int cascadeResolution; float cascadeDistance; int cascadeStagger;
    bool localShadows;
    bool contact; int contactSteps;
    float ambient; bool skyHeight;
    bool shafts; int shaftSteps;
    int cloudShadowSteps;
    int cloudDivisor;
    float waterDistance;
};
// High is the configuration the renderer was tuned and profiled with. The game
// is CPU-bound: the steps between levels mostly remove shadow-pass draw replays
// (cascades, cube maps, the sky height map), not pixel work.
static const QualityLevel kQualityLevels[] = {
    { "Low",    false,  512, 150.f, 2, false, false,  8, 0.f, false, false, 12, 1, 4, 300.f },
    { "Medium", true,  1024, 200.f, 2, true,  true,   8, 1.f, false, true,  12, 2, 2, 500.f },
    { "High",   true,  1024, 400.f, 1, true,  true,  16, 1.f, true,  true,  23, 3, 2, 800.f },
    { "Ultra",  true,  2048, 400.f, 0, true,  true,  24, 1.f, true,  true,  32, 6, 2, 800.f },
};
static const int kQualityCount = static_cast<int>(sizeof(kQualityLevels) / sizeof(kQualityLevels[0]));
static float g_ambient_restore = 1.0f;

static void ApplyQuality(const QualityLevel &q)
{
    WosDeferred::enabled = true;
    WosCascadeShadow::enabled = q.cascades;
    if (q.cascades && WosCascadeShadow::mode == 0) WosCascadeShadow::mode = 2;
    WosCascadeShadow::resolution = q.cascadeResolution;
    WosCascadeShadow::distance = q.cascadeDistance;
    WosCascadeShadow::stagger = q.cascadeStagger;
    WosLocalShadow::enabled = q.localShadows;
    WosDeferred::contactEnabled = q.contact;
    WosDeferred::contactSteps = q.contactSteps;
    if (q.ambient > 0.f && WosDeferred::ambientStrength <= 0.f) WosDeferred::ambientStrength = g_ambient_restore;
    if (q.ambient <= 0.f && WosDeferred::ambientStrength > 0.f) { g_ambient_restore = WosDeferred::ambientStrength; WosDeferred::ambientStrength = 0.f; }
    WosSkyHeight::enabled = q.skyHeight;
    WosVolumetric::enabled = q.shafts;
    WosVolumetric::steps = q.shaftSteps;
    WosCloudShadow::steps = q.cloudShadowSteps;
    WosCloudLowRes::divisor = q.cloudDivisor;
    WosWaterSSR::maxDistance = q.waterDistance;
    WosDeferred::SaveSettings();
    WosDeferred::WriteStatus(true);
    SaveCloudLowRes();
    WriteFloatIn("WaterSSR", "MaxDistance", WosWaterSSR::maxDistance);
}

// The level the current settings equal, or -1 once anything was tuned by hand.
static int CurrentQuality()
{
    for (int i = 0; i < kQualityCount; ++i) {
        const QualityLevel &q = kQualityLevels[i];
        if (!WosDeferred::enabled || WosCascadeShadow::enabled != q.cascades || WosLocalShadow::enabled != q.localShadows ||
            WosDeferred::contactEnabled != q.contact || (WosDeferred::ambientStrength > 0.f) != (q.ambient > 0.f) ||
            WosSkyHeight::enabled != q.skyHeight || WosVolumetric::enabled != q.shafts ||
            WosCloudShadow::steps != q.cloudShadowSteps || WosCloudLowRes::divisor != q.cloudDivisor ||
            std::fabs(WosWaterSSR::maxDistance - q.waterDistance) > 0.5f)
            continue;
        if (q.cascades && (WosCascadeShadow::resolution != q.cascadeResolution ||
                           std::fabs(WosCascadeShadow::distance - q.cascadeDistance) > 0.5f ||
                           WosCascadeShadow::stagger != q.cascadeStagger))
            continue;
        if (q.contact && WosDeferred::contactSteps != q.contactSteps) continue;
        if (q.shafts && WosVolumetric::steps != q.shaftSteps) continue;
        return i;
    }
    return -1;
}

static void Experimental()
{
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.70f, 0.20f, 1.0f), "%s", WOS_UI("(эксперимент)", "(experimental)"));
}

static void DrawSimplePage(bool &changed, bool &deferredChanged)
{
    ImGui::SeparatorText(WOS_UI("Качество", "Quality"));
    const int current = CurrentQuality();
    for (int i = 0; i < kQualityCount; ++i) {
        if (i) ImGui::SameLine();
        const bool active = i == current;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(kQualityLevels[i].name, ImVec2(90.0f, 0.0f))) ApplyQuality(kQualityLevels[i]);
        if (active) ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", current < 0 ? WOS_UI("свои настройки", "custom") : "");
    ImGui::TextDisabled("%s", WOS_UI("Игра упирается в процессор: ниже уровень - меньше проходов теней.",
                                     "The game is CPU-bound: lower levels mostly skip shadow passes."));

    ImGui::SeparatorText(WOS_UI("Эффекты", "Effects"));
    deferredChanged |= ImGui::Checkbox(WOS_UI("Новое освещение (deferred)", "Enhanced lighting"), &WosDeferred::enabled);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", WOS_UI("Главный выключатель: тени, AO, материалы, лучи и туман работают только с ним.",
                                       "Master switch: shadows, AO, materials, sun shafts and fog need it."));
    ImGui::BeginDisabled(!WosDeferred::enabled);
    if (ImGui::BeginTable("##effects", 2)) {
        ImGui::TableNextColumn();
        deferredChanged |= ImGui::Checkbox(WOS_UI("Тени солнца", "Sun shadows"), &WosCascadeShadow::enabled);
        if (WosCascadeShadow::enabled && WosCascadeShadow::mode == 0) WosCascadeShadow::mode = 2;
        ImGui::TableNextColumn();
        deferredChanged |= ImGui::Checkbox(WOS_UI("Тени фонарей", "Lamp shadows"), &WosLocalShadow::enabled);
        ImGui::TableNextColumn();
        deferredChanged |= ImGui::Checkbox(WOS_UI("Контактные тени", "Contact shadows"), &WosDeferred::contactEnabled);
        ImGui::TableNextColumn();
        bool ambient = WosDeferred::ambientStrength > 0.f;
        if (ImGui::Checkbox(WOS_UI("Ambient occlusion", "Ambient occlusion"), &ambient)) {
            if (ambient) WosDeferred::ambientStrength = g_ambient_restore > 0.f ? g_ambient_restore : 1.f;
            else { g_ambient_restore = WosDeferred::ambientStrength; WosDeferred::ambientStrength = 0.f; }
            deferredChanged = true;
        }
        ImGui::TableNextColumn();
        deferredChanged |= ImGui::Checkbox(WOS_UI("Свет и отражения неба", "Sky light and reflections"), &WosSkyIBL::enabled);
        ImGui::TableNextColumn();
        deferredChanged |= ImGui::Checkbox(WOS_UI("Тени облаков", "Cloud shadows"), &WosCloudShadow::enabled);
        ImGui::TableNextColumn();
        deferredChanged |= ImGui::Checkbox(WOS_UI("Лучи солнца", "Sun shafts"), &WosVolumetric::enabled);
        ImGui::TableNextColumn();
        deferredChanged |= ImGui::Checkbox(WOS_UI("Атмосферный туман", "Atmospheric fog"), &WosVolumetric::fogEnabled);
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
    if (ImGui::Checkbox(WOS_UI("Отражения в воде", "Water reflections"), &WosWaterSSR::enabled))
        WriteFloatIn("WaterSSR", "Enabled", WosWaterSSR::enabled ? 1.0f : 0.0f);

    int aa = WosDlss::enabled ? 4 : (WosSmaa::enabled ? 1 : 0) | (WosTaa::enabled ? 2 : 0);
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::Combo(WOS_UI("Сглаживание", "Anti-aliasing"), &aa, WOS_UI("Нет\0SMAA\0TAA\0SMAA + TAA\0DLAA (DXVK)\0\0", "Off\0SMAA\0TAA\0SMAA + TAA\0DLAA (DXVK)\0\0"))) {
        // DLAA runs on TAA's jitter, and TAA stands in whenever DLAA cannot run.
        WosDlss::enabled = aa == 4;
        WosSmaa::enabled = aa != 4 && (aa & 1) != 0;
        WosTaa::enabled = aa == 4 || (aa & 2) != 0;
        WriteFloatIn("SMAA", "Enabled", WosSmaa::enabled ? 1.0f : 0.0f);
        WriteFloatIn("TAA", "Enabled", WosTaa::enabled ? 1.0f : 0.0f);
        WriteFloatIn("DLSS", "Enabled", WosDlss::enabled ? 1.0f : 0.0f);
    }
    if (WosDlss::enabled) ImGui::TextDisabled("%s", WosTaa::faulted ? "DLAA needs TAA's jitter, and TAA has stopped" : WosDlss::status);

    ImGui::BeginDisabled(!WosDeferred::enabled);
    deferredChanged |= ImGui::Checkbox(WOS_UI("Дождь: мокрые улицы и лужи", "Rain: wet streets and puddles"), &WosDeferred::rainEnabled);
    if (WosDeferred::rainEnabled) {
        ImGui::SetNextItemWidth(200.0f);
        deferredChanged |= ImGui::SliderFloat(WOS_UI("Сила дождя", "Rainfall"), &WosDeferred::rainfallIntensity, 0.f, 1.f, "%.2f");
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText(WOS_UI("Погода", "Weather"));
    {
        const char *preview = g_selected_preset >= 0 && g_selected_preset < static_cast<int>(g_presets.size())
                                  ? g_presets[g_selected_preset].name.c_str() : WOS_UI("(свои облака)", "(custom clouds)");
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::BeginCombo(WOS_UI("Облака", "Clouds"), preview)) {
            for (int index = 0; index < static_cast<int>(g_presets.size()); ++index) {
                ImGui::PushID(index);
                if (ImGui::Selectable(g_presets[index].name.c_str(), index == g_selected_preset)) {
                    g_selected_preset = index;
                    ApplyPreset(g_presets[index]);
                    changed = true;
                }
                if (ImGui::IsItemHovered() && !g_presets[index].description.empty())
                    ImGui::SetTooltip("%s", g_presets[index].description.c_str());
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }

    ImGui::SeparatorText(WOS_UI("Изображение", "Image"));
    ImGui::SetNextItemWidth(200.0f);
    changed |= ImGui::Combo(WOS_UI("Тонмаппер", "Tone mapper"), &g_lighting.tone_mode,
                            WOS_UI("Как в игре\0AgX\0ACES\0\0", "Original\0AgX\0ACES\0\0"));
    if (g_lighting.tone_mode != 0) {
        ImGui::SetNextItemWidth(200.0f);
        changed |= ImGui::SliderFloat(WOS_UI("Экспозиция, EV", "Exposure, EV"), &g_lighting.exposure_ev, -3.0f, 3.0f, "%.2f");
    }

    ImGui::SeparatorText(WOS_UI("Экспериментальное", "Experimental"));
    ImGui::BeginDisabled(!WosDeferred::enabled);
    if (ImGui::Checkbox(WOS_UI("Свет фонарей в воздухе", "Lamp light shafts"), &WosVolumetric::localEnabled)) {
        WosLocalShadow::collectForShafts = WosVolumetric::localEnabled;
        deferredChanged = true;
    }
    Experimental();
    ImGui::EndDisabled();
    bool ssgi = WosIndirect::enabled;
    if (ImGui::Checkbox(WOS_UI("Отражённый свет (SSGI)", "Bounce light (SSGI)"), &ssgi)) {
        WosIndirect::enabled = ssgi;
        WosIndirect::Reset();
        if (ssgi) WosCascadeProbe::Reset();
        deferredChanged = true;
    }
    Experimental();
    if (ImGui::Checkbox(WOS_UI("Дым из труб", "Chimney smoke"), &WosChimneySmoke::enabled)) SaveChimneySmoke();
    Experimental();
    if (ImGui::Checkbox(WOS_UI("Мягкие частицы", "Soft particles"), &WosSoftParticles::enabled)) SaveSoftParticles();
    Experimental();
    bool nightReflection = WosNightReflection::mode != 0;
    if (ImGui::Checkbox(WOS_UI("Ночные отражения в окнах", "Night window reflections"), &nightReflection)) {
        WosNightReflection::mode = nightReflection ? 1 : 0;
        SaveNightReflection();
    }
    Experimental();
    if (ImGui::Checkbox(WOS_UI("Цикл дня и ночи", "Day/night cycle"), &WosDayCycle::enabled)) SaveDayCycle();
    Experimental();
    if (WosDayCycle::enabled) {
        if (ImGui::SliderInt(WOS_UI("Скорость (игровых сек/сек)", "Speed (game s per s)"), &WosDayCycle::rate, 1, 600)) SaveDayCycle();
        if (ImGui::Checkbox(WOS_UI("Солнце и луна движутся", "Sun and moon move"), &WosDayCycle::moveSun)) SaveDayCycle();
        const unsigned daySeconds = WosDayCycle::seconds % 86400;
        const int hour = int(daySeconds / 3600), minute = int(daySeconds / 60 % 60);
        ImGui::Text("%s %02d:%02d  %s", WOS_UI("Время", "Time"), hour, minute, WosDayCycle::tod);
        if (ImGui::Button("-1h")) WosDayCycle::SetTime(hour - 1, minute);
        ImGui::SameLine();
        if (ImGui::Button("+1h")) WosDayCycle::SetTime(hour + 1, minute);
        ImGui::SameLine();
        if (ImGui::Button("06:00")) WosDayCycle::SetTime(6, 0);
        ImGui::SameLine();
        if (ImGui::Button("12:00")) WosDayCycle::SetTime(12, 0);
        ImGui::SameLine();
        if (ImGui::Button("18:00")) WosDayCycle::SetTime(18, 0);
        ImGui::SameLine();
        if (ImGui::Button("00:00")) WosDayCycle::SetTime(0, 0);
    }
    {
        bool lampsOn = WosNightLamps::mode != 0;
        if (ImGui::Checkbox(WOS_UI("Фонари ночью", "Street lamps at night"), &lampsOn)) {
            WosNightLamps::mode = lampsOn ? 1 : 0;
            SaveNightLamps();
        }
        Experimental();
        if (lampsOn) {
            if (ImGui::SliderFloat(WOS_UI("Яркость фонарей", "Lamp brightness"), &WosNightLamps::intensity, 0.0f, 15.0f, "%.1f")) SaveNightLamps();
            if (ImGui::SliderInt(WOS_UI("Фонарей в кадре", "Lamps lit at once"), &WosNightLamps::maxLights, 1, 8)) SaveNightLamps();
        }
    }

    ImGui::Spacing();
    ImGui::TextDisabled("%s", WOS_UI("Тонкая настройка каждого эффекта - в расширенном режиме.",
                                     "Fine-tune every effect in Advanced settings."));
}

// Our sky, cloud and bloom shaders are exWoS mod folders; without them listed
// in exWoS.config.ini none of the effects has a pass to attach to. Say so and
// offer to add the lines, instead of leaving a menu that changes nothing.
static std::string ExWosConfigPath()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(g_module, path, MAX_PATH);
    std::string result(path);
    result.resize(result.find_last_of("\\/") + 1);
    return result + "exWoS\\exWoS.config.ini";
}

static std::string g_setup_status;

static void DrawSetupWarnings()
{
    // A few seconds of frames: the city and its post-process are drawn by then.
    if (!g_mod_shaders_seen && g_frame_index > 600) {
        const std::string config = ExWosConfigPath();
        const bool haveConfig = GetFileAttributesA(config.c_str()) != INVALID_FILE_ATTRIBUTES;
        ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.35f, 1.0f), "%s",
                           WOS_UI("Шейдеры мода не загружены - эффекты не работают.", "Mod shaders are not loaded - effects are inactive."));
        if (!haveConfig) {
            ImGui::TextWrapped("%s", WOS_UI("Не найден exWoS (exWoS\\exWoS.config.ini). Установите exWoS, затем перезапустите игру.",
                                            "exWoS was not found (exWoS\\exWoS.config.ini). Install exWoS, then restart the game."));
        } else {
            ImGui::TextWrapped("%s", WOS_UI("В exWoS.config.ini не включены Clouds_LiveMenu, GtaStyleLighting и Sky_NoLegacy.",
                                            "Clouds_LiveMenu, GtaStyleLighting and Sky_NoLegacy are not enabled in exWoS.config.ini."));
            if (ImGui::Button(WOS_UI("Включить их в exWoS.config.ini", "Enable them in exWoS.config.ini"))) {
                const bool ok = WritePrivateProfileStringA("mods", "Clouds_LiveMenu", "30", config.c_str()) &&
                                WritePrivateProfileStringA("mods", "GtaStyleLighting", "40", config.c_str()) &&
                                WritePrivateProfileStringA("mods", "Sky_NoLegacy", "50", config.c_str()) &&
                                WritePrivateProfileStringA("effects", "BloomPipeline", "1", config.c_str());
                g_setup_status = ok ? WOS_UI("Готово. Перезапустите игру.", "Done. Restart the game.")
                                    : WOS_UI("Не удалось записать exWoS.config.ini.", "Could not write exWoS.config.ini.");
            }
        }
        if (!g_setup_status.empty()) ImGui::TextColored(ImVec4(0.45f, 0.90f, 0.45f, 1.0f), "%s", g_setup_status.c_str());
        ImGui::Separator();
    } else if (g_mod_shaders_seen && g_frame_index > 600 && !g_volumetric_last && g_cam_valid) {
        ImGui::TextColored(ImVec4(1.0f, 0.70f, 0.20f, 1.0f), "%s",
                           WOS_UI("Включите Depth of Field в настройках графики игры: без него нет облаков и тумана.",
                                  "Enable Depth of Field in the game's graphics options: clouds and fog need it."));
        ImGui::Separator();
    }
}

static void DrawMenu()
{
    ImGui::SetNextWindowSize(ImVec2(520.0f, 620.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(WOS_UI("WoS - Live Settings", "WoS Graphics"), &g_menu_visible, ImGuiWindowFlags_NoCollapse);

    bool changed = false;
    bool deferredChanged = false;

    // Reserve the footer first: the tab body is a child that fills whatever is
    // left, so every tab scrolls on its own and the window never jumps size.
    const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing();

    if (ImGui::Checkbox(WOS_UI("Расширенные настройки", "Advanced settings"), &g_menu_advanced))
        WriteFloatIn("Menu", "Advanced", g_menu_advanced ? 1.0f : 0.0f);
    ImGui::Separator();
    DrawSetupWarnings();

    if (!g_menu_advanced) {
        ImGui::BeginChild("##simple", ImVec2(0.0f, -footer), false);
        DrawSimplePage(changed, deferredChanged);
        ImGui::EndChild();
    } else if (ImGui::BeginTabBar("wos_tabs", ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_TabListPopupButton)) {
        struct Tab { const char *name; int id; };
        static const Tab tabs[] = {
            {"Пресеты", 0}, {"Облака", 1}, {"Атмосфера", 2}, {"Свет", 3},
            {"Тени", 4}, {"Туман", 5}, {"Дождь", 6}, {"Вода", 7},
            {"Сглаживание", 8}, {"GI", 9}, {"Дым", 11}, {"Частицы", 12},
#ifndef WOS_RELEASE_UI
            {"Диагностика", 10},
#endif
        };
        for (const Tab &tab : tabs) {
            if (!ImGui::BeginTabItem(tab.name)) continue;
            ImGui::BeginChild("##body", ImVec2(0.0f, -footer), false);
            switch (tab.id) {
            case 0: DrawPresetTab(changed); break;
            case 1: DrawVolumeTab(changed); break;
            case 2: DrawLightingTab(changed); break;
            case 3: DrawDeferredLightTab(deferredChanged); break;
            case 4: DrawShadowTab(deferredChanged); break;
            case 5: DrawVolumetricTab(deferredChanged); break;
            case 6: DrawRainTab(deferredChanged); break;
            case 7: DrawWaterTab(); break;
            case 11: DrawSmokeTab(); break;
            case 12: DrawParticlesTab(); break;
            case 8: DrawAntiAliasTab(); break;
            case 9: DrawIndirectTab(); break;
            default: DrawDiagnosticsTab(deferredChanged); break;
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    ImGui::TextDisabled("%s", WOS_UI("F3 — меню, F4 — трек камеры. Изменения применяются и сохраняются сразу.",
                                     "F3: this menu. Changes apply and save immediately."));

    if (deferredChanged) { WosDeferred::SaveSettings(); WosDeferred::WriteStatus(true); }
    if (changed) SaveSettings();
    ImGui::End();
}

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static LRESULT CALLBACK CloudWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (g_menu_visible && ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam))
        return TRUE;
    if (g_menu_visible && ImGui::GetCurrentContext()) {
        const ImGuiIO &io = ImGui::GetIO();
        if (io.WantCaptureMouse || io.WantCaptureKeyboard)
            return TRUE;
    }
    return g_original_wndproc
        ? CallWindowProcW(g_original_wndproc, hwnd, message, wparam, lparam)
        : DefWindowProcW(hwnd, message, wparam, lparam);
}

static void InitImGui(IDirect3DDevice9 *device)
{
    if (g_imgui_ready) return;
    D3DDEVICE_CREATION_PARAMETERS creation = {};
    if (FAILED(device->GetCreationParameters(&creation))) return;
    g_window = creation.hFocusWindow;
    if (!g_window) return;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.0f, nullptr, io.Fonts->GetGlyphRangesCyrillic());
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 3.0f;
    style.FrameRounding = 2.0f;
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.05f, 0.20f, 0.38f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.18f, 0.58f, 0.95f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.35f, 0.75f, 1.0f, 1.0f);

    if (!ImGui_ImplWin32_Init(g_window) || !ImGui_ImplDX9_Init(device)) return;
    g_original_wndproc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(CloudWndProc)));
    LoadSettings();
    RefreshPresets();
    g_imgui_ready = true;
}

static unsigned RehookDevice(IDirect3DDevice9*device);
static HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9 *device, D3DPRESENT_PARAMETERS *params)
{
    // Module resets below still set targets, states and constants on their way
    // out, and the device returns to its defaults inside Reset: the eligibility
    // shadow learns nothing until the call is done, then both caches start clean.
    WosDeferredShadow::BeginDeviceReset();
    WosSmaa::Reset();WosConstCache::OnReset();
    WosTaa::Reset();
    WosDlss::Reset();
    WosWaterSSR::Reset();
    WosChimneySmoke::Reset();
    WosStreetSteam::Reset();
    WosSoftParticles::Reset();
    WosNightReflection::Reset();
    WosCloudLowRes::Reset();
    WosDeferredControl::Reset();
    WosDeferred::Reset();
    WosNativeClear::Finish("RESET");
    WosNormals::Reset();
    WosPCSS::Reset();
    WosProbe::Finish("DEVICE_RESET");
    WosCascadeProbe::Reset();
    WosCascadeMaterial::Reset();
    WosNativeMaterialContext::Finish("RESET");
    WosRSMGeometry::Reset();
    WosIndirect::Reset();
    ReleaseDepthResources();
    if (g_imgui_ready) ImGui_ImplDX9_InvalidateDeviceObjects();
    const HRESULT result = g_original_reset(device, params);
    WosDeferredShadow::EndDeviceReset(result);RehookDevice(device);WosConstCache::OnReset();
    if(SUCCEEDED(result))WosNativeRSM::Initialize(device);
    if (g_imgui_ready && SUCCEEDED(result)) ImGui_ImplDX9_CreateDeviceObjects();
    return result;
}

static HRESULT STDMETHODCALLTYPE HookBeginScene(IDirect3DDevice9 *device)
{
    RehookDevice(device);
    WosDeferred::ReleaseMrtForNative(device);
    WosNativeClear::BeginIfActive();
    if(WosIndirect::enabled||WosDeferred::enabled){WosIndirectCamera::Begin(reinterpret_cast<uintptr_t>(_ReturnAddress()));if(reinterpret_cast<uintptr_t>(_ReturnAddress())==0x009dcf3a)WosIndirectMaterials::BeginFrame();}
    WosEngineProbe::BeforeBeginScene(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    try {WosCascadeProbe::Prepare(device,reinterpret_cast<uintptr_t>(_ReturnAddress()));if(WosCascadeProbe::ready&&!WosLocalShadow::valid)WosLocalShadow::Prepare(device);} catch(...) {WosCascadeProbe::Reset();WosCascadeProbe::status="Cascade preparation failed";}
    return g_original_begin_scene(device);
}

static HRESULT STDMETHODCALLTYPE HookEndScene(IDirect3DDevice9 *device)
{
    WosDeferred::Flush(device);
    InitImGui(device);
    ++g_frame_index;
    g_track_sampled = false;
    g_cam_captured_this_frame = false;
    g_cam_valid = false;
    g_inverse_projection_valid = false;
    g_depth_captured_this_frame = false;
    g_volumetric_last = g_volumetric_seen;
    g_volumetric_seen = false;
    WosNightReflection::UpdateSky(g_style_palette.valid, g_style_palette.zenith);
    WosNightReflection::WriteStatus();
    WosDayCycle::Update();
    WosDayCycle::WriteStatus();
    WosNightLamps::Update(WosDayCycle::enabled && WosDayCycle::clockValid, WosDayCycle::seconds, WosNightReflection::night);
    WosNightLamps::WriteStatus();
    MaybeEndDump();
    if ((GetAsyncKeyState(VK_F6) & 1) != 0) BeginDump();
    if ((GetAsyncKeyState(VK_F4) & 1) != 0) ToggleRecording();
    if ((GetAsyncKeyState(VK_F3) & 1) != 0) {
        g_menu_visible = !g_menu_visible;
        for (bool &button : g_di_buttons) button = false;
        g_di_wheel = 0.0f;
        if (g_menu_visible) {
            g_cursor_placed = false;
            ClipCursor(nullptr);
        }
    }
    if (g_imgui_ready) {
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGuiIO &io = ImGui::GetIO();
        io.MouseDrawCursor = g_menu_visible;
        if (g_menu_visible && g_di_mouse_seen) {
            io.AddMousePosEvent(g_cursor_x, g_cursor_y);
            for (int button = 0; button < 3; ++button)
                io.AddMouseButtonEvent(button, g_di_buttons[button]);
            if (g_di_wheel != 0.0f) {
                io.AddMouseWheelEvent(0.0f, g_di_wheel);
                g_di_wheel = 0.0f;
            }
        }
        ImGui::NewFrame();
        if (g_menu_visible) DrawMenu();
        ImGui::EndFrame();
        ImGui::Render();
        WosProbe::suppress = true;
        {ImDrawData*drawData=ImGui::GetDrawData();if(drawData&&drawData->TotalVtxCount>0)ImGui_ImplDX9_RenderDrawData(drawData);}
        WosProbe::suppress = false;
    }
    return g_original_end_scene(device);
}

static HRESULT STDMETHODCALLTYPE HookSetPixelShader(IDirect3DDevice9 *device, IDirect3DPixelShader9 *shader)
{
    WosHookProfile::Setter prof;
    g_current_shader = shader;
    const HRESULT result=g_original_set_pixel_shader(device, shader);prof.Native();
    if(SUCCEEDED(result)){WosNativeRSM::RegisterPS(device,shader);WosDeferredShadow::OnSetPixelShader(shader);}
    return result;
}

static HRESULT STDMETHODCALLTYPE HookSetVertexShader(IDirect3DDevice9*d,IDirect3DVertexShader9*p){WosHookProfile::Setter prof;const HRESULT hr=g_original_set_vertex_shader(d,p);prof.Native();if(SUCCEEDED(hr))WosNativeRSM::RegisterVS(d,p);return hr;}
static HRESULT STDMETHODCALLTYPE HookSetTexture(IDirect3DDevice9*d,DWORD stage,IDirect3DBaseTexture9*t){WosHookProfile::Setter prof;t=WosNightReflection::Substitute(d,t);const HRESULT hr=g_original_set_texture(d,stage,t);prof.Native();if(SUCCEEDED(hr))WosNativeRSM::RegisterTexture(d,t);return hr;}
#include "../../../WoS_Water/native_water.h"
static HRESULT STDMETHODCALLTYPE HookCreatePixelShader(IDirect3DDevice9*d,const DWORD*bytes,IDirect3DPixelShader9**out){
    const HRESULT hr=g_original_create_pixel_shader(d,bytes,out);
    if(SUCCEEDED(hr)&&out&&!WosDeferred::busy){
        WosWater::Replace(d,out,g_original_create_pixel_shader,g_module);
        WosSoftParticles::Register(*out,bytes,true);
        WosNativeRSM::RegisterPS(d,*out);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE HookCreateVertexShader(IDirect3DDevice9*d,const DWORD*bytes,IDirect3DVertexShader9**out){const HRESULT hr=g_original_create_vertex_shader(d,bytes,out);if(SUCCEEDED(hr)&&out){WosNativeRSM::RegisterVS(d,*out);WosSoftParticles::Register(*out,bytes,false);}return hr;}
static HRESULT STDMETHODCALLTYPE HookCreateTexture(IDirect3DDevice9*d,UINT w,UINT h,UINT levels,DWORD usage,D3DFORMAT format,D3DPOOL pool,IDirect3DTexture9**out,HANDLE*shared){const HRESULT hr=g_original_create_texture(d,w,h,levels,usage,format,pool,out,shared);if(SUCCEEDED(hr)&&out&&!WosDeferred::busy)WosNativeRSM::RegisterTexture(d,*out);return hr;}

static HRESULT STDMETHODCALLTYPE HookDrawPrimitive(IDirect3DDevice9 *device, D3DPRIMITIVETYPE type, UINT start, UINT count)
{
    WosHookProfile::Draw prof;
    HRESULT result;
    {
    WosShadowWork::HookTimer hookTimer;
    NativeSmaaScope smaaScope(device);prof.Mark(WosHookProfile::Smaa);
    WosNativeMaterialContext::Observe(device,WosNativeMaterialContext::Kind::DP,reinterpret_cast<uintptr_t>(_ReturnAddress()),reinterpret_cast<uintptr_t>(_AddressOfReturnAddress()),WosNativeMaterialContext::DrawArgs::Primitive(type,start,count));prof.Mark(WosHookProfile::MaterialContext);
    WosNativeRSM::DrawScope nativeRsmScope(device,WosNativeMaterialContext::Kind::DP,reinterpret_cast<uintptr_t>(_ReturnAddress()),reinterpret_cast<uintptr_t>(_AddressOfReturnAddress()),WosNativeMaterialContext::DrawArgs::Primitive(type,start,count));prof.Mark(WosHookProfile::NativeRsm);
    if(WosIndirect::enabled||WosDeferred::enabled)WosIndirectCamera::Observe(device,count);
    if(!WosProbe::suppress&&!WosDeferred::busy){WosChimneySmoke::Observe(device);WosDayCycle::ObserveDraw();}prof.Mark(WosHookProfile::CameraObserve);
    ApplyShaderConstants(device, count);prof.Mark(WosHookProfile::ShaderConstants);
    ProbeDraw(device, count);
    WosPCSS::DrawScope pcssScope(device);
    WosNormals::DrawScope normalScope(device);
    WosCascadeMaterial::DrawScope cascadeScope(device);prof.Mark(WosHookProfile::PreScopes);
    WosDeferred::DrawScope deferredScope(device,!nativeRsmScope.OwnsObservation());prof.Mark(WosHookProfile::Deferred);
    // Held G-buffer targets would make the GI material scope bypass the frame.
    if(WosIndirect::enabled&&!deferredScope.Handled())WosDeferred::ReleaseMrtForNative(device);
    WosIndirectMaterials::DrawScope materialScope(device,WosIndirect::enabled && !deferredScope.Handled() && !WosCascadeProbe::continuous && !WosCascadeProbe::active,true); hookTimer.kind=(materialScope.changed||materialScope.deferred)?WosShadowWork::HookGiTargets:(deferredScope.Handled()?WosShadowWork::HookDeferred:WosShadowWork::HookOther);prof.Mark(WosHookProfile::IndirectMaterials);
    if(ClassifyShader(g_current_shader)!=kShaderOther)WosDeferred::ReleaseMrtForNative(device);
    WosWaterSSR::BloomDraw(device, ClassifyShader(g_current_shader)==kShaderBloom);
    WosChimneySmoke::BloomDraw(device, ClassifyShader(g_current_shader)==kShaderBloom);
    WosStreetSteam::BloomDraw(device, ClassifyShader(g_current_shader)==kShaderBloom);
    WosVolumetricBridge::Draw(device, ClassifyShader(g_current_shader)==kShaderBloom);
    WosCloudLowRes::BloomScope cloudScope(device, ClassifyShader(g_current_shader)==kShaderBloom,
        g_lighting.enabled && g_cam_valid && g_depth_valid && g_inverse_projection_valid, g_cam_pos, g_inverse_view_projection);
    WosVolumetric::SkyScope lampSkyScope(device, ClassifyShader(g_current_shader)==kShaderBloom);
    if(ClassifyShader(g_current_shader)!=kShaderOther)WosDeferred::ReleaseMrtForNative(device);
    if(!WosProbe::suppress&&!WosDeferred::busy&&ClassifyShader(g_current_shader)==kShaderCloud)WosSkyIBL::Capture(device,WosIndirectCamera::CurrentScene());prof.Mark(WosHookProfile::BloomPasses);
    WosIndirect::DrawScope indirectScope(device);
    WosProbe::Draw(device, "DP", type, count, reinterpret_cast<uintptr_t>(_ReturnAddress()));prof.Mark(WosHookProfile::IndirectProbe);
    WosCascadeProbe::Replay(device,[&](){return g_original_draw_primitive(device, type, start, count);});prof.Mark(WosHookProfile::CascadeReplay);
    WosLocalShadow::Replay(device,WosLocalShadow::DrawArgs{0u,uint32_t(type),0u,0u,0u,uint32_t(start),uint32_t(count)},[&](){return g_original_draw_primitive(device, type, start, count);});prof.Mark(WosHookProfile::LocalReplay);
    WosRSMGeometry::DrawScope rsmScope(device,!nativeRsmScope.OwnsObservation());
    WosSoftParticles::Scope softScope(device);
    WosTaa::JitterScope taaJitter(device,!WosProbe::suppress&&!WosDeferred::busy&&WosIndirectCamera::frameReady);prof.Mark(WosHookProfile::PostScopes);
    result=nativeRsmScope.Execute([&](){return rsmScope.Execute([&](){return materialScope.Execute([&](){return deferredScope.Execute([&](){return taaJitter.Draw([&](){return WosWaterSSR::Draw(device,[&](){return softScope.Draw([&](){return WosHookProfile::TimeNative([&](){return g_original_draw_primitive(device, type, start, count);});});});});});});});});
    prof.Mark(WosHookProfile::Execute);
    smaaScope.success=SUCCEEDED(result);
    }
    prof.Mark(WosHookProfile::Destructors);
    return result;
}

static HRESULT STDMETHODCALLTYPE HookDrawIndexedPrimitive(IDirect3DDevice9 *device, D3DPRIMITIVETYPE type, INT base, UINT min_index, UINT vertices, UINT start, UINT primitives)
{
    WosHookProfile::Draw prof;
    HRESULT result;
    {
    WosShadowWork::HookTimer hookTimer;
    NativeSmaaScope smaaScope(device);prof.Mark(WosHookProfile::Smaa);
    WosNativeMaterialContext::Observe(device,WosNativeMaterialContext::Kind::DIP,reinterpret_cast<uintptr_t>(_ReturnAddress()),reinterpret_cast<uintptr_t>(_AddressOfReturnAddress()),WosNativeMaterialContext::DrawArgs::Indexed(type,base,min_index,vertices,start,primitives));prof.Mark(WosHookProfile::MaterialContext);
    WosNativeRSM::DrawScope nativeRsmScope(device,WosNativeMaterialContext::Kind::DIP,reinterpret_cast<uintptr_t>(_ReturnAddress()),reinterpret_cast<uintptr_t>(_AddressOfReturnAddress()),WosNativeMaterialContext::DrawArgs::Indexed(type,base,min_index,vertices,start,primitives));prof.Mark(WosHookProfile::NativeRsm);
    if(WosIndirect::enabled||WosDeferred::enabled)WosIndirectCamera::Observe(device,primitives);
    if(!WosProbe::suppress&&!WosDeferred::busy){WosChimneySmoke::Observe(device);WosDayCycle::ObserveDraw();}prof.Mark(WosHookProfile::CameraObserve);
    ApplyShaderConstants(device, primitives);prof.Mark(WosHookProfile::ShaderConstants);
    ProbeDraw(device, primitives);
    WosPCSS::DrawScope pcssScope(device);
    WosNormals::DrawScope normalScope(device);
    WosCascadeMaterial::DrawScope cascadeScope(device);prof.Mark(WosHookProfile::PreScopes);
    WosDeferred::DrawScope deferredScope(device,!nativeRsmScope.OwnsObservation());prof.Mark(WosHookProfile::Deferred);
    // Held G-buffer targets would make the GI material scope bypass the frame.
    if(WosIndirect::enabled&&!deferredScope.Handled())WosDeferred::ReleaseMrtForNative(device);
    WosIndirectMaterials::DrawScope materialScope(device,WosIndirect::enabled && !deferredScope.Handled() && !WosCascadeProbe::continuous && !WosCascadeProbe::active,true); hookTimer.kind=(materialScope.changed||materialScope.deferred)?WosShadowWork::HookGiTargets:(deferredScope.Handled()?WosShadowWork::HookDeferred:WosShadowWork::HookOther);prof.Mark(WosHookProfile::IndirectMaterials);
    if(ClassifyShader(g_current_shader)!=kShaderOther)WosDeferred::ReleaseMrtForNative(device);
    WosWaterSSR::BloomDraw(device, ClassifyShader(g_current_shader)==kShaderBloom);
    WosChimneySmoke::BloomDraw(device, ClassifyShader(g_current_shader)==kShaderBloom);
    WosStreetSteam::BloomDraw(device, ClassifyShader(g_current_shader)==kShaderBloom);
    WosVolumetricBridge::Draw(device, ClassifyShader(g_current_shader)==kShaderBloom);
    WosCloudLowRes::BloomScope cloudScope(device, ClassifyShader(g_current_shader)==kShaderBloom,
        g_lighting.enabled && g_cam_valid && g_depth_valid && g_inverse_projection_valid, g_cam_pos, g_inverse_view_projection);
    WosVolumetric::SkyScope lampSkyScope(device, ClassifyShader(g_current_shader)==kShaderBloom);
    if(ClassifyShader(g_current_shader)!=kShaderOther)WosDeferred::ReleaseMrtForNative(device);
    if(!WosProbe::suppress&&!WosDeferred::busy&&ClassifyShader(g_current_shader)==kShaderCloud)WosSkyIBL::Capture(device,WosIndirectCamera::CurrentScene());prof.Mark(WosHookProfile::BloomPasses);
    WosIndirect::DrawScope indirectScope(device);
    WosProbe::Draw(device, "DIP", type, primitives, reinterpret_cast<uintptr_t>(_ReturnAddress()));prof.Mark(WosHookProfile::IndirectProbe);
    WosCascadeProbe::Replay(device,[&](){return g_original_draw_indexed_primitive(device, type, base, min_index, vertices, start, primitives);});prof.Mark(WosHookProfile::CascadeReplay);
    WosLocalShadow::Replay(device,WosLocalShadow::DrawArgs{1u,uint32_t(type),uint32_t(base),uint32_t(min_index),uint32_t(vertices),uint32_t(start),uint32_t(primitives)},[&](){return g_original_draw_indexed_primitive(device, type, base, min_index, vertices, start, primitives);});prof.Mark(WosHookProfile::LocalReplay);
    WosRSMGeometry::DrawScope rsmScope(device,!nativeRsmScope.OwnsObservation());
    WosSoftParticles::Scope softScope(device);
    WosTaa::JitterScope taaJitter(device,!WosProbe::suppress&&!WosDeferred::busy&&WosIndirectCamera::frameReady);prof.Mark(WosHookProfile::PostScopes);
    result=nativeRsmScope.Execute([&](){return rsmScope.Execute([&](){return materialScope.Execute([&](){return deferredScope.Execute([&](){return taaJitter.Draw([&](){return WosWaterSSR::Draw(device,[&](){return softScope.Draw([&](){return WosHookProfile::TimeNative([&](){return g_original_draw_indexed_primitive(device, type, base, min_index, vertices, start, primitives);});});});});});});});});
    prof.Mark(WosHookProfile::Execute);
    smaaScope.success=SUCCEEDED(result);
    }
    prof.Mark(WosHookProfile::Destructors);
    return result;
}

// Post-processing quads come through the user-pointer draw calls, so the hooks
// below are what actually see the fullscreen passes.
static HRESULT STDMETHODCALLTYPE HookDrawPrimitiveUP(IDirect3DDevice9 *device, D3DPRIMITIVETYPE type,
                                                     UINT primitives, const void *vertices, UINT stride)
{
    NativeSmaaScope smaaScope(device);
    WosNativeRSM::DrawScope nativeRsmScope(device);
    if(WosIndirect::enabled||WosDeferred::enabled)WosIndirectCamera::Observe(device,primitives);
    ApplyShaderConstants(device, primitives);
    WosPCSS::DrawScope pcssScope(device);
    WosNormals::DrawScope normalScope(device);
    WosCascadeMaterial::DrawScope cascadeScope(device);
    WosDeferred::DrawScope deferredScope(device,!nativeRsmScope.OwnsObservation());
    // Held G-buffer targets would make the GI material scope bypass the frame.
    if(WosIndirect::enabled&&!deferredScope.Handled())WosDeferred::ReleaseMrtForNative(device);
    WosIndirectMaterials::DrawScope materialScope(device,WosIndirect::enabled && !deferredScope.Handled() && !WosCascadeProbe::continuous && !WosCascadeProbe::active,false);
    if(ClassifyShader(g_current_shader)!=kShaderOther)WosDeferred::ReleaseMrtForNative(device);
    WosWaterSSR::BloomDraw(device, ClassifyShader(g_current_shader)==kShaderBloom);
    WosChimneySmoke::BloomDraw(device, ClassifyShader(g_current_shader)==kShaderBloom);
    WosStreetSteam::BloomDraw(device, ClassifyShader(g_current_shader)==kShaderBloom);
    WosVolumetricBridge::Draw(device, ClassifyShader(g_current_shader)==kShaderBloom);
    WosCloudLowRes::BloomScope cloudScope(device, ClassifyShader(g_current_shader)==kShaderBloom,
        g_lighting.enabled && g_cam_valid && g_depth_valid && g_inverse_projection_valid, g_cam_pos, g_inverse_view_projection);
    WosVolumetric::SkyScope lampSkyScope(device, ClassifyShader(g_current_shader)==kShaderBloom);
    if(ClassifyShader(g_current_shader)!=kShaderOther)WosDeferred::ReleaseMrtForNative(device);
    if(!WosProbe::suppress&&!WosDeferred::busy&&ClassifyShader(g_current_shader)==kShaderCloud)WosSkyIBL::Capture(device,WosIndirectCamera::CurrentScene());
    WosIndirect::DrawScope indirectScope(device);
    WosProbe::Draw(device, "DPUP", type, primitives, reinterpret_cast<uintptr_t>(_ReturnAddress()));
    WosRSMGeometry::DrawScope rsmScope(device,!nativeRsmScope.OwnsObservation());
    WosSoftParticles::Scope softScope(device);
    WosTaa::JitterScope taaJitter(device,!WosProbe::suppress&&!WosDeferred::busy&&WosIndirectCamera::frameReady);
    const HRESULT result=nativeRsmScope.Execute([&](){return rsmScope.Execute([&](){return materialScope.Execute([&](){return deferredScope.Execute([&](){return taaJitter.Draw([&](){return softScope.Draw([&](){return g_original_draw_primitive_up(device, type, primitives, vertices, stride);});});});});});});
    smaaScope.success=SUCCEEDED(result);
    return result;
}

static HRESULT STDMETHODCALLTYPE HookDrawIndexedPrimitiveUP(IDirect3DDevice9 *device, D3DPRIMITIVETYPE type,
                                                            UINT min_index, UINT vertices, UINT primitives,
                                                            const void *indices, D3DFORMAT index_format,
                                                            const void *vertex_data, UINT stride)
{
    NativeSmaaScope smaaScope(device);
    WosNativeRSM::DrawScope nativeRsmScope(device);
    if(WosIndirect::enabled||WosDeferred::enabled)WosIndirectCamera::Observe(device,primitives);
    ApplyShaderConstants(device, primitives);
    WosPCSS::DrawScope pcssScope(device);
    WosNormals::DrawScope normalScope(device);
    WosCascadeMaterial::DrawScope cascadeScope(device);
    WosDeferred::DrawScope deferredScope(device,!nativeRsmScope.OwnsObservation());
    // Held G-buffer targets would make the GI material scope bypass the frame.
    if(WosIndirect::enabled&&!deferredScope.Handled())WosDeferred::ReleaseMrtForNative(device);
    WosIndirectMaterials::DrawScope materialScope(device,WosIndirect::enabled && !deferredScope.Handled() && !WosCascadeProbe::continuous && !WosCascadeProbe::active,false);
    if(ClassifyShader(g_current_shader)!=kShaderOther)WosDeferred::ReleaseMrtForNative(device);
    WosWaterSSR::BloomDraw(device, ClassifyShader(g_current_shader)==kShaderBloom);
    WosChimneySmoke::BloomDraw(device, ClassifyShader(g_current_shader)==kShaderBloom);
    WosStreetSteam::BloomDraw(device, ClassifyShader(g_current_shader)==kShaderBloom);
    WosVolumetricBridge::Draw(device, ClassifyShader(g_current_shader)==kShaderBloom);
    WosCloudLowRes::BloomScope cloudScope(device, ClassifyShader(g_current_shader)==kShaderBloom,
        g_lighting.enabled && g_cam_valid && g_depth_valid && g_inverse_projection_valid, g_cam_pos, g_inverse_view_projection);
    WosVolumetric::SkyScope lampSkyScope(device, ClassifyShader(g_current_shader)==kShaderBloom);
    if(ClassifyShader(g_current_shader)!=kShaderOther)WosDeferred::ReleaseMrtForNative(device);
    if(!WosProbe::suppress&&!WosDeferred::busy&&ClassifyShader(g_current_shader)==kShaderCloud)WosSkyIBL::Capture(device,WosIndirectCamera::CurrentScene());
    WosIndirect::DrawScope indirectScope(device);
    WosProbe::Draw(device, "DIPUP", type, primitives, reinterpret_cast<uintptr_t>(_ReturnAddress()));
    WosRSMGeometry::DrawScope rsmScope(device,!nativeRsmScope.OwnsObservation());
    WosSoftParticles::Scope softScope(device);
    WosTaa::JitterScope taaJitter(device,!WosProbe::suppress&&!WosDeferred::busy&&WosIndirectCamera::frameReady);
    const HRESULT result=nativeRsmScope.Execute([&](){return rsmScope.Execute([&](){return materialScope.Execute([&](){return deferredScope.Execute([&](){return taaJitter.Draw([&](){return softScope.Draw([&](){return g_original_draw_indexed_primitive_up(device, type, min_index, vertices, primitives,
                                                indices, index_format, vertex_data, stride);});});});});});});
    smaaScope.success=SUCCEEDED(result);
    return result;
}

static HRESULT STDMETHODCALLTYPE HookClear(IDirect3DDevice9 *device, DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
    const uintptr_t caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    if(WosDeferred::busy) return WosNativeClear::originalClear(device,count,rects,flags,color,z,stencil);
    WosDeferred::ReleaseMrtForNative(device);
    WosDeferred::Flush(device);
    if(!WosProbe::suppress&&(flags&D3DCLEAR_ZBUFFER))WosPrimaryGIProvider::ClearDepth(device);
    WosRsmRuntime::ProductionBridge::NativeClearScope scope(WosNativeRSM::bridge,device,count,rects,flags,z,caller);
    const HRESULT hr=WosNativeClear::Invoke(device,count,rects,flags,color,z,stencil,caller);
    scope.Complete(hr);return hr;
}


using DeferredSetRTFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,DWORD,IDirect3DSurface9*);
using DeferredSetDSFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,IDirect3DSurface9*);
using DeferredStretchFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,IDirect3DSurface9*,const RECT*,IDirect3DSurface9*,const RECT*,D3DTEXTUREFILTERTYPE);
using DeferredFillFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,IDirect3DSurface9*,const RECT*,D3DCOLOR);
static DeferredSetRTFn deferredOriginalRT=nullptr;
static DeferredSetDSFn deferredOriginalDS=nullptr;
static DeferredStretchFn deferredOriginalStretch=nullptr;
static DeferredFillFn deferredOriginalFill=nullptr;
static bool DeferredBarrier(){return WosDeferred::pending&&!WosDeferred::busy&&!WosProbe::suppress;}
static HRESULT STDMETHODCALLTYPE DeferredSetRT(IDirect3DDevice9*d,DWORD slot,IDirect3DSurface9*s){
    WosDeferred::ReleaseMrtForNative(d);
    if(DeferredBarrier()&&(slot==0&&s!=WosDeferred::sceneColor))WosDeferred::Flush(d);
    HRESULT hr=deferredOriginalRT(d,slot,s);if(SUCCEEDED(hr))WosDeferredShadow::OnSetRenderTarget(slot,s);return hr;
}
static HRESULT STDMETHODCALLTYPE DeferredSetDS(IDirect3DDevice9*d,IDirect3DSurface9*s){
    WosDeferred::ReleaseMrtForNative(d);
    if(DeferredBarrier()&&s!=WosDeferred::sceneDepth)WosDeferred::Flush(d);
    HRESULT hr=deferredOriginalDS(d,s);if(SUCCEEDED(hr))WosDeferredShadow::OnSetDepthStencil(s);return hr;
}
// Eligibility shadow feeds: every setter the shadow relies on reports here.
using DeferredSetRSFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,D3DRENDERSTATETYPE,DWORD);
using DeferredSetVPFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,const D3DVIEWPORT9*);
static DeferredSetRSFn deferredOriginalRS=nullptr;
static DeferredSetVPFn deferredOriginalVP=nullptr;
static HRESULT STDMETHODCALLTYPE DeferredSetRS(IDirect3DDevice9*d,D3DRENDERSTATETYPE type,DWORD value){
    if(type==D3DRS_COLORWRITEENABLE1||type==D3DRS_COLORWRITEENABLE2||type==D3DRS_COLORWRITEENABLE3)WosDeferred::ReleaseMrtForNative(d);
    WosHookProfile::Setter prof;HRESULT hr=deferredOriginalRS(d,type,value);prof.Native();if(SUCCEEDED(hr))WosDeferredShadow::OnSetRenderState(type,value);return hr;
}
static HRESULT STDMETHODCALLTYPE DeferredSetVP(IDirect3DDevice9*d,const D3DVIEWPORT9*value){
    WosHookProfile::Setter prof;HRESULT hr=deferredOriginalVP(d,value);prof.Native();if(SUCCEEDED(hr))WosDeferredShadow::OnSetViewport(value);return hr;
}
static HRESULT STDMETHODCALLTYPE DeferredStretch(IDirect3DDevice9*d,IDirect3DSurface9*a,const RECT*ar,IDirect3DSurface9*b,const RECT*br,D3DTEXTUREFILTERTYPE f){
    WosDeferred::ReleaseMrtForNative(d);
    if(DeferredBarrier()&&(a==WosDeferred::sceneColor||b==WosDeferred::sceneColor||a==WosDeferred::sceneDepth||b==WosDeferred::sceneDepth))WosDeferred::Flush(d);
    return deferredOriginalStretch(d,a,ar,b,br,f);
}
static HRESULT STDMETHODCALLTYPE DeferredFill(IDirect3DDevice9*d,IDirect3DSurface9*s,const RECT*r,D3DCOLOR c){
    WosDeferred::ReleaseMrtForNative(d);
    if(DeferredBarrier()&&s==WosDeferred::sceneColor)WosDeferred::Flush(d);
    return deferredOriginalFill(d,s,r,c);
}
// Diagnostic counters only. State blocks bypass every per-call hook, so their
// frequency decides whether device state may be cached between draws.
using DeferredCreateSBFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,D3DSTATEBLOCKTYPE,IDirect3DStateBlock9**);
using DeferredBeginSBFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using DeferredEndSBFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,IDirect3DStateBlock9**);
using DeferredSBApplyFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DStateBlock9*);
static DeferredCreateSBFn deferredOriginalCreateSB=nullptr;
static DeferredBeginSBFn deferredOriginalBeginSB=nullptr;
static DeferredEndSBFn deferredOriginalEndSB=nullptr;
static DeferredSBApplyFn deferredOriginalSBApply=nullptr;
static void PatchVtable(void **vtable, size_t index, void *hook, void **original);
static HRESULT STDMETHODCALLTYPE DeferredSBApply(IDirect3DStateBlock9*b){WosDeferred::ReleaseMrtForNative(WosDeferred::owner);WosDeferred::NoteStateBlockApply();HRESULT hr=deferredOriginalSBApply(b);WosDeferredShadow::OnApply();WosConstCache::OnApply();return hr;}
// All runtime state blocks share one vtable; the first block seen patches Apply.
static void DeferredWatchStateBlock(IDirect3DStateBlock9*block){
    if(!block||deferredOriginalSBApply)return;
    PatchVtable(*reinterpret_cast<void***>(block),5,reinterpret_cast<void*>(DeferredSBApply),reinterpret_cast<void**>(&deferredOriginalSBApply));
}
static HRESULT STDMETHODCALLTYPE DeferredCreateSB(IDirect3DDevice9*d,D3DSTATEBLOCKTYPE type,IDirect3DStateBlock9**out){
    WosDeferred::ReleaseMrtForNative(d);
    HRESULT hr=deferredOriginalCreateSB(d,type,out);
    if(SUCCEEDED(hr)){++WosDeferred::stateBlockCreates;DeferredWatchStateBlock(out?*out:nullptr);}
    return hr;
}
static HRESULT STDMETHODCALLTYPE DeferredBeginSB(IDirect3DDevice9*d){
    WosDeferred::ReleaseMrtForNative(d);
    HRESULT hr=deferredOriginalBeginSB(d);if(SUCCEEDED(hr)){++WosDeferred::stateBlockBegins;WosDeferredShadow::OnBeginStateBlock();WosConstCache::OnBeginStateBlock();}return hr;
}
static HRESULT STDMETHODCALLTYPE DeferredEndSB(IDirect3DDevice9*d,IDirect3DStateBlock9**out){
    HRESULT hr=deferredOriginalEndSB(d,out);
    if(SUCCEEDED(hr)){++WosDeferred::stateBlockEnds;DeferredWatchStateBlock(out?*out:nullptr);}
    WosDeferredShadow::OnEndStateBlock();WosConstCache::OnEndStateBlock();
    RehookDevice(d);
    return hr;
}

static void PatchVtable(void **vtable, size_t index, void *hook, void **original)
{
    if (vtable[index] == hook) return;
    DWORD old_protect = 0;
    VirtualProtect(&vtable[index], sizeof(void *), PAGE_EXECUTE_READWRITE, &old_protect);
    *original = InterlockedExchangePointer(&vtable[index], hook);
    VirtualProtect(&vtable[index], sizeof(void *), old_protect, &old_protect);
}

// d3d9 keeps a per-device copy of the vtable on the heap and writes its own
// state setters back into it: Reset rewrites slots 49..113 (ours: 57, 65, 92,
// 94, 98, 107, 109, 113), BeginStateBlock swaps in recording versions (37 too)
// and EndStateBlock writes plain ones again; the functions differ from call to
// call (tests/reset_vtable_probe.cpp, 2026-10-11). Patch ours again wherever
// d3d9 put one of its own functions back, taking that function as the new
// original, never while a state block records. A slot owned by another
// module is left alone: it may chain to us.
static bool InD3d9(const void*p){
    static uintptr_t base=0,end=0;
    if(!base){
        const HMODULE m=GetModuleHandleW(L"d3d9.dll");if(!m)return false;
        const auto*dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(m);
        const auto*nt=reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(m)+dos->e_lfanew);
        base=reinterpret_cast<uintptr_t>(m);end=base+nt->OptionalHeader.SizeOfImage;
    }
    const uintptr_t v=reinterpret_cast<uintptr_t>(p);return v>=base&&v<end;
}
static unsigned RehookDevice(IDirect3DDevice9*device){
    if(!device||!deferredOriginalRS||WosDeferredShadow::recording)return 0;
    struct Slot{size_t index;void*hook;void**original;};
    const Slot slots[]={
        {37,reinterpret_cast<void*>(DeferredSetRT),reinterpret_cast<void**>(&deferredOriginalRT)},
        {39,reinterpret_cast<void*>(DeferredSetDS),reinterpret_cast<void**>(&deferredOriginalDS)},
        {47,reinterpret_cast<void*>(DeferredSetVP),reinterpret_cast<void**>(&deferredOriginalVP)},
        {57,reinterpret_cast<void*>(DeferredSetRS),reinterpret_cast<void**>(&deferredOriginalRS)},
        {65,reinterpret_cast<void*>(HookSetTexture),reinterpret_cast<void**>(&g_original_set_texture)},
        {92,reinterpret_cast<void*>(HookSetVertexShader),reinterpret_cast<void**>(&g_original_set_vertex_shader)},
        {94,reinterpret_cast<void*>(WosConstCache::SetVsF),reinterpret_cast<void**>(&WosConstCache::originalVsF)},
        {98,reinterpret_cast<void*>(WosConstCache::SetVsB),reinterpret_cast<void**>(&WosConstCache::originalVsB)},
        {107,reinterpret_cast<void*>(HookSetPixelShader),reinterpret_cast<void**>(&g_original_set_pixel_shader)},
        {109,reinterpret_cast<void*>(WosConstCache::SetPsF),reinterpret_cast<void**>(&WosConstCache::originalPsF)},
        {113,reinterpret_cast<void*>(WosConstCache::SetPsB),reinterpret_cast<void**>(&WosConstCache::originalPsB)}};
    void**vtable=*reinterpret_cast<void***>(device);
    bool stale=false;
    for(const auto&s:slots)if(*s.original&&vtable[s.index]!=s.hook){stale=true;break;}
    if(!stale){WosDeferredShadow::foreignSlots[0]=0;return 0;}
    unsigned patched=0;char what[64]="",foreign[64]="";
    auto note=[](char*text,size_t size,size_t index){
        const size_t used=std::strlen(text);
        if(used+5<size)std::snprintf(text+used,size-used,"%s%u",used?" ":"",(unsigned)index);};
    for(const auto&s:slots){
        if(!*s.original||vtable[s.index]==s.hook)continue;
        if(!InD3d9(vtable[s.index])){note(foreign,sizeof(foreign),s.index);continue;}
        PatchVtable(vtable,s.index,s.hook,s.original);++patched;note(what,sizeof(what),s.index);
    }
    std::memcpy(WosDeferredShadow::foreignSlots,foreign,sizeof(foreign));
    if(patched){
        WosTaa::host.setVS=g_original_set_vertex_shader;WosTaa::host.setPS=g_original_set_pixel_shader;
        WosDlss::host.setVS=g_original_set_vertex_shader;WosDlss::host.setPS=g_original_set_pixel_shader;
        WosWaterSSR::host.setPS=g_original_set_pixel_shader;
        WosDeferredShadow::rehooks+=patched;std::memcpy(WosDeferredShadow::rehookSlots,what,sizeof(what));
        // Calls made while d3d9 owned the slots were seen by neither the shadow
        // nor the constant filter.
        WosDeferredShadow::Invalidate();WosConstCache::Invalidate();
    }
    return patched;
}


static WosProbe::PresentFn deferredNativePresent=nullptr;
static HRESULT STDMETHODCALLTYPE DeferredControlPresentBridge(IDirect3DDevice9*d,const RECT*src,const RECT*dst,HWND wnd,const RGNDATA*dirty){
    WosDeferredControl::BeforePresent(d);
    return deferredNativePresent(d,src,dst,wnd,dirty);
}
static void DeferredAfterPresentBridge(IDirect3DDevice9*d,HRESULT hr){
    WosNativeRSM::AfterPresent(d,hr);
    WosDeferredControl::AfterPresent(d,hr);
}

static void HookDevice(IDirect3DDevice9 *device)
{
    void **vtable = *reinterpret_cast<void ***>(device);
    PatchVtable(vtable, 16, reinterpret_cast<void *>(HookReset), reinterpret_cast<void **>(&g_original_reset));
    WosSmaa::enabled=ReadFloatIn("SMAA","Enabled",1.0f)>0.5f;
    WosDeferred::LoadSettings();
    WosPrimaryGIProvider::Install();
    WosDeferredControl::reloadMaterials=WosDeferred::ReloadMaterials;
    WosDeferredControl::setIndirectEnabled=WosDeferredGIControl::SetEnabled;
    PatchVtable(vtable, 34, reinterpret_cast<void *>(DeferredStretch), reinterpret_cast<void **>(&deferredOriginalStretch));
    PatchVtable(vtable, 35, reinterpret_cast<void *>(DeferredFill), reinterpret_cast<void **>(&deferredOriginalFill));
    PatchVtable(vtable, 37, reinterpret_cast<void *>(DeferredSetRT), reinterpret_cast<void **>(&deferredOriginalRT));
    PatchVtable(vtable, 39, reinterpret_cast<void *>(DeferredSetDS), reinterpret_cast<void **>(&deferredOriginalDS));
    PatchVtable(vtable, 59, reinterpret_cast<void *>(DeferredCreateSB), reinterpret_cast<void **>(&deferredOriginalCreateSB));
    PatchVtable(vtable, 60, reinterpret_cast<void *>(DeferredBeginSB), reinterpret_cast<void **>(&deferredOriginalBeginSB));
    PatchVtable(vtable, 61, reinterpret_cast<void *>(DeferredEndSB), reinterpret_cast<void **>(&deferredOriginalEndSB));
    PatchVtable(vtable, 57, reinterpret_cast<void *>(DeferredSetRS), reinterpret_cast<void **>(&deferredOriginalRS));
    PatchVtable(vtable, 47, reinterpret_cast<void *>(DeferredSetVP), reinterpret_cast<void **>(&deferredOriginalVP));
    WosDeferredShadow::SetHooked(true);
    WosProbe::frameEnd = RendererFrameEnd;
    WosProbe::afterPresent = DeferredAfterPresentBridge;
    WosIndirect::nativeRsmRender=WosNativeRSM::Render;
    WosIndirect::nativeRsmReset=WosNativeRSM::Reset;
    WosNativeRSM::Initialize(device);
    PatchVtable(vtable, 17, reinterpret_cast<void *>(WosProbe::Present), reinterpret_cast<void **>(&WosProbe::originalPresent));
    if(WosProbe::originalPresent!=DeferredControlPresentBridge){deferredNativePresent=WosProbe::originalPresent;WosProbe::originalPresent=DeferredControlPresentBridge;}
    PatchVtable(vtable, 41, reinterpret_cast<void *>(HookBeginScene), reinterpret_cast<void **>(&g_original_begin_scene));
    PatchVtable(vtable, 42, reinterpret_cast<void *>(HookEndScene), reinterpret_cast<void **>(&g_original_end_scene));
    PatchVtable(vtable, 43, reinterpret_cast<void *>(HookClear), reinterpret_cast<void **>(&WosNativeClear::originalClear));
    PatchVtable(vtable, 81, reinterpret_cast<void *>(HookDrawPrimitive), reinterpret_cast<void **>(&g_original_draw_primitive));
    PatchVtable(vtable, 82, reinterpret_cast<void *>(HookDrawIndexedPrimitive), reinterpret_cast<void **>(&g_original_draw_indexed_primitive));
    PatchVtable(vtable, 83, reinterpret_cast<void *>(HookDrawPrimitiveUP), reinterpret_cast<void **>(&g_original_draw_primitive_up));
    PatchVtable(vtable, 84, reinterpret_cast<void *>(HookDrawIndexedPrimitiveUP), reinterpret_cast<void **>(&g_original_draw_indexed_primitive_up));
    PatchVtable(vtable, 107, reinterpret_cast<void *>(HookSetPixelShader), reinterpret_cast<void **>(&g_original_set_pixel_shader));
    PatchVtable(vtable, 92, reinterpret_cast<void *>(HookSetVertexShader), reinterpret_cast<void **>(&g_original_set_vertex_shader));
    PatchVtable(vtable, 65, reinterpret_cast<void *>(HookSetTexture), reinterpret_cast<void **>(&g_original_set_texture));
    WosConstCache::LoadSettings((WosProbe::Root()+"\\full_deferred\\settings.ini").c_str());
    PatchVtable(vtable, 94, reinterpret_cast<void *>(WosConstCache::SetVsF), reinterpret_cast<void **>(&WosConstCache::originalVsF));
    PatchVtable(vtable, 98, reinterpret_cast<void *>(WosConstCache::SetVsB), reinterpret_cast<void **>(&WosConstCache::originalVsB));
    PatchVtable(vtable, 109, reinterpret_cast<void *>(WosConstCache::SetPsF), reinterpret_cast<void **>(&WosConstCache::originalPsF));
    WosConstCache::psHooked=WosConstCache::originalPsF!=nullptr;
    PatchVtable(vtable, 113, reinterpret_cast<void *>(WosConstCache::SetPsB), reinterpret_cast<void **>(&WosConstCache::originalPsB));
    PatchVtable(vtable, 106, reinterpret_cast<void *>(HookCreatePixelShader), reinterpret_cast<void **>(&g_original_create_pixel_shader));
    PatchVtable(vtable, 91, reinterpret_cast<void *>(HookCreateVertexShader), reinterpret_cast<void **>(&g_original_create_vertex_shader));
    WosTaa::host.setVS=g_original_set_vertex_shader;WosTaa::host.setPS=g_original_set_pixel_shader;
    WosTaa::host.createVS=g_original_create_vertex_shader;WosTaa::host.drawUP=g_original_draw_primitive_up;
    WosTaa::enabled=ReadFloatIn("TAA","Enabled",1.0f)>0.5f;
    WosTaa::feedbackMax=ReadFloatIn("TAA","HistoryWeight",WosTaa::feedbackMax);
    WosTaa::sharpen=ReadFloatIn("TAA","Sharpen",WosTaa::sharpen);
    WosTaa::jitterScale=ReadFloatIn("TAA","Jitter",WosTaa::jitterScale);
    WosTaa::clipGamma=ReadFloatIn("TAA","ClipGamma",WosTaa::clipGamma);
    if(!WosTaa::LoadShaders(WosProbe::Root()+"\\taa")){WosTaa::faulted=true;WosTaa::status="TAA shaders missing in WoS_Deferred\\taa";}
    WosDlss::host.setVS=g_original_set_vertex_shader;WosDlss::host.setPS=g_original_set_pixel_shader;WosDlss::host.drawUP=g_original_draw_primitive_up;
    WosDlss::enabled=ReadFloatIn("DLSS","Enabled",0.0f)>0.5f;
    WosDlss::preset=int(ReadFloatIn("DLSS","Preset",float(WosDlss::preset)));
    if(WosDlss::enabled){WosTaa::enabled=true;WosSmaa::enabled=false;}
    {
        // Root() is <game>\WoS_Deferred; the helper lives in <game>\WoS_DLSS\bin.
        std::string game=WosProbe::Root();game.resize(game.find_last_of("\\/")+1);
        const std::string bin=game+"WoS_DLSS\\bin";
        wchar_t wide[MAX_PATH]={};MultiByteToWideChar(CP_ACP,0,(bin+"\\WoS_DLSS_Host.exe").c_str(),-1,wide,MAX_PATH);
        WosDlss::hostExe=wide;WosDlss::logPath=bin+"\\dlaa_game.log";
        if(!WosDlss::LoadShaders(WosProbe::Root()+"\\dlss")){WosDlss::faulted=true;WosDlss::status="DLAA shaders missing in WoS_Deferred\\dlss; TAA used";}
    }
    WosWaterSSR::host.setPS=g_original_set_pixel_shader;
    WosWaterSSR::directory=WosProbe::Root()+"\\water_ssr";
    WosWaterSSR::enabled=ReadFloatIn("WaterSSR","Enabled",1.0f)>0.5f;
    WosWaterSSR::strength=ReadFloatIn("WaterSSR","Strength",WosWaterSSR::strength);
    WosWaterSSR::rippleStrength=ReadFloatIn("WaterSSR","Ripples",WosWaterSSR::rippleStrength);
    WosWaterSSR::f0=ReadFloatIn("WaterSSR","F0",WosWaterSSR::f0);
    WosWaterSSR::roughness=ReadFloatIn("WaterSSR","Roughness",WosWaterSSR::roughness);
    WosWaterSSR::glint=ReadFloatIn("WaterSSR","Glint",WosWaterSSR::glint);
    WosWaterSSR::maxDistance=ReadFloatIn("WaterSSR","MaxDistance",WosWaterSSR::maxDistance);
    WosChimneySmoke::directory=WosProbe::Root()+"\\chimney_smoke";
    LoadChimneySmoke();
    WosStreetSteam::directory=WosProbe::Root()+"\\street_steam";
    LoadStreetSteam();
    WosStreetSteam::Install();
    // Lamps chain behind street steam in the chimney observer's extra slot.
    WosNightLamps::prevWanted = WosChimneySmoke::extraWanted;
    WosNightLamps::prevObserver = WosChimneySmoke::extraObserver;
    WosChimneySmoke::extraWanted = &WosNightLamps::Wanted;
    WosChimneySmoke::extraObserver = &WosNightLamps::Observe;
    WosNightLamps::directory = WosProbe::Root() + "\\night_lamps";
    LoadNightLamps();
    WosSoftParticles::directory=WosProbe::Root()+"\\soft_particles";
    LoadSoftParticles();
    WosNightReflection::directory=WosProbe::Root()+"\\night_reflection";
    LoadNightReflection();
    WosDayCycle::directory=WosProbe::Root()+"\\day_cycle";
    LoadDayCycle();
    WosCascadeProbe::sunToward=&WosDayCycle::SunToward;
    WosCloudLowRes::directory=WosProbe::Root()+"\\cloud_lowres";
    LoadCloudLowRes();
#ifdef WOS_RELEASE_UI
    g_menu_advanced=ReadFloatIn("Menu","Advanced",0.0f)>0.5f;
#else
    g_menu_advanced=ReadFloatIn("Menu","Advanced",1.0f)>0.5f;
#endif
    WosWater::onReplaced=[](const char*name,IDirect3DPixelShader9*shader){for(int k=0;k<3;++k)if(!std::strcmp(name,WosWaterSSR::kindNames[k]))WosWaterSSR::RegisterWater(shader,k);};
    PatchVtable(vtable, 23, reinterpret_cast<void *>(HookCreateTexture), reinterpret_cast<void **>(&g_original_create_texture));
}

// [Device] On12=1 in clouds_menu.ini builds the D3D9 interface on D3D9On12 (Windows' D3D9-over-D3D12
// mapping layer) instead of the native runtime, for an FPS comparison. The game and every hook still
// talk D3D9. device_api.json records what was actually created. Default 0 keeps the native path.
struct WosD3D9On12Args { BOOL Enable9On12; IUnknown *pD3D12Device; IUnknown *ppD3D12Queues[2]; UINT NumQueues; UINT NodeMask; };
static_assert(sizeof(WosD3D9On12Args) == 24, "D3D9ON12_ARGS layout (x86)");
using Direct3DCreate9On12Fn = IDirect3D9 *(WINAPI *)(UINT, WosD3D9On12Args *, UINT);
static const GUID kIidDirect3DDevice9On12 = {0xe7fda234, 0xb589, 0x4049, {0x94, 0x0d, 0x88, 0x78, 0x97, 0x75, 0x31, 0xc8}};
static bool g_on12_requested = false;
static bool g_on12_created = false;
// 0 not requested, 1 owner module of Direct3DCreate9 not found, 2 no Direct3DCreate9On12 export,
// 3 Direct3DCreate9On12 returned null, 4 created.
static int g_on12_step = 0;
static std::string g_d3d9_owner;

static std::string ModulePath(HMODULE module)
{
    if (!module) return std::string();
    char path[MAX_PATH] = {};
    GetModuleFileNameA(module, path, MAX_PATH);
    std::string result(path);
    for (char &c : result) if (c == '\\') c = '/';
    return result;
}

static std::string LoadedModulePath(const wchar_t *name)
{
    return ModulePath(GetModuleHandleW(name));
}

static void WriteDeviceApiStatus(IDirect3D9 *api, UINT adapter, D3DDEVTYPE type, IDirect3DDevice9 *device, HRESULT result)
{
    IUnknown *on12 = nullptr;
    const bool device_on12 = device && SUCCEEDED(device->QueryInterface(kIidDirect3DDevice9On12, reinterpret_cast<void **>(&on12))) && on12;
    if (on12) on12->Release();
    // The deferred path needs an INTZ depth texture; without it the renderer quietly stays native.
    const HRESULT intz = api ? api->CheckDeviceFormat(adapter, type, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE,
                                                      static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'))) : E_POINTER;
    FILE *f = std::fopen((WosProbe::Root() + "\\device_api.json").c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "{\"requested_on12\":%d,\"created_on12\":%d,\"on12_step\":%d,\"device_on12\":%d,\"create_device_hr\":\"0x%08lX\",\"intz_hr\":\"0x%08lX\",\"d3d9_owner\":\"%s\",\"d3d9\":\"%s\",\"d3d9on12\":\"%s\",\"d3d12\":\"%s\",\"d3d11\":\"%s\"}\n",
                 g_on12_requested ? 1 : 0, g_on12_created ? 1 : 0, g_on12_step, device_on12 ? 1 : 0,
                 static_cast<unsigned long>(result), static_cast<unsigned long>(intz), g_d3d9_owner.c_str(),
                 LoadedModulePath(L"d3d9.dll").c_str(), LoadedModulePath(L"d3d9on12.dll").c_str(),
                 LoadedModulePath(L"d3d12.dll").c_str(), LoadedModulePath(L"d3d11.dll").c_str());
    std::fclose(f);
}

static IDirect3D9 *CreateOn12(UINT sdk_version)
{
    // Take Direct3DCreate9On12 from the same d3d9 module the game imports from.
    HMODULE d3d9 = nullptr;
    g_on12_step = 1;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(g_original_d3d_create9), &d3d9))
        return nullptr;
    g_d3d9_owner = ModulePath(d3d9);
    g_on12_step = 2;
    auto create = reinterpret_cast<Direct3DCreate9On12Fn>(GetProcAddress(d3d9, "Direct3DCreate9On12"));
    if (!create) return nullptr;
    static WosD3D9On12Args args = {};
    args.Enable9On12 = TRUE;
    g_on12_step = 3;
    IDirect3D9 *d3d = create(sdk_version, &args, 1);
    if (d3d) g_on12_step = 4;
    return d3d;
}

static HRESULT STDMETHODCALLTYPE HookCreateDevice(IDirect3D9 *self, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
                                                   D3DPRESENT_PARAMETERS *params, IDirect3DDevice9 **device)
{
    const HRESULT result = g_original_create_device(self, adapter, type, window, flags, params, device);
    WriteDeviceApiStatus(self, adapter, type, SUCCEEDED(result) && device ? *device : nullptr, result);
    if (SUCCEEDED(result) && device && *device) HookDevice(*device);
    return result;
}

static IDirect3D9 *WINAPI HookDirect3DCreate9(UINT sdk_version)
{
    IDirect3D9 *d3d = nullptr;
    g_on12_requested = ReadFloatIn("Device", "On12", 0.0f) > 0.5f;
    if (g_on12_requested) {
        d3d = CreateOn12(sdk_version);
        g_on12_created = d3d != nullptr;
    }
    if (!d3d) d3d = g_original_d3d_create9(sdk_version);
    if (d3d) {
        void **vtable = *reinterpret_cast<void ***>(d3d);
        PatchVtable(vtable, 16, reinterpret_cast<void *>(HookCreateDevice), reinterpret_cast<void **>(&g_original_create_device));
    }
    return d3d;
}

static bool PatchImport(const char *dll_name, const char *function_name, void *hook, void **original)
{
    HMODULE executable = GetModuleHandleW(nullptr);
    if (!executable) return false;
    auto *base = reinterpret_cast<unsigned char *>(executable);
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    const auto &directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return false;
    auto *imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + directory.VirtualAddress);
    for (; imports->Name; ++imports) {
        const char *dll = reinterpret_cast<const char *>(base + imports->Name);
        if (_stricmp(dll, dll_name) != 0) continue;
        HMODULE module = GetModuleHandleA(dll_name);
        void *export_address = module ? reinterpret_cast<void *>(GetProcAddress(module, function_name)) : nullptr;
        auto *names = imports->OriginalFirstThunk
            ? reinterpret_cast<IMAGE_THUNK_DATA *>(base + imports->OriginalFirstThunk)
            : nullptr;
        auto *functions = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imports->FirstThunk);
        for (size_t index = 0; functions[index].u1.Function; ++index) {
            bool matches = export_address && reinterpret_cast<void *>(functions[index].u1.Function) == export_address;
            if (names && names[index].u1.AddressOfData && !IMAGE_SNAP_BY_ORDINAL(names[index].u1.Ordinal)) {
                auto *by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names[index].u1.AddressOfData);
                matches = std::strcmp(reinterpret_cast<const char *>(by_name->Name), function_name) == 0;
            }
            if (!matches) continue;
            DWORD old_protect = 0;
            VirtualProtect(&functions[index].u1.Function, sizeof(void *), PAGE_READWRITE, &old_protect);
            *original = reinterpret_cast<void *>(functions[index].u1.Function);
            functions[index].u1.Function = reinterpret_cast<ULONG_PTR>(hook);
            VirtualProtect(&functions[index].u1.Function, sizeof(void *), old_protect, &old_protect);
            return true;
        }
    }
    return false;
}

static void FeedMouseDelta(LONG dx, LONG dy, LONG wheel)
{
    g_di_mouse_seen = true;
    const ImVec2 display = ImGui::GetCurrentContext() ? ImGui::GetIO().DisplaySize : ImVec2(1920.0f, 1080.0f);
    if (!g_cursor_placed) {
        g_cursor_x = display.x * 0.5f;
        g_cursor_y = display.y * 0.5f;
        g_cursor_placed = true;
    }
    g_cursor_x = (std::min)((std::max)(g_cursor_x + static_cast<float>(dx), 0.0f), display.x - 1.0f);
    g_cursor_y = (std::min)((std::max)(g_cursor_y + static_cast<float>(dy), 0.0f), display.y - 1.0f);
    if (wheel != 0) g_di_wheel += static_cast<float>(wheel) / static_cast<float>(WHEEL_DELTA);
}

using DIGetDeviceStateFn = HRESULT(STDMETHODCALLTYPE *)(IDirectInputDevice8A *, DWORD, LPVOID);
using DIGetDeviceDataFn = HRESULT(STDMETHODCALLTYPE *)(IDirectInputDevice8A *, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
using DICreateDeviceFn = HRESULT(STDMETHODCALLTYPE *)(IDirectInput8A *, REFGUID, LPDIRECTINPUTDEVICE8A *, LPUNKNOWN);
using DirectInput8CreateFn = HRESULT(WINAPI *)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);

static DIGetDeviceStateFn g_original_di_get_state = nullptr;
static DIGetDeviceDataFn g_original_di_get_data = nullptr;
static DICreateDeviceFn g_original_di_create_device = nullptr;
static DirectInput8CreateFn g_original_di8_create = nullptr;

static int DeviceKind(void *device)
{
    const auto entry = g_di_devices.find(device);
    return entry == g_di_devices.end() ? 0 : entry->second;
}

static HRESULT STDMETHODCALLTYPE HookDIGetDeviceState(IDirectInputDevice8A *self, DWORD size, LPVOID data)
{
    const HRESULT result = g_original_di_get_state(self, size, data);
    if (!g_menu_visible || FAILED(result) || !data) return result;
    const int kind = DeviceKind(self);
    if (kind == 0) return result;
    if (kind == 1 && size >= sizeof(DIMOUSESTATE)) {
        auto *state = static_cast<DIMOUSESTATE *>(data);
        FeedMouseDelta(state->lX, state->lY, state->lZ);
        for (int button = 0; button < 3; ++button)
            g_di_buttons[button] = (state->rgbButtons[button] & 0x80) != 0;
    }
    // Swallow the input so Spider-Man neither turns nor swings while the menu is up.
    std::memset(data, 0, size);
    return result;
}

static HRESULT STDMETHODCALLTYPE HookDIGetDeviceData(IDirectInputDevice8A *self, DWORD object_size,
                                                     LPDIDEVICEOBJECTDATA objects, LPDWORD count, DWORD flags)
{
    const HRESULT result = g_original_di_get_data(self, object_size, objects, count, flags);
    if (!g_menu_visible || FAILED(result) || !count) return result;
    const int kind = DeviceKind(self);
    if (kind == 0) return result;
    if (kind == 1 && objects) {
        for (DWORD index = 0; index < *count; ++index) {
            const DIDEVICEOBJECTDATA &item = objects[index];
            const LONG value = static_cast<LONG>(item.dwData);
            switch (item.dwOfs) {
            case DIMOFS_X: FeedMouseDelta(value, 0, 0); break;
            case DIMOFS_Y: FeedMouseDelta(0, value, 0); break;
            case DIMOFS_Z: FeedMouseDelta(0, 0, value); break;
            case DIMOFS_BUTTON0: g_di_buttons[0] = (item.dwData & 0x80) != 0; break;
            case DIMOFS_BUTTON1: g_di_buttons[1] = (item.dwData & 0x80) != 0; break;
            case DIMOFS_BUTTON2: g_di_buttons[2] = (item.dwData & 0x80) != 0; break;
            default: break;
            }
        }
    }
    if ((flags & DIGDD_PEEK) == 0) *count = 0;
    return result;
}

static void HookInputDevice(void *device, int kind)
{
    if (!device) return;
    g_di_devices[device] = kind;
    void **vtable = *reinterpret_cast<void ***>(device);
    PatchVtable(vtable, 9, reinterpret_cast<void *>(HookDIGetDeviceState), reinterpret_cast<void **>(&g_original_di_get_state));
    PatchVtable(vtable, 10, reinterpret_cast<void *>(HookDIGetDeviceData), reinterpret_cast<void **>(&g_original_di_get_data));
}

static HRESULT STDMETHODCALLTYPE HookDICreateDevice(IDirectInput8A *self, REFGUID guid,
                                                    LPDIRECTINPUTDEVICE8A *device, LPUNKNOWN outer)
{
    const HRESULT result = g_original_di_create_device(self, guid, device, outer);
    if (SUCCEEDED(result) && device && *device) {
        if (IsEqualGUID(guid, GUID_SysMouse)) HookInputDevice(*device, 1);
        else if (IsEqualGUID(guid, GUID_SysKeyboard)) HookInputDevice(*device, 2);
    }
    return result;
}

static HRESULT WINAPI HookDirectInput8Create(HINSTANCE instance, DWORD version, REFIID iid, LPVOID *out, LPUNKNOWN outer)
{
    const HRESULT result = g_original_di8_create(instance, version, iid, out, outer);
    if (SUCCEEDED(result) && out && *out) {
        void **vtable = *reinterpret_cast<void ***>(*out);
        PatchVtable(vtable, 3, reinterpret_cast<void *>(HookDICreateDevice), reinterpret_cast<void **>(&g_original_di_create_device));
    }
    return result;
}

static void PatchGameImports()
{
    PatchImport("d3d9.dll", "Direct3DCreate9", reinterpret_cast<void *>(HookDirect3DCreate9),
                reinterpret_cast<void **>(&g_original_d3d_create9));
    PatchImport("dinput8.dll", "DirectInput8Create", reinterpret_cast<void *>(HookDirectInput8Create),
                reinterpret_cast<void **>(&g_original_di8_create));
}

static void EnsureRealWinmm()
{
    if (g_real_winmm) return;
    wchar_t system_path[MAX_PATH] = {};
    GetSystemDirectoryW(system_path, MAX_PATH);
    std::wstring path(system_path);
    path += L"\\winmm.dll";
    g_real_winmm = LoadLibraryW(path.c_str());
}

// Every other WinMM export is a thunk (build/winmm_forward.h) that jumps to the
// system DLL's function, resolved on first call and cached.
extern "C" UINT __cdecl WosForwardMissing() { return 0; }
static void *WosForwardResolve(unsigned index, const char *const *names, void **table)
{
    EnsureRealWinmm();
    void *target = g_real_winmm ? reinterpret_cast<void *>(GetProcAddress(g_real_winmm, names[index])) : nullptr;
    if (!target) target = reinterpret_cast<void *>(&WosForwardMissing);
    table[index] = target;
    return target;
}
#include "../build/winmm_forward.h"

extern "C" MMRESULT WINAPI proxy_timeSetEvent(UINT delay, UINT resolution, LPTIMECALLBACK callback, DWORD_PTR user, UINT event)
{
    EnsureRealWinmm();
    using Fn = MMRESULT(WINAPI *)(UINT, UINT, LPTIMECALLBACK, DWORD_PTR, UINT);
    static Fn function = g_real_winmm ? reinterpret_cast<Fn>(GetProcAddress(g_real_winmm, "timeSetEvent")) : nullptr;
    return function ? function(delay, resolution, callback, user, event) : 0;
}

extern "C" MMRESULT WINAPI proxy_timeKillEvent(UINT timer)
{
    EnsureRealWinmm();
    using Fn = MMRESULT(WINAPI *)(UINT);
    static Fn function = g_real_winmm ? reinterpret_cast<Fn>(GetProcAddress(g_real_winmm, "timeKillEvent")) : nullptr;
    return function ? function(timer) : TIMERR_NOCANDO;
}

// Placeholder so the linker can bind the mmsystemGetVersion forwarder in the .def file;
// winmm.lib does not ship an import stub for this legacy export.
extern "C" UINT WINAPI mmsystemGetVersion(void)
{
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        WosStreaming::Install();
        PatchGameImports();
    }
    return TRUE;
}
