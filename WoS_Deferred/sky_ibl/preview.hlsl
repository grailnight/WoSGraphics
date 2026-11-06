#include "package/pbr_legacy_bridge.hlsl"
#include "package/sky_ibl.hlsl"
float4 Preview : register(c0); // IBL enabled in this column; unused
float4 main(float2 uv:TEXCOORD0):COLOR0
{
    float column=floor(uv.x*4);
    float2 p=float2(frac(uv.x*4)*2-1,(uv.y*2-1));
    float radius=dot(p,p);
    if(radius>.88) return float4(.025,.028,.035,1);
    float3 n=normalize(float3(p.x,-p.y,sqrt(max(1-radius,0))));
    float3 v=float3(0,0,1);
    float3 rho=column<2?float3(.5,.08,.025):float3(.14,.23,.45);
    float power=fmod(column,2)<.5?4:512;
    float3 direct=WoSPbrLegacyDirect(n,v,normalize(float3(-.4,.7,.5)),float3(.85,.7,.55),rho,.8,power,1);
    float3 ibl=WoSSkyLighting(n,v,rho,.8,power,1);
    float3 color=direct+ibl;
    color=color/(1+color);
    return float4(pow(max(color,0),1/2.2),1);
}
