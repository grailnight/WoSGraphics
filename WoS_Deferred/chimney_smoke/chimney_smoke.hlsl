// Chimney smoke: one volumetric plume per draw, marched inside the plume's
// own screen rectangle at the native bloom composite and blended over the
// finished linear HDR scene (ONE, SRCALPHA: rgb = in-scattered light, a = transmittance).
//
// All positions are camera-relative (the eye is the origin), as in the other
// bloom-time passes; Inv maps clip space back to that frame.
sampler2D Depth : register(s0);          // the INTZ the scene was drawn with
float4 Inv[4]      : register(c0);
float4 BaseRadius  : register(c4);       // flue top (relative), radius at the flue
float4 DriftHeight : register(c5);       // horizontal drift at the plume top per metre of height, plume height
float4 Motion      : register(c6);       // rise speed m/s, time s, radius growth per metre, density
float4 SunDir      : register(c7);       // toward the sun, sun valid
float4 SunColour   : register(c8);       // linear HDR, fade
float4 Ambient     : register(c9);       // linear HDR, march steps
float4 Tint        : register(c10);      // smoke albedo, seed
float4 BoxMin      : register(c11);      // plume bounds (relative), unused w
float4 BoxMax      : register(c12);
float4 Shape       : register(c13);      // noise frequency, noise strength, base puff length, phase g

float Hash(float3 p)
{
    p = frac(p * 0.3183099 + 0.1);
    p *= 17.0;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float Noise(float3 x)
{
    float3 i = floor(x), f = frac(x);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(lerp(Hash(i + float3(0, 0, 0)), Hash(i + float3(1, 0, 0)), f.x),
                     lerp(Hash(i + float3(0, 1, 0)), Hash(i + float3(1, 1, 0)), f.x), f.y),
                lerp(lerp(Hash(i + float3(0, 0, 1)), Hash(i + float3(1, 0, 1)), f.x),
                     lerp(Hash(i + float3(0, 1, 1)), Hash(i + float3(1, 1, 1)), f.x), f.y), f.z);
}
float Fbm(float3 p)
{
    return Noise(p) * 0.62 + Noise(p * 2.13 + 7.1) * 0.38;
}
// Centre line of the plume at height h above the flue: it leaves the flue
// vertically and bends downwind as it rises.
float3 Centre(float h)
{
    float k = h / DriftHeight.w;
    return float3(DriftHeight.x, 0, DriftHeight.z) * h * (0.35 + 0.65 * k);
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float z = tex2D(Depth, uv).r;
    float4 q = float4(uv.x * 2 - 1, 1 - uv.y * 2, 1, 1);
    float4 far = float4(dot(Inv[0], q), dot(Inv[1], q), dot(Inv[2], q), dot(Inv[3], q));
    float3 ray = normalize(far.xyz / far.w);
    q.z = min(z, 0.99999);
    float4 hit = float4(dot(Inv[0], q), dot(Inv[1], q), dot(Inv[2], q), dot(Inv[3], q));
    float scene = z >= 0.99999 ? 1e7 : length(hit.xyz / hit.w);

    // Slab test against the plume box; the march never passes the scene surface.
    float3 inv = 1.0 / (abs(ray) > 1e-6 ? ray : 1e-6);
    float3 a = BoxMin.xyz * inv, b = BoxMax.xyz * inv;
    float3 lo = min(a, b), hi = max(a, b);
    float t0 = max(max(max(lo.x, lo.y), lo.z), 0.0);
    float t1 = min(min(min(hi.x, hi.y), hi.z), scene);
    if (t1 <= t0) return float4(0, 0, 0, 1);

    const int steps = int(Ambient.w);
    float dt = (t1 - t0) / steps;
    float jitter = Hash(float3(uv * 4096.0, Tint.w + frac(Motion.y * 7.0)));
    float cosView = dot(ray, SunDir.xyz);
    float g = Shape.w;
    // Henyey-Greenstein, normalised so an isotropic medium gives 1.
    float phase = (1 - g * g) / pow(max(1 + g * g - 2 * g * cosView, 1e-3), 1.5);

    float transmittance = 1.0;
    float3 light = 0;
    [loop] for (int i = 0; i < steps; ++i) {
        float t = t0 + (i + jitter) * dt;
        float3 p = ray * t - BaseRadius.xyz;
        float h = p.y;
        if (h < -0.2 || h > DriftHeight.w) continue;
        float hc = max(h, 0);
        float3 c = Centre(hc);
        float radius = BaseRadius.w + Motion.z * hc;
        // Slow sway of the column, growing with height, so it is not a tube.
        float sway = hc / DriftHeight.w;
        c.x += (Noise(float3(hc * 0.12 - Motion.y * 0.25, Tint.w * 31.0, 0.5)) - 0.5) * radius * 1.4 * sway;
        c.z += (Noise(float3(hc * 0.12 - Motion.y * 0.25, Tint.w * 31.0, 7.5)) - 0.5) * radius * 1.4 * sway;
        float3 r = p - c; r.y = 0;
        float d = length(r) / radius;
        if (d >= 1.0) continue;
        // Noise advected up the plume; its scale grows with the plume.
        float3 np = float3(r.x, hc - Motion.y * Motion.x, r.z) * (Shape.x / (0.5 + 0.35 * radius));
        float n = Fbm(np + float3(0, 0, Tint.w * 13.0));
        float body = 1 - d * d;
        float life = 1 - hc / DriftHeight.w;
        // Dense at the flue, thinning as it widens; the last fifth fades out.
        float density = saturate(body * 1.5 - (1 - n) * Shape.y) * pow(max(life, 0), 0.8) * saturate(life * 5.0)
                      * saturate((h + 0.2) / max(Shape.z, 0.05)) * Motion.w * 2.2 / (0.4 + radius);
        if (density <= 1e-4) continue;
        // Cheap self shadowing: the side of the plume facing the sun is lit,
        // the far side sees the sun through the rest of the column.
        float facing = dot(r / max(length(r), 1e-3), normalize(float3(SunDir.x, 0, SunDir.z) + 1e-4));
        float shadow = exp(-Motion.w * (1.2 - 0.9 * (facing * 0.5 + 0.5)) * (1 - d * 0.5) * 1.6);
        float3 lit = Tint.rgb * (SunColour.rgb * (SunDir.w * shadow * phase) + Ambient.rgb);
        float step = exp(-density * dt);
        light += transmittance * (1 - step) * lit;
        transmittance *= step;
        if (transmittance < 0.01) break;
    }
    float fade = SunColour.w;
    return float4(light * fade, lerp(1.0, transmittance, fade));
}
