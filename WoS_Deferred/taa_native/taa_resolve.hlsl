// TAA resolve: reprojected history, clipped to the current neighbourhood.
//
// Current is the display image of this frame, drawn with a sub-pixel jitter.
// Motion is not rendered by the game, so it is rebuilt from depth and the two
// engine cameras. The depth used is the nearest one in the 3x3 neighbourhood:
// an edge pixel then moves with the foreground object instead of smearing the
// background across the silhouette.
//
// The history is fetched with a 5-tap Catmull-Rom, which keeps a moving image
// from going soft the way repeated bilinear resampling does, then clipped toward
// the centre of the YCoCg box spanned by min/max and mean +/- gamma*sigma of the
// current 3x3. Whatever the history remembers that the current frame cannot
// produce around this pixel is ghosting, and is clipped away.
//
// Compile: fxc /T ps_3_0 /E main /O3 /Fo taa_resolve.cso taa_resolve.hlsl
sampler2D Current : register(s0);    // point
sampler2D SceneDepth : register(s1); // point, INTZ
sampler2D History : register(s2);    // linear, 16F
float4 InvVP[4] : register(c0);      // clip -> camera-relative world, rows
float4 PrevVP[4] : register(c4);     // camera-relative world -> previous clip, rows
float4 Metrics : register(c8);       // 1/w, 1/h, w, h
float4 Jitter : register(c9);        // current jitter in clip xy, history valid
float4 Blend : register(c10);        // feedback min, feedback max, clip gamma, debug view

float3 ToYCoCg(float3 c)
{
    return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25)));
}

float3 FromYCoCg(float3 c)
{
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

float3 HistoryCatmullRom(float2 uv)
{
    float2 position = uv * Metrics.zw;
    float2 centre = floor(position - 0.5) + 0.5;
    float2 f = position - centre;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 t0 = (centre - 1.0) * Metrics.xy;
    float2 t3 = (centre + 2.0) * Metrics.xy;
    float2 t12 = (centre + w2 / w12) * Metrics.xy;

    // The four corner taps of the 16 are dropped; their weights are the
    // smallest and renormalising keeps the result within the tap range.
    float3 sum = 0.0;
    float weight = 0.0;
    float w;
    w = w12.x * w0.y;  sum += tex2Dlod(History, float4(t12.x, t0.y, 0, 0)).rgb * w;  weight += w;
    w = w0.x * w12.y;  sum += tex2Dlod(History, float4(t0.x, t12.y, 0, 0)).rgb * w;  weight += w;
    w = w12.x * w12.y; sum += tex2Dlod(History, float4(t12.x, t12.y, 0, 0)).rgb * w; weight += w;
    w = w3.x * w12.y;  sum += tex2Dlod(History, float4(t3.x, t12.y, 0, 0)).rgb * w;  weight += w;
    w = w12.x * w3.y;  sum += tex2Dlod(History, float4(t12.x, t3.y, 0, 0)).rgb * w;  weight += w;
    return sum / max(weight, 0.0001);
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float3 centre = tex2Dlod(Current, float4(uv, 0, 0)).rgb;

    float3 m1 = 0.0, m2 = 0.0, lo = 1e5, hi = -1e5;
    float nearest = 2.0;
    float2 nearestOffset = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            float2 offset = float2(x, y) * Metrics.xy;
            float3 c = ToYCoCg(tex2Dlod(Current, float4(uv + offset, 0, 0)).rgb);
            m1 += c;
            m2 += c * c;
            lo = min(lo, c);
            hi = max(hi, c);
            float z = tex2Dlod(SceneDepth, float4(uv + offset, 0, 0)).r;
            if (z < nearest)
            {
                nearest = z;
                nearestOffset = offset;
            }
        }
    }

    if (Jitter.z < 0.5)
        return float4(centre, 1.0);

    // The pixel shows the surface point at its own position minus the jitter.
    float2 tapUV = uv + nearestOffset;
    float2 ndc = float2(tapUV.x * 2.0 - 1.0, 1.0 - tapUV.y * 2.0) - Jitter.xy;
    float4 clip = float4(ndc, min(nearest, 0.9999999), 1.0);
    float4 world = float4(dot(InvVP[0], clip), dot(InvVP[1], clip), dot(InvVP[2], clip), dot(InvVP[3], clip));
    if (!(abs(world.w) > 1e-12))
        return float4(centre, 1.0);
    float4 p = float4(world.xyz / world.w, 1.0);
    float4 previous = float4(dot(PrevVP[0], p), dot(PrevVP[1], p), dot(PrevVP[2], p), dot(PrevVP[3], p));
    if (!(previous.w > 1e-8))
        return float4(centre, 1.0);
    float2 previousNdc = previous.xy / previous.w;
    float2 previousUV = float2(previousNdc.x * 0.5 + 0.5, 0.5 - previousNdc.y * 0.5);
    float2 currentUV = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    float2 velocity = previousUV - currentUV;
    float2 historyUV = uv + velocity;

    if (Blend.w > 0.5 && Blend.w < 1.5)
        return float4(saturate(abs(velocity * Metrics.zw) * 0.25), 0.0, 1.0);

    if (any(historyUV < 0.0) || any(historyUV > 1.0))
        return float4(Blend.w > 1.5 ? float3(1.0, 0.0, 0.0) : centre, 1.0);

    float3 history = ToYCoCg(max(HistoryCatmullRom(historyUV), 0.0));
    if (Blend.w > 2.5 && Blend.w < 3.5)
        return float4(FromYCoCg(history), 1.0);

    float3 mean = m1 / 9.0;
    float3 sigma = sqrt(abs(m2 / 9.0 - mean * mean));
    // The variance box can shrink past the centre sample itself on an edge
    // (three bright taps among six dark ones). Clipping to a box that excludes
    // what this pixel shows now would pull a perfectly still image away from
    // itself every frame, so the box always contains the centre.
    float3 centreYCoCg = ToYCoCg(centre);
    float3 boxLo = min(max(lo, mean - Blend.z * sigma), centreYCoCg);
    float3 boxHi = max(min(hi, mean + Blend.z * sigma), centreYCoCg);
    float3 boxCentre = 0.5 * (boxLo + boxHi);
    // A flat axis (grey scenes have Cg = 0 everywhere) must not turn storage
    // rounding into "far outside": the clip scales the whole colour, so one
    // degenerate channel would drag all three toward the box centre. Half an
    // 8-bit step is below anything visible and above 16F rounding.
    float3 extent = max(0.5 * (boxHi - boxLo), 0.5 / 255.0);
    float3 fromCentre = history - boxCentre;
    float3 units = abs(fromCentre / extent);
    float outside = max(units.x, max(units.y, units.z));
    if (outside > 1.0)
        history = boxCentre + fromCentre / outside;

    // No luminance-difference weight here: on a jittered edge the current pixel
    // flips between both sides every frame by design, and trusting it more there
    // is exactly what keeps the edge noisy. Ghosting is the clip's job.
    // Fast motion resamples the history every frame; let it go sooner.
    float speed = length(velocity * Metrics.zw);
    float feedback = lerp(Blend.y, Blend.x, saturate(speed / 32.0));

    if (Blend.w > 1.5)
        return float4(feedback.xxx, 1.0);

    float3 result = lerp(centre, max(FromYCoCg(history), 0.0), feedback);
    return float4(result, 1.0);
}
