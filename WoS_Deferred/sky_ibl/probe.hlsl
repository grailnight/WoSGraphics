#include "package/pbr_legacy_bridge.hlsl"
#include "package/sky_ibl.hlsl"
float4 ProbeNormal : register(c0);
float4 ProbeView : register(c1);
float4 ProbeRho : register(c2);
float4 ProbeSpec : register(c3);
float4 main(float2 uv:TEXCOORD0):COLOR0
{
    return float4(WoSSkyLighting(normalize(ProbeNormal.xyz),normalize(ProbeView.xyz),ProbeRho.rgb,ProbeSpec.rgb,ProbeSpec.a,ProbeRho.a),.75);
}
