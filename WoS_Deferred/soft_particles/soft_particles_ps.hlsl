// Soft plain EFX particles: the native texture * vertex colour, faded where the
// particle comes within Range metres of the scene behind it.
sampler2D Diffuse : register(s0);    // the engine's own binding, untouched
sampler2D SceneDepth : register(s1); // R32F copy of the scene INTZ

float4 Screen : register(c0); // xy = 1 / target size; 1 / view depth = z * Screen.z + Screen.w
float4 Soft : register(c1);   // x = range (m), y = extra range per metre of distance, z = strength
float4 Mode : register(c2);   // x = scale colour, y = colour towards white, z = scale alpha

float4 main(float4 color : COLOR0, float2 uv : TEXCOORD0, float depth : TEXCOORD1, float2 pixel : VPOS) : COLOR0 {
    float4 c = tex2D(Diffuse, uv) * color;
    float z = tex2Dlod(SceneDepth, float4((pixel + 0.5) * Screen.xy, 0, 0)).r;
    // z = 1 (sky) gives the far plane, so particles against the sky stay whole.
    float scene = 1.0 / max(z * Screen.z + Screen.w, 1e-6);
    float range = max(Soft.x + Soft.y * depth, 1e-3);
    float fade = lerp(1.0, saturate((scene - depth) / range), Soft.z);
    c.rgb = lerp(c.rgb, c.rgb * fade, Mode.x);
    c.rgb = lerp(c.rgb, lerp(float3(1, 1, 1), c.rgb, fade), Mode.y);
    c.a *= lerp(1.0, fade, Mode.z);
    return c;
}
