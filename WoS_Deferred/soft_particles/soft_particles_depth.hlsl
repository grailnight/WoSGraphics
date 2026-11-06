// Copies the scene INTZ into the R32F texture the EFX shaders read as depth.
// Values stay hardware z (0 near, 1 far), which is what the native z-feather
// and decal shaders expect in ZBufferSampler.
sampler2D SceneDepth : register(s0);
float4 main(float2 uv : TEXCOORD0) : COLOR0 { return tex2Dlod(SceneDepth, float4(uv, 0, 0)).rrrr; }
