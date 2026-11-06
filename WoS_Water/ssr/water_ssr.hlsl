// Shaderpack-style water: screen-space reflections, Fresnel and a sun glint.
//
// Runs once per frame at the native bloom composite, on the finished linear HDR
// scene. Only pixels the water mask claims are written; everything else is
// clipped, so the pass costs nothing where no water is visible.
//
// The mask is drawn by replaying the native water geometry right after the
// engine drew it: RGB = the rippled world normal the native shader lights with
// (* 0.5 + 0.5), A = the camera distance of the water surface. A later draw
// that covers the water (a boat, Spider-Man) changes the depth but not the mask,
// so a pixel whose depth no longer matches that distance is not water any more.
//
// A reflected ray is marched in camera-relative world space and projected every
// step, the same construction the SSGI trace uses. A hit takes the lit colour of
// the pixel it struck. A ray that leaves the screen after crossing open sky takes
// the sky it last crossed, so clouds and the painted panorama appear in the
// water. A ray that finds neither keeps the native environment reflection.
//
// Compile: fxc /T ps_3_0 /E main /O3 /Fo water_ssr.cso water_ssr.hlsl
sampler2D SceneHDR : register(s0);   // copy of the scene before this pass
sampler2D SceneDepth : register(s1); // INTZ the scene was drawn with
sampler2D WaterMask : register(s2);
float4 InvVP[4] : register(c0);      // camera-relative inverse view-projection rows
float4 Geometry : register(c5);      // invSize.xy, maximum ray length, frame index
float4 Look : register(c6);          // strength, ripple strength, F0, roughness
float4 Sun : register(c7);           // direction toward the sun, glint strength (0 = none)
float4 VP[4] : register(c8);         // camera-relative view-projection rows
float4 SunColour : register(c12);    // native sun radiance, debug view

#define STEPS 28
#define REFINE 5

bool Inside(float2 uv)
{
    return all(uv >= 0.0) && all(uv <= 1.0);
}

float RawDepth(float2 uv)
{
    return tex2Dlod(SceneDepth, float4(uv, 0.0, 0.0)).r;
}

// Linear view depth (clip w) of the scene at uv, or 0 for sky.
float SceneW(float2 uv, float z)
{
    float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, z, 1.0);
    float w = dot(InvVP[3], clip);
    return w > 0.00000001 ? 1.0 / w : 0.0;
}

float4 Project(float3 q)
{
    float4 h = float4(q, 1.0);
    return float4(dot(VP[0], h), dot(VP[1], h), dot(VP[2], h), dot(VP[3], h));
}

float2 ScreenUV(float4 clip)
{
    return float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
}

// True where the mask holds water that is still the visible surface at uv.
bool IsWater(float2 uv, float distance)
{
    float recorded = tex2Dlod(WaterMask, float4(uv, 0.0, 0.0)).a;
    return recorded > 0.0 && abs(recorded - distance) <= 0.3 + distance * 0.02;
}

float Noise(float2 pixel)
{
    float2 q = pixel + Geometry.w * 5.588238;
    return frac(52.9829189 * frac(dot(q, float2(0.06711056, 0.00583715))));
}

float EdgeFade(float2 uv)
{
    float2 edge = min(uv, 1.0 - uv);
    return saturate(min(edge.x, edge.y) * 12.0);
}

float4 main(float2 uv : TEXCOORD0, float2 pixel : VPOS) : COLOR0
{
    float4 source = tex2Dlod(SceneHDR, float4(uv, 0.0, 0.0));
    float4 mask = tex2Dlod(WaterMask, float4(uv, 0.0, 0.0));
    clip(mask.a - 0.0001);

    float z = RawDepth(uv);
    clip(0.9999999 - z);
    float4 clipPos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, z, 1.0);
    float4 world = float4(dot(InvVP[0], clipPos), dot(InvVP[1], clipPos),
                          dot(InvVP[2], clipPos), dot(InvVP[3], clipPos));
    clip(world.w - 0.00000001);
    float3 p = world.xyz / world.w;
    float distance = length(p);
    // Something drawn after the water now covers this pixel.
    clip(0.3 + distance * 0.02 - abs(mask.a - distance));

    float3 ripple = mask.rgb * 2.0 - 1.0;
    ripple *= rsqrt(max(dot(ripple, ripple), 0.0001));
    float3 n = normalize(lerp(float3(0.0, 1.0, 0.0), ripple, saturate(Look.y)));
    float3 v = -p / max(distance, 0.0001);
    float nv = saturate(dot(n, v));
    float f0 = saturate(Look.z);
    float fresnel = f0 + (1.0 - f0) * pow(1.0 - nv, 5.0);

    float3 r = reflect(-v, n);
    // A ripple steep enough to reflect into the water still sees the sky side.
    r.y = abs(r.y);
    r = normalize(r);

    float rayLength = max(Geometry.z, 1.0);
    float3 origin = p + n * (0.05 + distance * 0.002);
    float jitter = Noise(pixel);

    float previousT = 0.0;
    float hitT = 0.0;
    float2 hitUV = 0.0, skyUV = 0.0;
    bool hit = false, sky = false, done = false;
    [loop]
    for (int s = 0; s < STEPS && !done; ++s)
    {
        // Quadratic spacing: dense near the surface where contact reflections
        // live, long strides toward the distant skyline.
        float fraction = ((float)s + jitter) / (float)STEPS;
        float t = rayLength * fraction * fraction + 0.1;
        float stepLength = t - previousT;
        float4 c = Project(origin + r * t);
        if (!(c.w > 0.00001)) { done = true; continue; }
        float2 at = ScreenUV(c);
        if (!Inside(at)) { done = true; continue; }
        float sz = RawDepth(at);
        if (sz >= 0.9999999)
        {
            sky = true;
            skyUV = at;
            previousT = t;
            continue;
        }
        float sw = SceneW(at, sz);
        float gap = c.w - sw;
        float thickness = max(0.5, c.w * 0.03) + stepLength * 1.5;
        if (sw > 0.0 && gap > 0.0 && gap < thickness && !IsWater(at, sw))
        {
            // Binary refinement between the last free point and this one.
            float lo = previousT, hi = t;
            [loop]
            for (int k = 0; k < REFINE; ++k)
            {
                float mid = (lo + hi) * 0.5;
                float4 mc = Project(origin + r * mid);
                float2 muv = ScreenUV(mc);
                float mw = SceneW(muv, RawDepth(muv));
                if (mw > 0.0 && mc.w - mw > 0.0) hi = mid; else lo = mid;
            }
            float4 hc = Project(origin + r * hi);
            hitUV = saturate(ScreenUV(hc));
            hitT = hi;
            hit = true;
            done = true;
            continue;
        }
        previousT = t;
    }

    float3 reflection = source.rgb;
    float confidence = 0.0;
    if (hit)
    {
        reflection = tex2Dlod(SceneHDR, float4(hitUV, 0.0, 0.0)).rgb;
        float reach = 1.0 - smoothstep(0.7, 1.0, hitT / rayLength);
        confidence = EdgeFade(hitUV) * reach;
    }
    else if (sky)
    {
        reflection = tex2Dlod(SceneHDR, float4(skyUV, 0.0, 0.0)).rgb;
        confidence = EdgeFade(skyUV);
    }
    reflection = clamp(reflection, 0.0, 64.0);

    float amount = saturate(fresnel * max(Look.x, 0.0)) * confidence;
    float3 colour = lerp(source.rgb, reflection, amount);

    // Sun glint: GGX on the rippled normal, Smith visibility, Schlick Fresnel.
    float3 glint = 0.0;
    if (Sun.w > 0.0)
    {
        float3 l = normalize(Sun.xyz);
        float3 h = normalize(l + v);
        float nl = saturate(dot(n, l)), nh = saturate(dot(n, h)), vh = saturate(dot(v, h));
        float alpha = max(Look.w * Look.w, 0.0004), a2 = alpha * alpha;
        float d = nh * nh * (a2 - 1.0) + 1.0;
        float distribution = a2 / max(3.14159265 * d * d, 0.0000001);
        float visibility = 0.5 / max(nl * sqrt(nv * nv * (1.0 - a2) + a2) + nv * sqrt(nl * nl * (1.0 - a2) + a2), 0.0001);
        float fs = f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);
        glint = SunColour.rgb * (min(distribution * visibility * fs, 48.0) * nl * Sun.w);
    }
    colour += glint;

    if (SunColour.w > 2.5) return float4(confidence, hit ? 1.0 : 0.0, sky ? 1.0 : 0.0, source.a);
    if (SunColour.w > 1.5) return float4(reflection * confidence, source.a);
    if (SunColour.w > 0.5) return float4(n * 0.5 + 0.5, source.a);
    return float4(colour, source.a);
}
