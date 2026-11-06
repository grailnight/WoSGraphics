// Modified HLSL port of three pure-math Google Filament functions.
// Upstream commit: f840f1e6aa65ec915f6bc8a2c67869f5ad631e35
// Upstream: shaders/src/surface_brdf.fs (desktop D_GGX, correlated Smith, Schlick).
// License: Apache-2.0; see third_party/filament/LICENSE and provenance.json.
// Changes: GLSL vec3->float3, names prefixed, unused D_GGX h removed,
// PREVENT_DIV0 replaced with max denominator, pow5 expanded to multiplies.
// WoS_* adapters below are local compatibility policy, not upstream Filament.
#ifndef WOS_FILAMENT_BRDF_HLSL
#define WOS_FILAMENT_BRDF_HLSL
static const float WOS_BRDF_PI = 3.14159265358979323846;

// roughness is microfacet alpha, NOT perceptualRoughness (which is sqrt(alpha)).
// Preconditions: roughness > 0; NoH, NoV, NoL and VoH in [0,1].
float Filament_D_GGX(float roughness, float NoH)
{
    // Walter et al. 2007, "Microfacet Models for Refraction through Rough Surfaces"
    float oneMinusNoHSquared = 1.0 - NoH * NoH;
    float a = NoH * roughness;
    float k = min(roughness / (oneMinusNoHSquared + a * a), 453.5);
    return k * (k * (1.0 / WOS_BRDF_PI));
}
float Filament_V_SmithGGXCorrelated(float roughness, float NoV, float NoL)
{
    // Heitz 2014, "Understanding the Masking-Shadowing Function in Microfacet-Based BRDFs"
    float a2 = roughness * roughness;
    float lambdaV = NoL * sqrt((NoV - a2 * NoV) * NoV + a2);
    float lambdaL = NoV * sqrt((NoL - a2 * NoL) * NoL + a2);
    return 0.5 / max(lambdaV + lambdaL, 0.0000077);
}
float3 Filament_F_Schlick(float3 f0, float f90, float VoH)
{
    // Schlick 1994, "An Inexpensive BRDF Model for Physically-Based Rendering"
    float x = 1.0 - VoH;
    float x2 = x * x;
    float x5 = x2 * x2 * x;
    return f0 + (f90 - f0) * x5;
}

// Native Phong exponents describe reflection-vector lobes. A half-vector lobe
// uses approximately four times the power. This width mapping to GGX is a
// heuristic because GGX has different tails. Perceptual roughness=sqrt(alpha).
float WoS_GGXAlphaFromLegacyPhong(float nativePower)
{
    float halfVectorPower = 4.0 * clamp(nativePower, 1.0, 2048.0);
    return max(sqrt(2.0 / (halfVectorPower + 2.0)), 0.02);
}

// Compatibility adapter preserving the native normal-incidence highlight
// coefficient and exact zero-spec masking. The existing material coefficient
// is not labelled F0. A generic 4% dielectric Fresnel SHAPE is used instead;
// native RGB remains its amplitude/tint. No metalness is inferred.
// Return is the specular lobe only: caller multiplies native lightRGB*NoL,
// shadow visibility, user specular intensity and native fog transmittance.
// This is peak-normalized legacy lighting, NOT physically calibrated PBR.
float3 WoS_GGXSpecularLobe(float3 n, float3 v, float3 l,
                        float3 nativeSpecular, float nativePower)
{
    float NoL = saturate(dot(n, l));
    float NoV = saturate(dot(n, v));
    float3 h = v + l;
    float hh = dot(h, h);
    if (NoL <= 0 || NoV <= 0 || hh <= 1e-12) return 0;
    h *= rsqrt(hh);
    float NoH = saturate(dot(n, h));
    float VoH = saturate(dot(v, h));
    float alpha = WoS_GGXAlphaFromLegacyPhong(nativePower);
    float d = Filament_D_GGX(alpha, NoH);
    float visibility = Filament_V_SmithGGXCorrelated(alpha, NoV, NoL);
    float fresnel = Filament_F_Schlick(float3(0.04, 0.04, 0.04), 1.0, VoH).x;
    // At n=v=l: D=1/(PI*alpha^2), V=1/4, F=0.04 -> shape=1.
    float peakNormalization = 4.0 * WOS_BRDF_PI * alpha * alpha / 0.04;
    return max(nativeSpecular, 0) * (d * visibility * fresnel * peakNormalization);
}
#endif
