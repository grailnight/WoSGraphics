// Street steam (SM3 port): Spider-Man 3 draws its street steam as texture *
// vertex alpha (fx_smk_light, top row of a 4x2 atlas) and fades it out within
// 1.5..3 m of the camera. Here the same sprite is lit with the scene's light,
// blended premultiplied over the linear HDR scene (ONE, SRCALPHA) and softened
// where it cuts into the street or a facade, since these quads are tens of
// metres wide and would otherwise show hard seams.
sampler2D Smoke : register(s0);       // fx_smk_light, sRGB read
sampler2D Depth : register(s1);       // the INTZ the scene was drawn with
float4 Inv[4]   : register(c0);       // clip -> camera-relative world
float4 Screen   : register(c4);       // 1/width, 1/height, soft range m, near fade per m
float4 Light    : register(c5);       // linear HDR light on the steam, unused w
float4 Forward  : register(c6);       // view direction

float4 main(float4 data : TEXCOORD0, float grey : TEXCOORD1, float2 vpos : VPOS) : COLOR0
{
    float4 smoke = tex2D(Smoke, data.xy);
    float2 uv = (vpos + 0.5) * Screen.xy;
    float z = tex2D(Depth, uv).r;
    float4 q = float4(uv.x * 2 - 1, 1 - uv.y * 2, min(z, 0.99999), 1);
    float4 hit = float4(dot(Inv[0], q), dot(Inv[1], q), dot(Inv[2], q), dot(Inv[3], q));
    float scene = z >= 0.99999 ? 1e7 : dot(hit.xyz / hit.w, Forward.xyz);
    float soft = saturate((scene - data.w) / Screen.z);
    float nearFade = saturate(data.w * Screen.w - 1);   // SM3 ALPHAFADE (0.667, -1)
    float a = saturate(smoke.a * data.z * soft * nearFade);
    // grey: SM3's per-particle colour (white for the city smoke, 0.18..0.67 for funnel steam)
    return float4(smoke.rgb * grey * Light.rgb * a, 1 - a);
}
