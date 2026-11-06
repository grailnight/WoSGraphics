// Sky visibility for the deferred resolve (replaces the 16 tap ambient occlusion).
//
// The resolve multiplies everything that is not direct light by this value:
// the engine's own environment sample, the baked prelight, emissive, fog, and
// the Sky IBL pass that follows it. A path tracer gets that term by shooting
// rays at the sky and counting the ones that escape. Here the same quantity is
// integrated analytically from depth-buffer horizons, the construction of
// ground truth ambient occlusion (Jimenez et al. 2016): for a few slices
// through the view vector, the highest occluder on each side bounds the open
// arc, and the cosine weighted integral of that arc is exact for a uniform sky.
// A street canyon, an alley or the underside of a bridge therefore goes as dark
// as the fraction of sky it actually sees, not to a fixed "half at a corner".
//
// Light that such a surface does receive from the lit surfaces around it is
// added later by SSGI at the bloom draw, whose donor is this frame's image
// after this darkening, so the two do not count the same light twice.
//
// The register contract is the previous pass's, unchanged, so the renderer
// DLL does not need to be rebuilt:
//   Ao.x world radius, Ao.y strength (the runtime writes 1), Ao.z maximum
//   camera distance, Ao.w bias against depth quantisation on flat ground.
// Anything off screen or sky counts as open, the screen-space equivalent of a
// ray that escaped.
//
// What the frame cannot show, the top-down height map can: the far side of a
// street, the building behind the camera. Eight cosine distributed rays march
// through it in world space and count as blocked once they dip below the
// highest surface. The two estimates only ever miss occluders, so the pass
// keeps the lower one. A receiver under an overhang is below the map itself;
// for it the map knows nothing and the screen estimate stands alone.
//   c50 u = x * c50.x + c50.y, v = z * c50.z + c50.w (camera-relative x, z)
//   c51 surface height = c51.x - depth * c51.y, c51.z enabled, c51.w ray length
//   c52 x world units per map texel, yz per-frame pattern shift
//
// Runs at half of each axis. The slice rotation and step jitter follow a 4x4
// ordered pattern, and the upsample averages exactly one full period of it.
// Compile: fxc /T ps_3_0 /E main /O3
sampler2D SceneDepth : register(s0);
sampler2D NormalShadowSet : register(s1);
sampler2D SkyHeight : register(s3);
float4 InverseVP[4] : register(c0);
float4 Screen : register(c4); // full-resolution pixel size xy
float4 Ao : register(c5);     // world radius, strength, maximum camera distance, bias
float4 HeightMap : register(c50);
float4 HeightRange : register(c51);
float4 HeightTexel : register(c52);

#define SLICES 3
#define STEPS 12
#define THIN_RELAX 0.1
#define HALF_PI 1.57079633
#define PI 3.14159265
#define HEIGHT_RAYS 8
#define HEIGHT_STEPS 8

// The camera sits at the origin of this space, the same convention the resolve
// uses when it takes the view vector as the negated position.
bool Position(float2 uv, float z, out float3 p)
{
    float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, z, 1.0);
    float4 world = float4(dot(InverseVP[0], clip), dot(InverseVP[1], clip),
                          dot(InverseVP[2], clip), dot(InverseVP[3], clip));
    p = world.xyz / max(world.w, 0.00000001);
    return z >= 0.0 && z < 0.9999999 && world.w > 0.00000001;
}

float3 DecodeNormal(float2 packed)
{
    float2 f = packed * 2 - 1;
    float3 n = float3(f, 1 - abs(f.x) - abs(f.y));
    float t = saturate(-n.z);
    n.xy += float2(n.x >= 0 ? -t : t, n.y >= 0 ? -t : t);
    return normalize(n);
}

// Exact arc cosine: an approximation of a few hundredths of a radian leaves a
// flat, fully open floor reading as ten percent occluded, because the tangent
// plane horizon no longer maps back onto the clamp.
float FastAcos(float x)
{
    return acos(clamp(x, -1.0, 1.0));
}

// Cosine weighted open arc between the horizon angle h and the normal angle g,
// both measured in the slice from the view vector.
float Arc(float h, float g, float cosG, float sinG)
{
    return -cos(2.0 * h - g) + cosG + 2.0 * h * sinG;
}

// Ordered 4x4 index 0..15, built from two nested 2x2 patterns.
float Bayer4(float2 cell)
{
    float2 lo = fmod(cell, 2.0);
    float2 hi = fmod(floor(cell * 0.5), 2.0);
    float b0 = lo.x * 2.0 + lo.y * 3.0 - 4.0 * lo.x * lo.y; // 0 2 / 3 1
    float b1 = hi.x * 2.0 + hi.y * 3.0 - 4.0 * hi.x * hi.y;
    return b0 * 4.0 + b1;
}

bool HeightAt(float3 q, out float h)
{
    float2 uv = float2(q.x * HeightMap.x + HeightMap.y, q.z * HeightMap.z + HeightMap.w);
    h = 0.0;
    if (!all(uv > 0.001) || !all(uv < 0.999)) return false;
    h = HeightRange.x - tex2Dlod(SkyHeight, float4(uv, 0.0, 0.0)).r * HeightRange.y;
    return true;
}

float HeightVisibility(float3 p, float3 n, float rotation, float jitter)
{
    // One exit only. With early returns ahead of the ray loops fxc produced
    // code that returned 1 for every upward-facing receiver once the loops
    // were present (sky_visibility/gpu_test.cpp caught it); a flag does not.
    float texel = max(HeightTexel.x, 0.01);
    // Probe a little way out along the normal: a wall pixel's own column is
    // the roof above it, the column just outside is the street.
    float3 probe = p + n * max(0.5, 3.0 * texel);
    float h0 = 0.0;
    bool mapped = HeightRange.z > 0.5 && HeightAt(probe, h0);
    // Below the highest surface: an overhang or an interior the map cannot
    // describe. That receiver is left to the screen-space estimate.
    bool usable = mapped && probe.y >= h0 - max(1.0, 4.0 * texel);

    float3 helper = abs(n.y) < 0.9 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(helper, n));
    float3 bitangent = cross(n, tangent);
    float3 origin = p + n * max(0.3, 2.0 * texel);
    float len = max(HeightRange.w, 1.0);
    float bias = max(0.2, texel);
    float open = 0.0;
    [branch] if (usable)
    {
        [loop]
        for (int r = 0; r < HEIGHT_RAYS; ++r)
        {
            // Stratified azimuth rotated by the 4x4 pattern, elevations on a
            // golden-ratio sequence: the upsample then averages 128 directions.
            float a = ((float)r + rotation) / (float)HEIGHT_RAYS;
            float c = frac((float)r * 0.618034 + jitter);
            float radial = sqrt(c);
            float sn, cs; sincos(a * 6.28318531, sn, cs);
            float3 dir = tangent * (cs * radial) + bitangent * (sn * radial) + n * sqrt(max(1.0 - c, 0.0));
            float blocked = 0.0;
            bool inside = true;
            [loop]
            for (int k = 0; k < HEIGHT_STEPS; ++k)
            {
                float f = ((float)k + jitter) / (float)HEIGHT_STEPS;
                float3 q = origin + dir * (len * f * f + texel);
                float h = 0.0;
                // Leaving the map counts as escaping to the sky.
                inside = inside && HeightAt(q, h);
                if (inside && blocked < 0.5 && q.y < h - bias) blocked = 1.0;
            }
            open += 1.0 - blocked;
        }
    }
    return usable ? open / (float)HEIGHT_RAYS : 1.0;
}

// Point sampling returns one full-resolution pixel; the position must be
// rebuilt at that pixel's centre, not at the half-resolution texel centre half
// a pixel away, or every point of a grazing floor floats off its own plane and
// the floor occludes itself.
float2 Snap(float2 uv)
{
    return (floor(uv / Screen.xy) + 0.5) * Screen.xy;
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    uv = Snap(uv);
    // Full visibility is the neutral answer. A zero here would read as complete
    // occlusion and paint every uncovered pixel black once the resolve multiplies.
    float4 ns = tex2Dlod(NormalShadowSet, float4(uv, 0.0, 0.0));
    if (floor(ns.a + 0.5) < 0.5) return 1.0;

    float3 p;
    float centre = tex2Dlod(SceneDepth, float4(uv, 0.0, 0.0)).r;
    if (!Position(uv, centre, p)) return 1.0;
    float distance = length(p);
    float maxDistance = max(Ao.z, 0.01);
    if (distance > maxDistance) return 1.0;
    float3 n = DecodeNormal(ns.xy);
    float3 v = -p / max(distance, 0.00001);

    // World units one screen pixel covers at this depth, from the projection
    // at the SAME depth, so the radius does not depend on what a neighbour holds.
    float3 stepX, stepY;
    Position(uv + float2(Screen.x, 0.0), centre, stepX);
    Position(uv + float2(0.0, Screen.y), centre, stepY);
    float2 pixelScale = max(float2(length(stepX - p), length(stepY - p)), 0.00001);

    float radius = max(Ao.x, 0.01);
    // Up to the whole screen: the wall across a street is usually on the far
    // side of the frame, and a shorter clamp left canyon walls reading 0.99
    // open where a ray tracer says 0.5 (sky_visibility/gpu_test.cpp).
    float2 uvRadius = min(Screen.xy * radius / pixelScale, 1.0);
    // Fewer than two pixels of reach means the geometry is too far for the
    // radius to resolve anything on screen; only the height map can.
    bool screenReach = max(uvRadius.x / Screen.x, uvRadius.y / Screen.y) >= 2.0;

    float2 cell = floor(uv / (Screen.xy * 2.0));
    float order = Bayer4(cell);
    // c52.yz shift the pattern every frame; the temporal pass after this one
    // integrates the shifts instead of letting a fixed pattern ripple across
    // surfaces as the camera moves. Zero reproduces a static pattern.
    float rotation = frac((order + 0.5) / 16.0 + HeightTexel.y);
    float jitter = frac((fmod(order * 7.0, 16.0) + 0.5) / 16.0 + HeightTexel.z);
    float bias = max(Ao.w, 0.0) * 0.08;

    float visible = 0.0, open = 0.0;
    [loop]
    for (int s = 0; s < SLICES && screenReach; ++s)
    {
        float phi = ((float)s + rotation) * (PI / (float)SLICES);
        float2 d; sincos(phi, d.y, d.x);
        float2 direction = d * uvRadius;

        // The slice plane holds the view vector and the world direction that
        // this screen direction maps to at the receiver's depth.
        float3 along;
        Position(uv + direction * 0.05, centre, along);
        float3 t = along - p;
        t -= v * dot(t, v);
        float tLen = length(t);
        if (tLen < 0.0000001) continue;
        t /= tLen;
        float3 axis = cross(v, t);
        float3 np = n - axis * dot(n, axis);
        float npLen = length(np);
        if (npLen < 0.0001) continue;
        float cosG = saturate(dot(np, v) / npLen);
        float g = (dot(np, t) >= 0.0 ? 1.0 : -1.0) * FastAcos(cosG);
        float sinG = sin(g);

        // Horizons start at the tangent plane: nothing below it is visible
        // to a Lambertian receiver whatever the depth buffer holds.
        float lowPositive = cos(g + HALF_PI);
        float lowNegative = cos(g - HALF_PI);
        float horizonPositive = lowPositive;
        float horizonNegative = lowNegative;

        [loop]
        for (int k = 0; k < STEPS; ++k)
        {
            float f = ((float)k + jitter) / (float)STEPS;
            // Quadratic spacing: contact detail near the receiver, the far
            // walls of a canyon with the last few steps. Never closer than
            // one and a half pixels, which is the receiver itself.
            float2 offset = direction * (f * f);
            float2 minimum = abs(d) * Screen.xy * 1.5;
            offset = sign(d) * max(abs(offset), minimum);

            for (int side = 0; side < 2; ++side)
            {
                float2 tapUV = Snap(side == 0 ? uv + offset : uv - offset);
                if (!all(tapUV >= Screen.xy * 0.5) || !all(tapUV <= 1.0 - Screen.xy * 0.5)) continue;
                float3 q;
                if (!Position(tapUV, tex2Dlod(SceneDepth, float4(tapUV, 0.0, 0.0)).r, q)) continue;
                float3 delta = q - p;
                float len = length(delta);
                if (len < 0.0001) continue;
                float c = dot(delta / len, v) - bias;
                // Occluders fade out over the last 40% of the radius, so the
                // edge of the search reads as a soft limit rather than a ring.
                float fade = saturate((radius - len) / (radius * 0.4));
                // The depth buffer holds only the front face, so a lamp post or
                // a character reads as a wall of infinite depth and throws a
                // halo far across the ground. A horizon that later samples do
                // not keep up is let down again, a little per step (Jimenez
                // et al.'s thickness heuristic): a real wall keeps raising it,
                // a thin object is passed and the sky behind it counts again.
                if (side == 0)
                {
                    float value = lerp(lowPositive, c, fade);
                    horizonPositive = value > horizonPositive ? value : lerp(horizonPositive, value, THIN_RELAX);
                }
                else
                {
                    float value = lerp(lowNegative, c, fade);
                    horizonNegative = value > horizonNegative ? value : lerp(horizonNegative, value, THIN_RELAX);
                }
            }
        }

        float hPositive = g + min(FastAcos(horizonPositive) - g, HALF_PI);
        float hNegative = g + max(-FastAcos(horizonNegative) - g, -HALF_PI);
        visible += npLen * (Arc(hPositive, g, cosG, sinG) + Arc(hNegative, g, cosG, sinG));
        open += npLen * (Arc(g + HALF_PI, g, cosG, sinG) + Arc(g - HALF_PI, g, cosG, sinG));
    }

    float visibility = open > 0.0001 ? saturate(visible / open) : 1.0;
    visibility = min(visibility, HeightVisibility(p, n, rotation, jitter));
    // Fade to open before the distance limit so the cut is not a visible line.
    visibility = lerp(visibility, 1.0, smoothstep(maxDistance * 0.8, maxDistance, distance));
    visibility = 1.0 - saturate(Ao.y) * (1.0 - visibility);
    return visibility.xxxx;
}
