#include "pbr_legacy_bridge.hlsl"
#include "deferred_brdf.hlsl"
#include "deferred_cloud_shadow.hlsl"
// Geometry data only: no lit scene-color subtraction or reconstructed albedo.
sampler2D Residual : register(s0);
sampler2D NormalShadowSet : register(s1);
sampler2D DiffuseFog : register(s2);
sampler2D SpecularPower : register(s3);
sampler2D SceneDepth : register(s4);
sampler2D LightSets : register(s5);
sampler2D NativeScene : register(s6);
// Four sun-shadow cascades packed as 2x2 tiles of one INTZ atlas: one sampler,
// so a tap costs no branch over four maps.
sampler2D CascadeAtlas : register(s7);
// Ambient visibility at full resolution, already upsampled. It multiplies the
// residual and nothing else: the residual is what the native material produced
// once the direct sun and point lights were taken out of it, which is the
// engine's own environment sample plus emissive and fog. Occlusion belongs to
// that term and to no other, and here it is still a separate quantity, so no
// heuristic is needed to keep it off sunlit surfaces.
sampler2D AmbientVisibility : register(s8);
float4 InverseVP[4] : register(c0);
// pixel size xy, light table height z, debug mode w
float4 Screen : register(c4);
// direct intensity, specular intensity, contact length (0 = off), contact thickness
float4 Settings : register(c5);
// contact steps, contact strength, contact normal bias, reserved
float4 Contact : register(c6);
// Camera-relative forward view-projection, dot-product rows like InverseVP.
float4 ForwardVP[4] : register(c7);
// Per cascade three camera-relative dot-product rows; the cascades are
// orthographic, so the fourth row would always be (0,0,0,1).
float4 CascadeRow[12] : register(c11);
float4 CascadeTexel : register(c23);   // world units per shadow texel
float4 CascadeRange : register(c24);   // world units per unit of cascade clip depth
float4 CascadeUv : register(c25);      // atlas UV per world unit
// sun angular size (penumbra width per unit blocker distance), longest blocker
// distance searched, shadow strength, mode (0 off, 1 combine, 2 replace)
float4 SunShadow : register(c26);
// depth bias in world units, normal bias in texels, half an atlas texel,
// cascade boundary dither band in clip units
float4 ShadowBias : register(c27);
// x = ambient occlusion strength, 0 disables the term exactly.
float4 Ambient : register(c28);

// Vogel disks: even coverage at every tap count, and rotating one per pixel
// turns banding into noise that the penumbra width already hides.
static const float2 BlockerDisk[12] = {
    float2(0.2041,0.0000), float2(-0.2607,0.2388), float2(0.0399,-0.4547), float2(0.3286,0.4286),
    float2(-0.6030,-0.1067), float2(0.5712,-0.3634), float2(-0.1911,0.7107), float2(-0.3644,-0.7016),
    float2(0.7906,0.2887), float2(-0.8224,0.3395), float2(0.3965,-0.8472), float2(0.2930,0.9341)
};
static const float2 FilterDisk[16] = {
    float2(0.1768,0.0000), float2(-0.2258,0.2068), float2(0.0346,-0.3938), float2(0.2846,0.3712),
    float2(-0.5222,-0.0924), float2(0.4947,-0.3147), float2(-0.1655,0.6155), float2(-0.3156,-0.6076),
    float2(0.6846,0.2500), float2(-0.7123,0.2940), float2(0.3434,-0.7337), float2(0.2537,0.8089),
    float2(-0.7647,-0.4432), float2(0.8971,-0.1972), float2(-0.5475,0.7788), float2(-0.1265,-0.9761)
};

float3 DecodeNormal(float2 packed)
{
    float2 f = packed * 2 - 1;
    float3 n = float3(f, 1 - abs(f.x) - abs(f.y));
    float t = saturate(-n.z);
    n.xy += float2(n.x >= 0 ? -t : t, n.y >= 0 ? -t : t);
    return normalize(n);
}
float4 Light(float index, float row)
{
    return tex2Dlod(LightSets, float4((index + .5) / 32, (row + .5) / Screen.z, 0, 0));
}
float3 Evaluate(float3 n, float3 v, float3 l, float3 radiance, float3 rho, float3 specular, float power)
{
    return WoSPbrLegacyDirect(n,v,l,radiance,rho,specular,power,Settings.y);
}
float Hash(float2 uv)
{
    float2 pixel = uv / Screen.xy;
    return frac(52.9829189 * frac(0.06711056 * pixel.x + 0.00583715 * pixel.y));
}
// Percentage-closer soft shadows for one cascade. Blocker search first, then a
// filter whose radius is the penumbra the blockers actually cast: with a
// directional light the penumbra grows with the distance between blocker and
// receiver, both of which are linear world distances here because every cascade
// is orthographic. The result is that the same world penumbra comes out of every
// cascade, so crossing a cascade boundary changes only the sampling density.
float PcssVisibility(float2 uv, float2 tileMin, float receiver, float texel, float range,
                     float uvScale, float2 rotation)
{
    float2 low = tileMin + ShadowBias.z;
    float2 high = tileMin + .5 - ShadowBias.z;
    float bias = ShadowBias.x / max(range, 1e-6);
    float searchWorld = max(SunShadow.x * SunShadow.y, texel);
    float searchUv = searchWorld * uvScale;
    float blockerSum = 0;
    float blockerCount = 0;
    [unroll] for (int i = 0; i < 12; ++i)
    {
        float2 disk = BlockerDisk[i];
        float2 offset = float2(disk.x * rotation.x - disk.y * rotation.y,
                               disk.x * rotation.y + disk.y * rotation.x) * searchUv;
        float sampled = tex2Dlod(CascadeAtlas, float4(clamp(uv + offset, low, high), 0, 0)).r;
        float hit = step(sampled, receiver - bias);
        blockerSum += sampled * hit;
        blockerCount += hit;
    }
    if (blockerCount < .5) return 1;
    float blocker = blockerSum / blockerCount;
    float penumbra = SunShadow.x * max(receiver - blocker, 0) * range;
    float radius = clamp(penumbra, texel, searchWorld) * uvScale;
    float lit = 0;
    [unroll] for (int j = 0; j < 16; ++j)
    {
        float2 disk = FilterDisk[j];
        float2 offset = float2(disk.x * rotation.x - disk.y * rotation.y,
                               disk.x * rotation.y + disk.y * rotation.x) * radius;
        float sampled = tex2Dlod(CascadeAtlas, float4(clamp(uv + offset, low, high), 0, 0)).r;
        lit += 1 - step(sampled, receiver - bias);
    }
    return lit * .0625;
}
// Select the tightest cascade containing the receiver, then filter it.
// Returns visibility, whether any cascade covered the pixel, and its index.
// The containment margin is shrunk by a dithered band so that the boundary
// between two cascades dissolves instead of drawing a line across the ground.
#define CASCADE_TEST(index) \
    if (found < .5) \
    { \
        float4 rx = CascadeRow[index * 3 + 0]; \
        float4 ry = CascadeRow[index * 3 + 1]; \
        float4 rz = CascadeRow[index * 3 + 2]; \
        float3 c = float3(dot(rx, p4), dot(ry, p4), dot(rz, p4)); \
        float margin = 4 * max(SunShadow.x * SunShadow.y, CascadeTexel[index]) * CascadeUv[index]; \
        if (max(abs(c.x), abs(c.y)) < 1 - margin - dither && c.z > 0 && c.z < 1) \
        { \
            found = 1; \
            rowX = rx; rowY = ry; rowZ = rz; \
            tileMin = float2((index & 1) * .5, (index >> 1) * .5); \
            texel = CascadeTexel[index]; \
            range = CascadeRange[index]; \
            uvScale = CascadeUv[index]; \
        } \
    }
float3 CascadeShadow(float3 position, float3 n, float2 screenUv)
{
    float4 p4 = float4(position, 1);
    float hash = Hash(screenUv);
    float dither = ShadowBias.w * hash;
    float found = 0;
    float4 rowX = 0, rowY = 0, rowZ = 0;
    float2 tileMin = 0;
    float texel = 0, range = 0, uvScale = 0;
    CASCADE_TEST(0)
    CASCADE_TEST(1)
    CASCADE_TEST(2)
    CASCADE_TEST(3)
    if (found < .5) return float3(1, 0, -1);
    // Normal offset before projection: it removes the acne that a pure depth
    // bias only trades for peter-panning, and it scales with the cascade texel.
    float4 biased = float4(position + n * (ShadowBias.y * texel), 1);
    float3 shadow = float3(dot(rowX, biased), dot(rowY, biased), dot(rowZ, biased));
    float2 local = float2(shadow.x * .5 + .5, .5 - shadow.y * .5);
    float2 atlasUv = tileMin + local * .5;
    float angle = hash * 6.2831853;
    float2 rotation = float2(cos(angle), sin(angle));
    float visibility = PcssVisibility(atlasUv, tileMin, saturate(shadow.z), texel, range, uvScale, rotation);
    return float3(saturate(1 - SunShadow.z * (1 - visibility)), 1, tileMin.x * 2 + tileMin.y * 4);
}
// Screen-space contact shadows, second design. A ray toward the sun is marched
// through the depth buffer to catch occluders the shadow maps cannot resolve:
// the gap under a foot or a car, creases in clothing, the base of a lamp post.
// Returns 1 when lit.
//
// What changed from the first design, and why:
// - The ray is measured on screen, not only in the world. Its world length is
//   capped so that it never covers more than ContactTune.x pixels. Near the
//   camera a fixed world length spread 16 steps over hundreds of pixels and
//   skipped the thin occluders it was meant to find; far away it spent all of
//   them inside one pixel. Now every step lands at a similar pixel spacing.
// - Thickness grows with view depth (ContactTune.z per view unit). A constant
//   one either let a distant pole cast a long false shadow over the ground far
//   behind it, or missed real occluders up close.
// - The pixel the ray starts from is skipped: it holds the receiver itself.
// - Occlusion weakens with distance to the blocker (ContactTune.y), which is
//   the penumbra growing, so a shadow hardens where it touches its caster.
// - The noise moves every frame (ContactTune.w) and TAA averages it, instead of
//   a fixed per-pixel pattern that shows as a grain on every shadow edge.
// Occlusion stays continuous, never a first-hit boolean, and the march keeps
// the largest contribution; every pixel runs the same number of steps.
//
// x max screen length in pixels (0 disables), y softness, z thickness per view
// unit, w noise frame (0 = static noise).
float4 ContactTune : register(c30);
float4 ContactClip(float3 p)
{
    float4 p4 = float4(p, 1);
    return float4(dot(ForwardVP[0], p4), dot(ForwardVP[1], p4), dot(ForwardVP[2], p4), dot(ForwardVP[3], p4));
}
float ContactShadow(float3 position, float3 n, float3 l, float2 uv, float originW)
{
    float4 origin = ContactClip(position);
    if (origin.w <= 1e-6) return 1;
    // View depth of any depth sample = k / dot(InverseVP[3], ndc); k from the origin pixel.
    float k = origin.w * originW;
    float3 start = position + n * Contact.z;
    float4 c0 = ContactClip(start);
    float4 c1 = ContactClip(start + l * Settings.z);
    // A sun behind the camera: stop the ray before it reaches the near plane.
    float minW = .25 * c0.w;
    if (c1.w < minW) c1 = lerp(c0, c1, (c0.w - minW) / max(c0.w - c1.w, 1e-6));
    float2 pixels = (c1.xy / c1.w - c0.xy / c0.w) * .5 / Screen.xy;
    float span = length(pixels);
    if (span < 1) return 1;
    // Clip coordinates are affine in the world, so shortening the ray is a lerp.
    float fraction = min(1, ContactTune.x / span);
    c1 = lerp(c0, c1, fraction);
    span *= fraction;

    float2 pixel = uv / Screen.xy + ContactTune.w * 5.588238;
    float jitter = frac(52.9829189 * frac(.06711056 * pixel.x + .00583715 * pixel.y));
    float steps = max(Contact.x, 1);
    float occlusion = 0;
    [loop] for (int i = 0; i < 32; ++i)
    {
        if (i >= steps) break;
        float f = (i + jitter) / steps;
        float4 clip = lerp(c0, c1, f);
        float2 ndc = clip.xy / clip.w;
        float2 sampleUv = float2(ndc.x * .5 + .5, .5 - ndc.y * .5);
        if (any(sampleUv < 0) || any(sampleUv > 1)) break;
        float sampled = tex2Dlod(SceneDepth, float4(sampleUv, 0, 0)).r;
        float sampleW = dot(InverseVP[3], float4(ndc, sampled, 1));
        float sceneDepth = k / (abs(sampleW) < 1e-8 ? 1e-8 : sampleW);
        float behind = clip.w - sceneDepth;
        float bias = clip.w * 2e-3;
        float thickness = Settings.w + clip.w * ContactTune.z;
        float entering = saturate((behind - bias) / bias);
        float leaving = saturate((thickness - behind) / (.5 * thickness));
        // The first pixel is the receiver itself.
        float own = f * span >= 1.5 ? 1 : 0;
        occlusion = max(occlusion, own * entering * leaving * saturate(1 - ContactTune.y * f));
    }
    return 1 - saturate(Contact.y * occlusion);
}
#include "deferred_local_shadow.hlsl"
#include "wet_surface.hlsl"
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 ns = tex2D(NormalShadowSet, uv);
    float id = floor(ns.a + .5);
    clip(id - .5);
    clip(Screen.z - .5 - id);
    float4 base = tex2D(Residual, uv);
    base.a = tex2D(NativeScene, uv).a;
    float4 rf = tex2D(DiffuseFog, uv);
    float4 sp = tex2D(SpecularPower, uv);
    float depth = tex2D(SceneDepth, uv).r;
    float4 p = float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1);
    float4 world = float4(dot(InverseVP[0], p), dot(InverseVP[1], p), dot(InverseVP[2], p), dot(InverseVP[3], p));
    clip(abs(world.w) - 1e-8);
    float3 position = world.xyz / world.w;
    float3 v = -position * rsqrt(max(dot(position, position), 1e-12));
    float3 n = DecodeNormal(ns.xy);
    float2 rain=RainMask(position+RainOrigin.xyz,n,Light(31,id).x);
    if(rain.x>0) {
        rf.rgb *= lerp(1,.72,rain.x)*lerp(1,.62,rain.y);
        sp.rgb = lerp(sp.rgb,max(sp.rgb,.28),rain.x);
        sp.a = lerp(sp.a,max(sp.a,96),rain.x);
        n=RainNormal(position+RainOrigin.xyz,n,rain.y);
    }
    // Material class of the draw that wrote this pixel; zero where unknown.
    float materialClass = Light(31, id).y;
    if (abs(materialClass - 12) < .5) materialClass = WoSFacadeClass(rf.rgb, sp.rgb);
    WoSPbrSetClass(materialClass, rain.x);
    // After the rain normal, so ripples are filtered with the rest.
    WoSSpecularAASetup(n, position);

    float4 dir = Light(0, id);
    float4 sun = Light(1, id);
    float dl = dot(dir.xyz, dir.xyz);
    float3 direct = 0;
    float contact = 1;
    float cloud = 1;
    float3 cascade = float3(1, 0, -1);
    if (sun.w > .5)
        direct = max(rf.rgb, 0) * sun.rgb * saturate(ns.z);
    else if (dl > 1e-12)
    {
        float3 l = dir.xyz * rsqrt(dl);
        float sunVisibility = abs(SunShadow.w) > 1.5 ? 1 : saturate(ns.z);
        // Nothing to gain from a shadow lookup where the surface faces away
        // from the sun, or where the native term is already black and the mode
        // can only darken further.
        float needCascade = SunShadow.w > .5 && dot(n, l) > 0 &&
                            (SunShadow.w > 1.5 || sunVisibility > .002);
        if (needCascade)
        {
            cascade = CascadeShadow(position, n, uv);
            // Mode 2 lets a covered pixel take the cascade term alone; mode 1
            // combines by min, so cascades can only add shadow where the maps
            // hold a caster the native pass missed.
            sunVisibility = SunShadow.w > 1.5 ? cascade.x : min(sunVisibility, cascade.x);
        }
        // Combined by min, so a shadow-map penumbra that is already darker
        // keeps its own softness; a pixel the sun has already lost costs no march.
        if (Settings.z > 0 && ContactTune.x > 0 && sunVisibility > .02)
        {
            contact = ContactShadow(position, n, l, uv, world.w);
            sunVisibility = min(sunVisibility, contact);
        }
        // The cloud deck attenuates the sun before any ground geometry can
        // shadow it, so it scales the radiance rather than the visibility:
        // one is how much light left the sun, the other is how much of the
        // sky disc a building is standing in front of. Keeping it out of
        // sunVisibility also keeps it out of the contact-shadow gate, and
        // lets a pixel under a hole in the deck exceed what the uniform
        // coupling left in the table. A pixel the sun has already lost to
        // geometry costs no march.
        if (CloudMarch.w > .5 && (sunVisibility > .002 || Screen.w > 6.5))
            cloud = WoSCloudShadow(position, l);
        direct = Evaluate(n, v, l, sun.rgb * (cloud * sunVisibility), rf.rgb, sp.rgb, sp.a);
        // RainCoat returns zero without a puddle, but fxc flattens that early
        // return, so a dry pixel paid the whole lobe for the sun and each light.
        [branch] if (rain.y > 0)
            direct += RainCoat(n,v,l,sun.rgb*(cloud*sunVisibility),rain.y);
    }
    [loop] for (int i = 0; i < 8; ++i)
    {
        if (i >= dir.w) break;
        float4 lightPosition = Light(2 + i * 2, id);
        float3 delta = lightPosition.xyz - position;
        float distance2 = dot(delta, delta);
        float distance = sqrt(max(distance2, 0));
        // Soft lamp pools (Proper Shaders style): Ambient.z blends the native
        // linear falloff a into smoothstep(a) = a + a(1-a)(2a-1); zero keeps it
        // bit for bit. Radius stretch and lamp dimming are applied on the CPU to
        // the light table (the resolve has no temp register to spare).
        float attenuation = 1 - saturate(distance * max(lightPosition.w, 0));
        attenuation *= 1 + Ambient.z * (1 - attenuation) * (2 * attenuation - 1);
        float3 l = delta * rsqrt(max(distance2, 1e-12));
        float3 radiance = Light(3 + i * 2, id).rgb * attenuation;
        float visibility = 1;
        if (dot(n, l) > 0 && max(radiance.r, max(radiance.g, radiance.b)) > .001)
            visibility = LocalMapShadow(position, n, lightPosition, uv);
        // Out of range, the lobe exits early inside Evaluate (zero radiance).
        // Contact.w softens lamp highlights only; the sun keeps Settings.y.
        direct += WoSPbrLegacyDirect(n, v, l, radiance * visibility, rf.rgb, sp.rgb, sp.a, Settings.y * (1 - Contact.w));
        [branch] if (rain.y > 0)
            direct += RainCoat(n,v,l,radiance*visibility,rain.y);
    }
    direct *= max(Settings.x, 0) * saturate(rf.a);
    if (Screen.w > 6.5) return float4(cloud.xxx, base.a);
    if (Screen.w > 5.5)
    {
        float3 tint = cascade.z < -.5 ? float3(.15, .15, .15) :
                      (cascade.z < .5 ? float3(.9, .25, .25) :
                      (cascade.z < 1.5 ? float3(.25, .9, .25) :
                      (cascade.z < 2.5 ? float3(.25, .45, .95) : float3(.9, .85, .25))));
        return float4(tint * lerp(.35, 1, cascade.x), base.a);
    }
    if (Screen.w > 4.5) return float4(cascade.xxx, base.a);
    if (Screen.w > 3.5) return float4(contact.xxx, base.a);
    if (Screen.w > 2.5) return float4(n * .5 + .5, base.a);
    if (Screen.w > 1.5) return float4(.1, .8, .35, base.a);
    // Written as a deficit so that a strength of zero, or a fully open pixel,
    // reproduces the previous output bit for bit rather than multiplying by a
    // rounded one.
    float visibility = 1.0 - (1.0 - tex2D(AmbientVisibility, uv).r) * saturate(Ambient.x);
    if (Screen.w > 7.5) return float4(visibility.xxx, base.a);
    if (Screen.w > .5) return float4(direct, base.a);
    float3 result=base.rgb * visibility + direct;
    if(rain.y>.001 && RainReflection.y>0) {
        float4 reflection=RainSSR(position,n,v);
        float fresnel=.02+.98*pow(1-saturate(dot(n,v)),5);
        result=lerp(result,reflection.rgb,saturate(rain.y*fresnel*RainReflection.y*reflection.a)*saturate(rf.a));
    }
    return float4(result,base.a);
}
