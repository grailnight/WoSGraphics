// Replacement for the engine's plain EFX particle vertex shader (plain_vs.cso:
// LocalToScreen only). Same position math, plus the view depth the soft pixel
// shader fades with. The engine's projection has clip w = view depth.
float4 LocalToScreen[4] : register(c0); // rows, as the native dp4s read them
float4 Jitter : register(c4);           // TAA clip-space step (x, y); 0 when the draw is not jittered

struct Input { float4 position : POSITION; float4 color : COLOR0; float2 uv : TEXCOORD0; };
struct Output { float4 position : POSITION; float4 color : COLOR0; float2 uv : TEXCOORD0; float depth : TEXCOORD1; };

Output main(Input i) {
    float4 p = float4(i.position.xyz, 1.0);
    float4 clip = float4(dot(p, LocalToScreen[0]), dot(p, LocalToScreen[1]), dot(p, LocalToScreen[2]), dot(p, LocalToScreen[3]));
    Output o;
    o.depth = clip.w;
    // Same term the TAA twins append: position + jitter * w.
    clip.xy += Jitter.xy * clip.w;
    o.position = clip;
    o.color = i.color;
    o.uv = i.uv;
    return o;
}
