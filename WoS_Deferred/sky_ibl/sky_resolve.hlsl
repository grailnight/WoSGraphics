#include "package/pbr_legacy_bridge.hlsl"
#include "package/sky_ibl.hlsl"
sampler2D NormalShadowSet : register(s1);
sampler2D DiffuseFog : register(s2);
sampler2D SpecularPower : register(s3);
sampler2D SceneDepth : register(s4);
// Still bound from the deferred resolve that runs just before this pass.
sampler2D LightSets : register(s5);
sampler2D AmbientVisibility : register(s8);
float4 InverseVP[4] : register(c0);
float4 Screen : register(c4);
float4 Ambient : register(c28);
float3 DecodeNormal(float2 packed)
{
    float2 f=packed*2-1;
    float3 n=float3(f,1-abs(f.x)-abs(f.y));float t=saturate(-n.z);
    n.xy+=float2(n.x>=0?-t:t,n.y>=0?-t:t);
    return normalize(n);
}
float4 main(float2 uv:TEXCOORD0):COLOR0
{
    float4 ns=tex2D(NormalShadowSet,uv);
    clip(ns.w-.5);
    float4 rf=tex2D(DiffuseFog,uv),sp=tex2D(SpecularPower,uv);
    float4 p=float4(uv.x*2-1,1-uv.y*2,tex2D(SceneDepth,uv).r,1);
    float4 world=float4(dot(InverseVP[0],p),dot(InverseVP[1],p),dot(InverseVP[2],p),dot(InverseVP[3],p));
    clip(abs(world.w)-1e-8);
    float3 pos=world.xyz/world.w;
    float3 view=-pos*rsqrt(max(dot(pos,pos),1e-12));
    float3 n=DecodeNormal(ns.xy);
    // Material class of the draw that wrote this pixel (texel 31.y of its light set).
    // Facade windows (12) keep the native reflection already in the residual;
    // a sky reflection added here would reflect them twice.
    float materialClass=tex2Dlod(LightSets,float4(31.5/32,(floor(ns.w+.5)+.5)/Screen.z,0,0)).y;
    WoSPbrSetClass(abs(materialClass-12)<.5?0:materialClass,0);
    WoSSpecularAASetup(n,pos);
    float ao=1-(1-tex2D(AmbientVisibility,uv).r)*saturate(Ambient.x);
    return float4(WoSSkyLighting(n,view,rf.rgb,sp.rgb,sp.a,ao)*saturate(rf.a),0);
}
