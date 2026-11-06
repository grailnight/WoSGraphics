// TAA present: resolved history back to the display image.
//
// Temporal accumulation softens texture detail a little; a cross-shaped unsharp
// mask gives it back. The sharpened value is clamped to the min/max of its own
// five taps, so it can restore contrast but never draw a halo. Alpha is the
// display image's own, which later native passes may still read.
//
// Compile: fxc /T ps_3_0 /E main /O3 /Fo taa_present.cso taa_present.hlsl
sampler2D Resolved : register(s0); // point, 16F
sampler2D Source : register(s1);   // point, this frame's display image
float4 Metrics : register(c8);     // 1/w, 1/h, w, h
float4 Present : register(c11);    // sharpen amount

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float3 c = tex2Dlod(Resolved, float4(uv, 0, 0)).rgb;
    float alpha = tex2Dlod(Source, float4(uv, 0, 0)).a;
    if (!(Present.x > 0.0))
        return float4(c, alpha);

    float3 n = tex2Dlod(Resolved, float4(uv - float2(0, Metrics.y), 0, 0)).rgb;
    float3 s = tex2Dlod(Resolved, float4(uv + float2(0, Metrics.y), 0, 0)).rgb;
    float3 w = tex2Dlod(Resolved, float4(uv - float2(Metrics.x, 0), 0, 0)).rgb;
    float3 e = tex2Dlod(Resolved, float4(uv + float2(Metrics.x, 0), 0, 0)).rgb;
    float3 lo = min(c, min(min(n, s), min(w, e)));
    float3 hi = max(c, max(max(n, s), max(w, e)));
    float3 sharp = c + (4.0 * c - n - s - w - e) * (0.25 * Present.x);
    return float4(clamp(sharp, lo, hi), alpha);
}
