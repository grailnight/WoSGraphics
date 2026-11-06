// Test-only geometry for water_ssr_gpu_test.cpp.
// VS: camera-relative positions; emits the interpolators the native water VS
// provides (TEXCOORD2 world position, TEXCOORD3 normal, TEXCOORD4 UV).
float4 VP[4] : register(c0);
struct Out { float4 position : POSITION; float3 world : TEXCOORD2; float3 normal : TEXCOORD3; float2 uv : TEXCOORD4; };
Out vs_main(float3 p : POSITION)
{
    Out o;
    float4 h = float4(p, 1.0);
    o.position = float4(dot(VP[0], h), dot(VP[1], h), dot(VP[2], h), dot(VP[3], h));
    o.world = p;
    o.normal = float3(0.0, 1.0, 0.0);
    o.uv = p.xz * 0.1;
    return o;
}
float4 Colour : register(c0);
float4 ps_solid(float3 world : TEXCOORD2) : COLOR0
{
    return Colour;
}
