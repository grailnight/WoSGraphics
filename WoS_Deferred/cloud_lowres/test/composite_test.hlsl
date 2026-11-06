// Test stand-in for the cloud lines of bloom_gta.hlsl: the same surface read,
// the same branch between the low-resolution buffer and the per-pixel march.
// Output: rgb = scattered light, a = transmittance. c39.x = 1 forces the
// per-pixel march (the reference); WOS_CLOUD_STEPS=256 builds the ground truth.
#include "../../../WoS_VolumetricClouds/native/wos_world.hlsli"
#include "../../../WoS_VolumetricClouds/native/wos_clouds.hlsli"

sampler2D gWoSCloudBuffer  : register(s4);
float4 gWoSCloudBufferInfo : register(c36);
float4 gTestMode           : register(c39);

float4 main(float2 uv : TEXCOORD0, float2 pixelPosition : VPOS) : COLOR0
{
    WoSSurface surface = WoSReadSurface(uv);
    float transmittance = 1.0;
    float3 scattered;
    [branch] if (gTestMode.x < 0.5 && gWoSCloudBufferInfo.w == 11.0)
        scattered = WoSCloudsFromBuffer(gWoSCloudBuffer, surface, uv, pixelPosition, normalize(gWoSSun.xyz), transmittance);
    else
        scattered = WoSVolumetricClouds(surface, normalize(gWoSSun.xyz), pixelPosition, transmittance);
    return float4(scattered, transmittance);
}
