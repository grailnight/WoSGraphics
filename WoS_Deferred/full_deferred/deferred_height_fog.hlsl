// World Y is up. Density is constant below the base and falls exponentially
// above it. Integrate the actual clipped ray, including base-plane crossings.
float4 FogHeight : register(c34); // camera Y - base Y, falloff, reserved, reserved

float FogOpticalPath(float distance, float rayY)
{
    float path = max(min(distance, Fog.z) - Fog.y, 0);
    if (FogHeight.y <= 0 || path <= 0) return path;
    float h0 = FogHeight.x + rayY * Fog.y;
    float h1 = h0 + rayY * path;
    float low = min(h0, h1), high = max(h0, h1);
    float below = saturate(-low / max(high - low, 1e-6));
    float delta = FogHeight.y * (max(high, 0) - max(low, 0));
    // Stable at the horizon; no subtraction of nearly equal exponentials.
    float average = delta < .01 ? 1 - delta * .5 + delta * delta / 6
                               : (1 - exp(-delta)) / max(delta, 1e-6);
    average *= exp(-FogHeight.y * max(low, 0));
    return path * (below + (1 - below) * average);
}
