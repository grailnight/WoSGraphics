#ifndef WOS_SHARED_CLOUD_FIELD
#define WOS_SHARED_CLOUD_FIELD
float WoSCloudHash(float3 p)
{
    p = frac(p * 0.3183099 + float3(0.71, 0.113, 0.419));
    p *= 17.0;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float WoSCloudNoise(float3 x)
{
    float3 cell = floor(x);
    float3 f = frac(x);
    f = f * f * (3.0 - 2.0 * f);

    float n000 = WoSCloudHash(cell);
    float n100 = WoSCloudHash(cell + float3(1.0, 0.0, 0.0));
    float n010 = WoSCloudHash(cell + float3(0.0, 1.0, 0.0));
    float n110 = WoSCloudHash(cell + float3(1.0, 1.0, 0.0));
    float n001 = WoSCloudHash(cell + float3(0.0, 0.0, 1.0));
    float n101 = WoSCloudHash(cell + float3(1.0, 0.0, 1.0));
    float n011 = WoSCloudHash(cell + float3(0.0, 1.0, 1.0));
    float n111 = WoSCloudHash(cell + float3(1.0, 1.0, 1.0));

    float x00 = lerp(n000, n100, f.x);
    float x10 = lerp(n010, n110, f.x);
    float x01 = lerp(n001, n101, f.x);
    float x11 = lerp(n011, n111, f.x);
    return lerp(lerp(x00, x10, f.y), lerp(x01, x11, f.y), f.z);
}

float WoSSharedCloudDensity(float3 world, float bottom, float top, float3 wind, float4 layer, float4 shapeParams)
{
    float height = saturate((world.y - bottom) / max(top - bottom, 1.0));
    // Original WoS implementation, visually inspired by Bliss; no Bliss code.
    // A low flat base and taller rounded crown leave room for billowing edges.
    float profile = smoothstep(0.0, 0.12, height) * (1.0 - smoothstep(0.48, 1.0, height));
    if (profile <= 0.002)
        return 0.0;

    float3 q = world / max(layer.z, 20.0) + wind;
    // Separate broad weather patches from smaller three-dimensional lobes.
    float weather = WoSCloudNoise(float3(q.x * 0.27, 0.37, q.z * 0.27));
    float body = WoSCloudNoise(q * float3(0.85, 0.65, 0.85)) * 0.7
               + WoSCloudNoise(q * 1.91 + 13.1) * 0.3;
    float coverage = saturate(layer.y);
    float mass = saturate((body + weather * 0.3 - (0.94 - coverage * 0.72)) * 2.8);
    float erosion = (1.0 - WoSCloudNoise(q * 4.3 + 27.7)) * 0.20 * saturate(shapeParams.y);
    return saturate((mass * profile - erosion * (1.0 - mass * 0.75)) * 1.7) * smoothstep(0.0, 0.08, coverage);
}

// Bend only the distant deck around the viewer. Noise remains anchored in
// world XZ; curvature vanishes smoothly when approaching/entering the layer.
float3 WoSCloudDomePoint(float3 p, float3 origin, float curvature)
{
    float2 offset = p.xz - origin.xz;
    p.y += curvature * dot(offset, offset);
    return p;
}

// Positive exit of a*t*t + vertical*t = height, for a camera below the deck.
float WoSCloudDomeIntersection(float height, float vertical, float a)
{
    float root = sqrt(vertical * vertical + 4.0 * a * height);
    if (vertical >= 0.0)
        return 2.0 * height / max(root + vertical, 1.0e-8);
    return (root - vertical) / max(2.0 * a, 1.0e-8);
}


#endif
