// Bilateral upsample of the half-resolution sky visibility to full resolution,
// so that the resolve pays one point sample for it.
//
// The visibility pass rotates its slices by a 4x4 ordered pattern. A 4x4
// footprint of half-resolution texels holds every rotation exactly once, so
// averaging it integrates twelve slice directions instead of three and the
// pattern cancels instead of showing as a grid. Depth and normal weights keep
// that average from crossing silhouettes and creases.
//
// Compile: fxc /T ps_3_0 /E main /O3
sampler2D Occlusion : register(s0);
sampler2D SceneDepth : register(s1);
sampler2D NormalShadowSet : register(s2);
float4 InverseVP[4] : register(c0);
float4 Screen : register(c4);   // full-resolution pixel size xy
float4 Upsample : register(c6); // half-resolution pixel size xy, relative depth tolerance

// Proportional to linear view depth: comparing taps needs only a ratio, and the
// fourth inverse row alone gives one, at one dot product per tap.
float DepthKey(float2 uv, float z)
{
    float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, z, 1.0);
    return 1.0 / max(dot(InverseVP[3], clip), 0.000000001);
}

float3 DecodeNormal(float2 packed)
{
    float2 f = packed * 2 - 1;
    float3 n = float3(f, 1 - abs(f.x) - abs(f.y));
    float t = saturate(-n.z);
    n.xy += float2(n.x >= 0 ? -t : t, n.y >= 0 ? -t : t);
    return normalize(n);
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float depth = tex2Dlod(SceneDepth, float4(uv, 0.0, 0.0)).r;
    float key = DepthKey(uv, depth);
    // Taken before any branch so the derivative is not read in divergent flow.
    float slope = fwidth(key);

    // Uncovered pixels and sky must come out neutral: the resolve multiplies by
    // this, and anything but one there would darken something the pass has no
    // business touching.
    float4 ns = tex2Dlod(NormalShadowSet, float4(uv, 0.0, 0.0));
    if (floor(ns.a + 0.5) < 0.5) return 1.0;
    if (!(depth >= 0.0 && depth < 0.9999999)) return 1.0;
    float3 normal = DecodeNormal(ns.xy);

    // Allowing the measured local depth slope keeps grazing surfaces, which is
    // most of a street, from rejecting every tap.
    float tolerance = max(key * max(Upsample.z, 0.0) + slope * 8.0, key * 0.004);

    float2 grid = uv / Upsample.xy - 0.5;
    float2 fraction = frac(grid);
    float2 base = floor(grid);

    float occluded = 0.0, total = 0.0, reachable = 0.0;
    [loop]
    for (int i = 0; i < 16; ++i)
    {
        float row = floor((float)i * 0.25);
        float2 xy = float2((float)i - row * 4.0, row) - 1.0; // -1..2 on each axis
        float2 tapUV = (base + xy + 0.5) * Upsample.xy;
        tapUV = clamp(tapUV, Upsample.xy * 0.5, 1.0 - Upsample.xy * 0.5);
        // Nearly flat: every rotation of the pattern should count, with only a
        // mild preference for the texels closest to this pixel.
        float2 away = abs(xy - fraction);
        float2 tent = 1.5 - away * 0.5;
        float spatial = tent.x * tent.y;
        float tapDepth = tex2Dlod(SceneDepth, float4(tapUV, 0.0, 0.0)).r;
        float similarity = tapDepth < 0.9999999
                         ? saturate(1.0 - abs(DepthKey(tapUV, tapDepth) - key) / tolerance)
                         : 0.0;
        float4 tapNs = tex2Dlod(NormalShadowSet, float4(tapUV, 0.0, 0.0));
        float facing = floor(tapNs.a + 0.5) < 0.5 ? 0.0 : saturate(dot(normal, DecodeNormal(tapNs.xy)));
        facing *= facing;
        float weight = spatial * similarity * similarity * facing * facing;
        // Accumulate the deficit rather than the visibility: a weighted mean of
        // values that are all exactly one would otherwise come back as 0.99999994.
        occluded += (1.0 - tex2Dlod(Occlusion, float4(tapUV, 0.0, 0.0)).r) * weight;
        total += weight;
        reachable += spatial;
    }

    if (total <= 0.0001) return 1.0;
    // Trust the mean in proportion to how much of the footprint agreed with this
    // pixel, so a lone surviving tap cannot restore full amplitude as a dot.
    float confidence = saturate(total / max(reachable * 0.35, 0.0001));
    confidence *= confidence;
    return saturate(1.0 - (occluded / total) * confidence).xxxx;
}
