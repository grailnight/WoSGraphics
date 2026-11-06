// Volumetric sun shafts, marched at half resolution against the same four
// cascade tiles the deferred resolve already samples for PCSS.
//
// The pass owns no geometry and no shadow map of its own: the atlas, the
// camera-relative cascade rows and the depth bias arrive in the registers
// WosCascadeShadow::Bind already fills for the resolve, so a shaft ends
// exactly where the shadow it belongs to ends. Points outside every cascade
// are treated as lit, which turns the far field into even haze rather than a
// hard edge at the last cascade.
//
// Alpha carries the view distance of the pixel the ray stopped at. The
// composite needs it to weight its four half-resolution taps, and computing
// it here is free: the march already reconstructed the position.
sampler2D SceneDepth : register(s4);
sampler2D CascadeAtlas : register(s7);
float4 InverseVP[4] : register(c0);
// Written by WosCascadeShadow::Bind; identical meaning to the resolve.
float4 CascadeRow[12] : register(c11);
float4 CascadeTexel : register(c23);
float4 CascadeRange : register(c24);
float4 CascadeUv : register(c25);
float4 SunShadow : register(c26);
float4 ShadowBias : register(c27);
// direction toward the sun (unit, world axes), step count
float4 Sun : register(c28);
// scattering colour, extinction per world unit
float4 Scatter : register(c29);
// longest march, phase anisotropy, half-resolution pixel size xy
float4 March : register(c30);
// Additional homogeneous atmosphere. Attenuate each scattering sample by
// its own distance to the eye, not by the depth of the background surface.
float4 Fog : register(c32); // extinction, start distance, maximum distance
#include "deferred_height_fog.hlsl"
// Local point lights (deferred_light_shafts.hlsl): c36..c39, c40..c88, s9/s10.
#include "deferred_light_shafts.hlsl"

float Hash(float2 uv)
{
    float2 pixel = uv / March.zw;
    return frac(52.9829189 * frac(0.06711056 * pixel.x + 0.00583715 * pixel.y));
}
// One tap per step. A point in air has no normal, so the normal offset the
// receiver path relies on does not exist here and only the depth bias remains;
// the jittered step start scatters the residual banding instead.
#define VOLUME_CASCADE(index) \
    if (found < .5) \
    { \
        float3 c = float3(dot(CascadeRow[index * 3 + 0], p4), dot(CascadeRow[index * 3 + 1], p4), \
                          dot(CascadeRow[index * 3 + 2], p4)); \
        if (max(abs(c.x), abs(c.y)) < 1 && c.z > 0 && c.z < 1) \
        { \
            found = 1; \
            local = c; \
            tileMin = float2((index & 1) * .5, (index >> 1) * .5); \
            range = CascadeRange[index]; \
        } \
    }
float SunVisible(float3 position)
{
    float4 p4 = float4(position, 1);
    float found = 0, range = 1;
    float3 local = 0;
    float2 tileMin = 0;
    VOLUME_CASCADE(0)
    VOLUME_CASCADE(1)
    VOLUME_CASCADE(2)
    VOLUME_CASCADE(3)
    if (found < .5) return 1;
    float2 uv = tileMin + float2(local.x * .5 + .5, .5 - local.y * .5) * .5;
    uv = clamp(uv, tileMin + ShadowBias.z, tileMin + .5 - ShadowBias.z);
    float bias = ShadowBias.x / max(range, 1e-6);
    float sampled = tex2Dlod(CascadeAtlas, float4(uv, 0, 0)).r;
    return 1 - step(sampled, local.z - bias);
}
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float depth = tex2Dlod(SceneDepth, float4(uv, 0, 0)).r;
    float4 p = float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1);
    float4 world = float4(dot(InverseVP[0], p), dot(InverseVP[1], p), dot(InverseVP[2], p), dot(InverseVP[3], p));
    // The sky sits at the far plane, which this projection puts at infinity
    // (w = 0). Returning nothing there cut the lamp glow along every skyline,
    // so the sky takes the ray through a mid-depth point and a far distance.
    // Only the lamps use it: the sun march over the sky stays as it was (none).
    bool sky = abs(world.w) < 1e-8;
    [branch] if (sky)
    {
        p.z = .5;
        world = float4(dot(InverseVP[0], p), dot(InverseVP[1], p), dot(InverseVP[2], p), dot(InverseVP[3], p));
        if (abs(world.w) < 1e-8) return 0;
        world.xyz = normalize(world.xyz / world.w) * 1e5;
        world.w = 1;
    }
    float3 position = world.xyz / world.w;
    float distance = length(position);
    if (!(distance > 1e-4)) return 0;
    float3 ray = position / distance;
    float jitter = Hash(uv);
    float3 light = 0;
    // Zero steps means no sun this frame: the cascade registers are unbound.
    [branch] if (Sun.w >= 1 && !sky)
    {
        // The sky reconstructs to the far plane; the cap is what keeps its ray
        // the same length as a rooftop's, so the horizon does not saturate.
        float far = min(distance, March.x);
        float steps = Sun.w;
        float stepLength = far / steps;
        float g = March.y;
        float gg = g * g;
        float cosTheta = dot(ray, Sun.xyz);
        float phase = (1 - gg) / (12.566370614 * pow(max(1 + gg - 2 * g * cosTheta, 1e-4), 1.5));
        // Uniform steps, so the per-step transmittance factor is a constant.
        float stepTransmittance = exp(-Scatter.w * stepLength);
        float transmittance = 1;
        float lit = 0;
        [loop] for (int i = 0; i < 64; ++i)
        {
            if (i >= steps) break;
            float sampleDistance = (i + jitter) * stepLength;
            float fogPath = FogOpticalPath(sampleDistance, ray.y);
            float fogTransmission = exp(-max(Fog.x, 0) * fogPath);
            lit += SunVisible(ray * sampleDistance) * transmittance * fogTransmission;
            transmittance *= stepTransmittance;
        }
        light = Scatter.rgb * (lit * stepLength * Scatter.w * phase);
    }
    // Lamps are not capped by the sun's march length: a lamp in front of the
    // sky still glows over it.
    light += LampShafts(ray, distance, jitter);
    return float4(light, distance);
}
