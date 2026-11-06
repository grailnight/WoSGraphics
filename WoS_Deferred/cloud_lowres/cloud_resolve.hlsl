// Low-resolution cloud march, pass 2 of 2: temporal resolve.
//
// Writes the full cloud buffer from this frame's sparse march (one pixel per
// 2x2 block) and last frame's buffer, reprojected. The approach follows the
// temporal clouds of Proper Shaders (Junior_Djjr); the code is ours.
//
//   * The pixel marched this frame keeps gResolve.x of its history, which is
//     what averages the march jitter over time.
//   * A pixel waiting for its turn keeps its history, leaking gResolve.y of the
//     way toward the upsampled fresh samples each frame.
//   * History is held inside the range of the four nearest fresh samples,
//     widened by gResolve.z. The rail has to be loose: all a 2x2 block knows
//     this frame is one sample, so a tight box would draw the blocks.
//
// Reprojection needs a distance along the ray. The buffer stores none, so the
// middle of the marched segment stands in for it: the parallax error of a
// cloud a few hundred units off that point is a small fraction of a pixel per
// frame, and every pixel is marched again within four frames.
#include "../../WoS_VolumetricClouds/native/wos_world.hlsli"
#include "../../WoS_VolumetricClouds/native/wos_clouds.hlsli"

sampler2D gMarch   : register(s0); // this frame's sparse march, point sampled
sampler2D gHistory : register(s1); // last frame's resolved buffer, bilinear

float4 gMarchGrid : register(c36);       // cloud buffer width, height; this frame's slot
float4 gResolve   : register(c37);       // marched blend, spatial mix, clamp expand, history valid
float4 gMarchSize : register(c38);       // march width, height, 1/width, 1/height
float4 gPrevViewProj[4] : register(c40); // last frame's viewProj, rebased to this frame's camera

float4 Fresh(float2 texel)
{
    return tex2Dlod(gMarch, float4((texel + 0.5) * gMarchSize.zw, 0.0, 0.0));
}

float4 main(float2 pixel : VPOS) : COLOR0
{
    float2 p = floor(pixel);
    // March texel i covers buffer pixel 2i + slot, so this pixel sits at m in
    // march texels: whole on a marched pixel, half-way between two otherwise.
    float2 m = (p - gMarchGrid.zw) * 0.5;
    float2 base = floor(m);
    float2 f = m - base;
    float4 a = Fresh(base);
    float4 b = Fresh(base + float2(1.0, 0.0));
    float4 c = Fresh(base + float2(0.0, 1.0));
    float4 d = Fresh(base + float2(1.0, 1.0));
    float4 spatial = lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
    bool marched = f.x < 0.25 && f.y < 0.25;

    float4 low = min(min(a, b), min(c, d));
    float4 high = max(max(a, b), max(c, d));
    float4 margin = (high - low) * gResolve.z + 0.004 + 0.02 * abs(high);

    float2 uv = (p + 0.5) / gMarchGrid.xy;
    float3 origin = float3(gWoSCamRight.w, gWoSCamUp.w, gWoSCamForward.w);
    float3 ray = normalize(WoSUnproject(uv, 0.5) - origin);
    WoSCloudRay segment;
    float distance = 20000.0;
    if (WoSCloudRaySetup(origin, ray, 1.0e7, segment))
        distance = 0.5 * (segment.enter + segment.leave);
    float4 relative = float4(ray * distance, 1.0);
    float4 clip = float4(dot(gPrevViewProj[0], relative), dot(gPrevViewProj[1], relative),
                         dot(gPrevViewProj[2], relative), dot(gPrevViewProj[3], relative));
    float w = max(clip.w, 0.0001);
    float2 previous = float2(clip.x / w * 0.5 + 0.5, 0.5 - clip.y / w * 0.5);
    bool valid = gResolve.w > 0.5 && clip.w > 0.0001 &&
                 all(previous > 0.0) && all(previous < 1.0);

    float4 result = spatial;
    [branch] if (valid)
    {
        float4 history = tex2Dlod(gHistory, float4(previous, 0.0, 0.0));
        history = clamp(history, low - margin, high + margin);
        float keep = marched ? gResolve.x : 1.0 - gResolve.y;
        result = lerp(spatial, history, keep);
    }
    result.rgb = max(result.rgb, 0.0);
    result.a = saturate(result.a);
    return result;
}
