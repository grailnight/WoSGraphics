// Depth-aware upsample of the half-resolution shafts, blended additively into
// the scene HDR target.
//
// A plain bilinear upsample bleeds a shaft half a low-resolution texel past
// every silhouette, and a city skyline is nothing but silhouettes. The march
// stored the view distance of each of its own pixels in alpha, so each of the
// four taps can be weighted by how far its ray ended from this pixel's ray:
// a tap that stopped on the building behind the edge loses against the taps
// that stopped on the roof in front of it. No second depth pass is needed
// because the marched distance is exactly the depth the tap actually used.
sampler2D Volumetric : register(s0);
sampler2D SceneDepth : register(s4);
float4 InverseVP[4] : register(c0);
// half-resolution width and height, strength, distance rejection per world unit
float4 Composite : register(c31);
// Analytic homogeneous atmosphere, evaluated at full-resolution scene depth.
float4 Fog : register(c32); // extinction, start distance, maximum distance
#include "deferred_height_fog.hlsl"
float4 FogAmbient : register(c33); // linear HDR ambient radiance

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float depth = tex2D(SceneDepth, uv).r;
    float4 p = float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1);
    float4 world = float4(dot(InverseVP[0], p), dot(InverseVP[1], p), dot(InverseVP[2], p), dot(InverseVP[3], p));
    // Sky pixels are left to the bloom shader, which adds the lamp glow after
    // the clouds (otherwise the cloud layer multiplies the glow away).
    [branch] if (abs(world.w) < 1e-8) return float4(0, 0, 0, 1);
    float centre = length(world.xyz / world.w);
    float2 size = Composite.xy;
    float2 base = uv * size - .5;
    float2 f = frac(base);
    float2 origin = (floor(base) + .5) / size;
    float2 texel = 1 / size;
    float3 colour = 0;
    float weight = 0;
    // ps_3_0 has no bitwise operators, so the four corners are written out.
    float2 corners[4] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 1) };
    [unroll] for (int i = 0; i < 4; ++i)
    {
        float2 offset = corners[i];
        float4 tap = tex2Dlod(Volumetric, float4(origin + offset * texel, 0, 0));
        float bilinear = lerp(1 - f.x, f.x, offset.x) * lerp(1 - f.y, f.y, offset.y);
        float w = bilinear / (1 + abs(tap.a - centre) * Composite.w);
        colour += tap.rgb * w;
        weight += w;
    }
    // Alpha is written but masked off by the caller: the scene alpha is the
    // native one and nothing here is entitled to change it.
    float3 shafts = colour / max(weight, 1e-6) * Composite.z;
    float rayY = abs(world.w) < 1e-8 ? 0 : (world.y / world.w) / max(centre, 1e-6);
    float path = FogOpticalPath(centre, rayY);
    float transmission = exp(-max(Fog.x, 0) * path);
    // Premultiplied radiance + scene * transmission. Alpha is a blend factor;
    // scene alpha stays masked off. Zero density retains additive shafts.
    // Shafts already include sample-to-eye atmospheric attenuation in the
    // march. Only the background scene gets full-depth transmission here.
    return float4(shafts + FogAmbient.rgb * (1 - transmission), transmission);
}
