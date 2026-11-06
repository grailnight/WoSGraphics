// First in-game integration: specular-workflow PBR with estimated parameters.
// Native coefficients are NOT measured F0/albedo. Where the diffuse texture's
// name gives a material class, that class sets the roughness window and the
// metalness; elsewhere both are estimated from the specular mask as before.
// Existing textures still control the local amount/tint and width of reflection.
#ifndef WOS_PBR_LEGACY_BRIDGE
#define WOS_PBR_LEGACY_BRIDGE
#include "deferred_brdf.hlsl"

// v3: the estimate's own numbers become runtime state instead of literals, so
// they can be judged in the game rather than through a shader rebuild. Nothing
// about the estimate changed; only who owns its constants.
// x roughness floor, y roughness ceiling, z unresolved surface variance,
// w dielectric F0 at normal incidence.
float4 PbrSurface : register(c43);
// x metalness strength (0 disables the term exactly), y specular-mask threshold,
// z mask width above the threshold, w metal tint pull toward legacy albedo.
float4 PbrMetal : register(c44);

// Geometric specular antialiasing (Tokuyoshi and Kaplanyan 2019, "Improved
// Geometric Specular Antialiasing"). How far the normal turns between
// neighbouring pixels is surface detail one pixel cannot resolve. It is added
// as microfacet variance, so a highlight on a ripple or a bevel widens into its
// pixel-average instead of flickering between frames.
// x strength (0 disables the term exactly), y clamp on the added alpha^2,
// z relative depth step that marks a silhouette, w reserved.
float4 SpecularAA : register(c29);
static float WoSSpecularKernel = 0;

// Material classes. The DLL names each draw's surface from its diffuse
// texture (texture_classes.bin: metal, brick, asphalt ...) and stores the class
// in texel 31.y of the draw's light set. x strength (0 disables the term
// exactly), y how far rain lowers a class's roughness window, zw reserved.
float4 PbrClassControl : register(c53);
// Per class: x roughness floor, y roughness ceiling, z metalness,
// w minimum reflectance coverage. Row 0 (unknown) and row 12 (facade, which
// the resolve turns into glass or the estimate per pixel) are never read.
float4 PbrClassTable[14] : register(c56);
// Symbiote goo (venomhive, sympod, symgrowth). Its native shader adds the
// specular map times the sun and the environment cube straight on top of a
// dark diffuse: a specular workflow, where the map's own violet colour is the
// reflectance. Read as a dielectric, F0 fell to .04 of that and the goo
// showed only its dark bark lit by the warm sun. Most city goo is drawn by the
// metallic goo shader instead (metallicTint squared as its specular), which
// the DLL puts in this class by the shader itself.
static const float WoSSymbioteClass = 13;
// The resolve sits at the SM3 limit of 32 temporaries: even a second scalar
// kept for the whole pixel does not fit beside the PCSS loop. So one float
// carries both: the class number in its integer part and the wetness that
// narrows the class's roughness window in its fraction (0 to .4). Each
// lighting call selects the row again.
static float WoSClassKey = 0;

// Call once per pixel before any lighting; class 0 keeps the estimate below.
// Water on a surface is smoother than the surface, so wetness lowers the window.
void WoSPbrSetClass(float id, float wetness)
{
    float wet = .4 * saturate(PbrClassControl.y) * saturate(wetness);
    WoSClassKey = PbrClassControl.x > 0 ? floor(id + .5) + wet : 0;
}

// Facade draws (class 12) share one atlas across many materials and name no
// single class. Their window pixels are recognisable in the G-buffer itself:
// every facade family multiplies the native specular by (1 - window weight)
// or by the property mask, so a window carries exactly zero specular, and its
// rho is fade * (a, b, a) from a window colour constant, so red equals blue
// while green differs. Walls keep the estimate; windows become glass.
float WoSFacadeClass(float3 rho, float3 coefficient)
{
    bool window = max(coefficient.r, max(coefficient.g, coefficient.b)) == 0 &&
                  rho.r == rho.b && rho.g != rho.r;
    return window ? 5 : 0;
}

float4 WoSClassRow()
{
    float id = floor(WoSClassKey);
    float4 row = 0;
    [unroll] for (int i = 1; i < 14; ++i)
        row = abs(id - i) < .5 ? PbrClassTable[i] : row;
    row.xy *= 1 - 2.5 * (WoSClassKey - id);
    return row;
}
float WoSClassWeight() { return WoSClassKey > .5 ? saturate(PbrClassControl.x) : 0; }
// How far F0 moves from the dielectric estimate to the specular colour itself.
float WoSClassSpecularWorkflow()
{
    return abs(floor(WoSClassKey) - WoSSymbioteClass) < .5 ? WoSClassWeight() : 0;
}

// A class can hold a weak specular mask up to a plausible reflectance: metal
// and glass with a dim mask would otherwise reflect like plaster. Symbiote
// reflectance is a colour, so there the floor lifts the strongest channel and
// keeps the hue: the metallic goo writes its tint squared (blue about .05),
// which a grey floor would bleach.
float3 WoSPbrCoverage(float3 coverage)
{
    float w = WoSClassWeight();
    float lift = w * saturate(WoSClassRow().w);
    float peak = max(coverage.r, max(coverage.g, coverage.b));
    float3 tinted = coverage * (lift / max(peak, 1e-4));
    float3 lifted = WoSClassSpecularWorkflow() > 0 && peak > 1e-4 ? tinted : lift.xxx;
    return w > 0 ? max(coverage, lifted) : coverage;
}

// Call it from uniform control flow, before any lighting: ddx/ddy of a value
// read inside a dynamic branch are undefined on SM3.
void WoSSpecularAASetup(float3 n, float3 position)
{
    float3 dx = ddx(n);
    float3 dy = ddy(n);
    float distance = length(position);
    // A depth step means the quad straddles two surfaces. That normal change is
    // a silhouette, not unresolved detail, and must not blur either side of it.
    float jump = SpecularAA.z * distance;
    // Selections, not products: a helper pixel over the sky can carry an
    // infinite position, and zero times that would be NaN.
    float gx = abs(ddx(distance)) < jump ? dot(dx, dx) : 0;
    float gy = abs(ddy(distance)) < jump ? dot(dy, dy) : 0;
    // Screen-space filter variance 1/(2 pi) is the paper's pixel footprint.
    float kernel = min(2 * 0.15915494 * SpecularAA.x * (gx + gy), SpecularAA.y);
    WoSSpecularKernel = SpecularAA.x > 0 && kernel > 0 ? kernel : 0;
}

// Unset pixel shader constants read as zero. A zero ceiling therefore means the
// runtime that drew this frame predates the tuning, and the v2 literals are
// restored so that such a frame is bit-for-bit what it used to be.
float4 WoSPbrSurfaceParams()
{
    return PbrSurface.y > 0 ? PbrSurface : float4(.40, .95, .12, .04);
}

float WoSPbrRoughness(float power)
{
    float4 s = WoSPbrSurfaceParams();
    float low = clamp(s.x, .02, 1);
    float high = clamp(s.y, low, 1);
    float roughness = clamp(sqrt(WoS_GGXAlphaFromLegacyPhong(power) + max(s.z, 0)), low, high);
    float w = WoSClassWeight();
    if (w > 0)
    {
        // The texture's own glossiness (sqrt alpha spans about .14 to .84 over
        // native powers 2048..1) is spread across the class window rather
        // than clamped to it, so wear and masks still vary inside a material.
        float t = saturate((sqrt(WoS_GGXAlphaFromLegacyPhong(power)) - .14) / .70);
        float4 row = WoSClassRow();
        float classLow = clamp(row.x, .02, 1);
        float classRoughness = lerp(classLow, clamp(row.y, classLow, 1), t);
        roughness = lerp(roughness, classRoughness, w);
    }
    // Widened after the clamp, so the ceiling cannot take the filter back. The
    // kernel adds to alpha^2 = roughness^4; a zero kernel returns the value
    // above untouched rather than a round trip through two square roots.
    float filtered = sqrt(sqrt(saturate(roughness * roughness * roughness * roughness + WoSSpecularKernel)));
    return WoSSpecularKernel > 0 ? filtered : roughness;
}

// Legacy assets carry no metalness channel. Metals do carry a strong, often
// tinted specular mask where dielectrics carry a weak grey one, so the mask's
// own strength is the only estimator available until authored material IDs
// exist. This is a guess about a texture, not a measurement of a material.
// At strength zero it returns exactly zero and nothing downstream changes.
float WoSPbrMaskMetallic(float3 coefficient)
{
    float strength = saturate(PbrMetal.x);
    if (strength <= 0) return 0;
    float3 c = max(coefficient, 0);
    float mask = max(c.r, max(c.g, c.b));
    float threshold = max(PbrMetal.y, 0);
    return strength * smoothstep(threshold, threshold + max(PbrMetal.z, 1e-4), mask);
}
// A named class replaces the guess: brick with a bright mask is not metal,
// and a dull steel texture is.
float WoSPbrMetallic(float3 coefficient)
{
    float guess = WoSPbrMaskMetallic(coefficient);
    float w = WoSClassWeight();
    return w > 0 ? lerp(guess, saturate(WoSClassRow().z), w) : guess;
}

float3 WoSPbrLegacyDirect(float3 n, float3 v, float3 l, float3 radiance,
                         float3 rho, float3 coefficient, float power, float specularGain)
{
    float nv = saturate(dot(n,v));
    float nl = saturate(dot(n,l));
    float3 h = v + l;
    float hh = dot(h,h);
    // No radiance gives a zero lobe, so it leaves by the same exit: a point
    // light out of range, or a sun the shadow maps have taken entirely. A
    // separate branch around the call does not fit in the resolve's temps.
    if (nv <= 0 || nl <= 0 || hh <= 1e-12 || max(radiance.r, max(radiance.g, radiance.b)) <= 0) return 0;
    h *= rsqrt(hh);
    // v2: add unresolved surface variance instead of allowing polished-city lobes.
    // Preserve texture-dependent variation; the floor is now a setting.
    float roughness = WoSPbrRoughness(power);
    float alpha = roughness * roughness;
    // The previous gain adjusted highlight height. Here it adjusts estimated
    // interface reflectance before both diffuse and specular are evaluated.
    // Coverage weights the entire interface, including its grazing response.
    // The old factor 50 restored nearly white grazing reflection on weak masks.
    float3 coverage = WoSPbrCoverage(saturate(max(coefficient,0) * max(specularGain,0)));
    float3 albedo = saturate(rho);
    float metal = WoSPbrMetallic(coefficient);
    // A metal's F0 is its own reflectance colour, and legacy albedo is the only
    // colour this frame holds for it. The pull keeps that borrowed tint under
    // control: 0 spends it entirely on a neutral grey of the same luminance.
    float3 metalF0 = coverage * lerp(dot(albedo, float3(.2126,.7152,.0722)).xxx,
                                     albedo, saturate(PbrMetal.w));
    // Bound the dielectric F0; metals take the estimate above instead.
    float3 f0 = lerp(clamp(WoSPbrSurfaceParams().w, 0, .16) * coverage, metalF0, metal);
    f0 = lerp(f0, coverage, WoSClassSpecularWorkflow());
    float3 f90 = coverage;
    float vh = saturate(dot(v,h));
    float x = 1-vh; float x2=x*x;
    float3 f = f0 + (f90-f0)*(x2*x2*x);
    float xv=1-nv, xl=1-nl;
    float3 fv=f0+(f90-f0)*(xv*xv*xv*xv*xv);
    float3 fl=f0+(f90-f0)*(xl*xl*xl*xl*xl);
    // A conductor transmits nothing, so its diffuse lobe goes with its metalness.
    float3 diffuse=(1-fv)*(1-fl)*(1-metal)*albedo/WOS_BRDF_PI;
    float3 specular=Filament_D_GGX(alpha,saturate(dot(n,h))) *
                    Filament_V_SmithGGXCorrelated(alpha,nv,nl) * f;
    // PI bridge preserves old Lambert's scale when specular is zero. It does
    // not establish physical photometric units for the game's light sources.
    return max(radiance,0) * (WOS_BRDF_PI * nl) * (diffuse+specular);
}
#endif
