// Temporal accumulation of the half-resolution sky visibility.
//
// The visibility pass rotates its slices and rays by a pattern that now also
// shifts every frame. On its own that would flicker; accumulated here over
// about eight frames it integrates many more directions than one frame can
// afford, and the camera-fixed pattern that used to ripple across surfaces as
// the camera moved is gone.
//
// History is reprojected through last frame's camera and accepted only where
// the surface it stored lies at the distance this frame's surface should have
// had from that camera, so a disocclusion starts fresh instead of smearing.
// It is also clamped to the range of this frame's neighbourhood, so an
// occluder that moved away (a car driving off) does not leave a shadow behind.
//
// The resolve runs once per geometry batch, and a batch owns only the pixels
// its geometry tagged. Pixels of the other batch are copied through verbatim,
// so after both batches the buffer holds this frame's result everywhere, and
// the runtime alternates the two buffers on every pass, not every frame.
//
// COLOR0: R = visibility, G = camera distance of the receiver (0 = none).
// Compile: fxc /T ps_3_0 /E main /O3
sampler2D Current : register(s0);    // this frame's half-resolution visibility
sampler2D SceneDepth : register(s1);
sampler2D History : register(s2);    // the latest output of this pass
sampler2D NormalShadowSet : register(s3); // alpha = this batch's coverage tag
float4 InverseVP[4] : register(c0);
float4 Screen : register(c4);        // full-resolution pixel size xy
float4 Upsample : register(c6);      // half-resolution pixel size xy
float4 PrevVP[4] : register(c7);     // this frame's camera-relative position to last frame's clip
float4 TemporalAo : register(c11);   // x history valid, y weight of the new frame, z relative distance tolerance, w clamp margin
float4 CameraDelta : register(c12);  // xyz this frame's camera minus last frame's

float2 Snap(float2 uv)
{
    return (floor(uv / Screen.xy) + 0.5) * Screen.xy;
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float2 receiver = Snap(uv);
    // Not this batch's pixel: keep whatever the buffer holds for it.
    if (floor(tex2Dlod(NormalShadowSet, float4(receiver, 0.0, 0.0)).a + 0.5) < 0.5)
        return tex2Dlod(History, float4(uv, 0.0, 0.0));
    float current = tex2Dlod(Current, float4(uv, 0.0, 0.0)).r;
    float z = tex2Dlod(SceneDepth, float4(receiver, 0.0, 0.0)).r;
    float4 clip = float4(receiver.x * 2.0 - 1.0, 1.0 - receiver.y * 2.0, z, 1.0);
    float4 world = float4(dot(InverseVP[0], clip), dot(InverseVP[1], clip),
                          dot(InverseVP[2], clip), dot(InverseVP[3], clip));
    bool surface = z >= 0.0 && z < 0.9999999 && world.w > 0.00000001;
    float3 p = world.xyz / max(world.w, 0.00000001);
    float distance = surface ? length(p) : 0.0;
    float result = current;

    [branch] if (surface && TemporalAo.x > 0.5)
    {
        float4 h = float4(p, 1.0);
        float4 pc = float4(dot(PrevVP[0], h), dot(PrevVP[1], h), dot(PrevVP[2], h), dot(PrevVP[3], h));
        float2 prevUV = float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);
        bool inside = pc.w > 0.00001 && all(prevUV >= Upsample.xy * 0.5) && all(prevUV <= 1.0 - Upsample.xy * 0.5);
        float4 history = tex2Dlod(History, float4(prevUV, 0.0, 0.0));
        float expected = length(p + CameraDelta.xyz);
        bool same = inside && history.g > 0.0
                 && abs(history.g - expected) <= expected * TemporalAo.z + 0.05;
        if (same)
        {
            // Neighbourhood of this frame, 3x3 half-resolution texels.
            float low = current, high = current;
            [unroll] for (int y = -1; y <= 1; ++y)
            {
                [unroll] for (int x = -1; x <= 1; ++x)
                {
                    float v = tex2Dlod(Current, float4(uv + float2(x, y) * Upsample.xy, 0.0, 0.0)).r;
                    low = min(low, v);
                    high = max(high, v);
                }
            }
            float kept = clamp(history.r, low - TemporalAo.w, high + TemporalAo.w);
            result = lerp(kept, current, saturate(TemporalAo.y));
        }
    }
    return float4(result, distance, 0.0, 0.0);
}
