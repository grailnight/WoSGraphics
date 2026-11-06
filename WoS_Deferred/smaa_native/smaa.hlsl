float4 metrics : register(c0);
#define SMAA_RT_METRICS metrics
#define SMAA_HLSL_3 1
#define SMAA_PRESET_HIGH 1
#define SMAA_AREATEX_SELECT(sample) sample.rg
#include "third_party/SMAA.hlsl"
sampler2D sourceTex : register(s0);
sampler2D auxiliaryTex : register(s1);
sampler2D searchTex : register(s2);
float4 Edges(float2 uv:TEXCOORD0):COLOR0 {
    float4 offsets[3]; SMAAEdgeDetectionVS(uv,offsets);
    return float4(SMAAColorEdgeDetectionPS(uv,offsets,sourceTex),0,0);
}
float4 Weights(float2 uv:TEXCOORD0):COLOR0 {
    float4 offsets[3]; float2 pixel;
    SMAABlendingWeightCalculationVS(uv,pixel,offsets);
    return SMAABlendingWeightCalculationPS(uv,pixel,offsets,sourceTex,auxiliaryTex,searchTex,0);
}
float4 Blend(float2 uv:TEXCOORD0):COLOR0 {
    float4 offset; SMAANeighborhoodBlendingVS(uv,offset);
    float4 result=SMAANeighborhoodBlendingPS(uv,offset,sourceTex,auxiliaryTex);
    result.a=tex2D(sourceTex,uv).a;
    return result;
}
