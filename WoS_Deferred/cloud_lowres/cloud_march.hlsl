// Low-resolution cloud march, pass 1 of 2.
//
// Runs at the native bloom composite, before it, with the constants the hook
// has already uploaded for the bloom shader (camera c22..c25, inverse viewProj
// c32..c35, sun c29, cloud layer c16/c17/c20/c21). The march itself is the
// bloom's own WoSCloudMarch, unchanged.
//
// The cloud buffer is 1/2 or 1/4 of the screen, and this pass fills only one
// pixel of every 2x2 block of it per frame; cloud_resolve.hlsl rebuilds the
// rest from history. So one frame marches 1/16 or 1/64 of the screen.
//
// Every ray marches the whole layer, as if its pixel were sky. That makes the
// buffer independent of geometry: it has no depth edges to bleed across when
// upsampled and no disocclusions when reprojected. The bloom composite cuts
// the clouds against the full-resolution depth instead (WoSCloudsFromBuffer).
//
// Output: rgb = scattered light, a = transmittance (opacity already applied).
#include "../../WoS_VolumetricClouds/native/wos_world.hlsli"
#include "../../WoS_VolumetricClouds/native/wos_clouds.hlsli"

float4 gMarchGrid : register(c36); // cloud buffer width, height; this frame's slot in the 2x2 block
float4 gMarchSeed : register(c37); // x = jitter offset for this frame, in [0, 1)

float4 main(float2 pixel : VPOS) : COLOR0
{
    float2 cloudPixel = floor(pixel) * 2.0 + gMarchGrid.zw;
    float2 uv = (cloudPixel + 0.5) / gMarchGrid.xy;

    float3 origin = float3(gWoSCamRight.w, gWoSCamUp.w, gWoSCamForward.w);
    float3 ray = normalize(WoSUnproject(uv, 0.5) - origin);

    // The seed moves the first sample every time a pixel comes round again,
    // so history integrates step offsets one frame cannot afford.
    float jitter = frac(WoSCloudJitter(cloudPixel) + gMarchSeed.x);

    float transmittance = 1.0;
    float3 scattered = 0.0;
    WoSCloudRay segment;
    [branch] if (WoSCloudRaySetup(origin, ray, 1.0e7, segment))
        scattered = WoSCloudMarch(origin, ray, segment, normalize(gWoSSun.xyz), jitter, WOS_CLOUD_STEPS, transmittance);
    return float4(scattered, transmittance);
}
