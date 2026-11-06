// DLAA present: the DLSS output back into the display image. The display
// image's own alpha is kept, as the TAA present does.
// View.x: 0 DLAA, 1 the aliased input (for A/B), 2 motion (pixels / 4).
//
// Compile: fxc /T ps_3_0 /E main /O3 /Fo dlss_present.cso dlss_present.hlsl
sampler2D Dlss : register(s0);   // point, 16F
sampler2D Input : register(s1);  // point, copy of the display image
sampler2D Motion : register(s2); // point, UV delta
float4 View : register(c0);      // mode, unused, w, h

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 input = tex2Dlod(Input, float4(uv, 0, 0));
    if (View.x > 1.5)
    {
        float2 m = tex2Dlod(Motion, float4(uv, 0, 0)).xy * View.zw;
        return float4(saturate(abs(m) * 0.25), 0.0, input.a);
    }
    if (View.x > 0.5)
        return input;
    return float4(tex2Dlod(Dlss, float4(uv, 0, 0)).rgb, input.a);
}
