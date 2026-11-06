// Light shafts of local point lights, for the half-resolution volumetric march.
//
// The engine's local lights are points: position, range and RGB, no direction.
// Each one is given a radius R (native range times a user scale) and a colour
// already multiplied by density and strength. The medium around it is lit by
//     f(d) = 1/(1+(d/a)^2) - fR,   fR = 1/(1+(R/a)^2),   for d < R,
// a bright core of radius a that reaches exactly zero at R, so the sphere has
// no visible rim. Along a view ray with closest approach h at ray distance t0,
// with u = t - t0 and k^2 = h^2 + a^2, the clipped chord integrates to
//     a^2/k (atan(u1/k) - atan(u0/k)) - fR (u1 - u0).
//
// The lights that own a cube shadow map this frame are integrated numerically
// instead, with samples equiangular in atan(u/k). That is the same
// substitution, so each sample weighs (a^2 - fR (u^2 + k^2)) / k per radian,
// and with an empty cube the march converges to the closed form: a lamp does
// not change brightness when it gains or loses its shadow map, only its shape.
// Cube addressing below is a copy of LocalDepth in deferred_local_shadow.hlsl
// (FaceMatrix in deferred_local_shadow.h), static caster layer included.
float4 CubeParams : register(c36);        // count, tile edge, emitter radius, strength
float4 CubeTune : register(c37);          // world bias, normal bias, near plane, static layer bound
float4 CubePosition[2] : register(c38);   // camera-relative xyz, 1/range
float4 ShaftParams : register(c40);       // light count, cube samples, core radius, reserved
float4 ShaftPosition[24] : register(c41); // camera-relative xyz, radius
float4 ShaftColour[24] : register(c65);   // premultiplied rgb, cube slot or -1
sampler2D ShaftCube : register(s9);
sampler2D ShaftCubeStatic : register(s10);

float ShaftCubeDepth(float3 direction, float slot, float farPlane)
{
    float3 a = abs(direction);
    float face, x, y, z;
    if (a.x >= a.y && a.x >= a.z) { face = direction.x >= 0 ? 0 : 1; x = direction.x >= 0 ? -direction.z : direction.z; y = direction.y; z = a.x; }
    else if (a.y >= a.z) { face = direction.y >= 0 ? 2 : 3; x = direction.x; y = direction.y >= 0 ? direction.z : -direction.z; z = a.y; }
    else { face = direction.z >= 0 ? 4 : 5; x = direction.z >= 0 ? direction.x : -direction.x; y = direction.y; z = a.z; }
    float2 local = float2(x, -y) / max(z, 1e-8) * .5 + .5;
    local = clamp(local, .5 / CubeParams.y, 1 - .5 / CubeParams.y);
    float tile = slot * 6 + face;
    float2 uv = (float2(fmod(tile, 3), floor(tile / 3)) + local) / float2(3, 4);
    float d = tex2Dlod(ShaftCube, float4(uv, 0, 0)).r;
    float held = tex2Dlod(ShaftCubeStatic, float4(uv, 0, 0)).r;
    d = CubeTune.w > .5 ? min(d, held) : d;
    float axial = CubeTune.z * farPlane / max(farPlane - d * (farPlane - CubeTune.z), 1e-6);
    return axial * length(direction) / max(z, 1e-8);
}
float ShaftCubeVisible(float3 position, float4 cube, float slot, float farPlane)
{
    float3 delta = position - cube.xyz;
    float distance = length(delta);
    // Inside the near plane and beyond the far plane the map holds nothing.
    if (distance <= CubeTune.z * 2 || distance >= farPlane) return 1;
    return ShaftCubeDepth(delta, slot, farPlane) >= distance - CubeTune.x ? 1 : 0;
}
// ray is the unit view direction, rayLength the distance to the surface the
// ray stops at, jitter the per-pixel step offset in [0, 1).
float3 LampShafts(float3 ray, float rayLength, float jitter)
{
    float3 sum = 0;
    float a2 = ShaftParams.z * ShaftParams.z;
    [loop] for (int i = 0; i < 24; ++i)
    {
        if (i >= ShaftParams.x) break;
        float4 L = ShaftPosition[i];
        float4 C = ShaftColour[i];
        float t0 = dot(L.xyz, ray);
        float h2 = max(dot(L.xyz, L.xyz) - t0 * t0, 0);
        float R2 = L.w * L.w;
        float s2 = R2 - h2;
        [branch] if (s2 > 0)
        {
            float s = sqrt(s2);
            // The chord inside the sphere, cut at the eye and at the surface.
            float u0 = max(t0 - s, 0) - t0;
            float u1 = min(t0 + s, rayLength) - t0;
            [branch] if (u1 > u0)
            {
                float k2 = h2 + a2;
                float k = sqrt(k2);
                float fR = a2 / (R2 + a2);
                float th0 = atan(u0 / k);
                float th1 = atan(u1 / k);
                float integral = a2 / k * (th1 - th0) - fR * (u1 - u0);
                [branch] if (C.w > -.5 && ShaftParams.y >= 1)
                {
                    float4 cube = C.w < .5 ? CubePosition[0] : CubePosition[1];
                    float farPlane = min(1 / max(cube.w, 1e-6), 500);
                    float n = ShaftParams.y;
                    float dth = (th1 - th0) / n;
                    float marched = 0;
                    [loop] for (int j = 0; j < 48; ++j)
                    {
                        if (j >= n) break;
                        float u = k * tan(th0 + (j + jitter) * dth);
                        float w = max(a2 - fR * (u * u + k2), 0);
                        marched += w * ShaftCubeVisible(ray * (t0 + u), cube, C.w, farPlane);
                    }
                    integral = marched * dth / k;
                }
                // Attenuation to the eye at the brightest point of the chord;
                // a lamp's glow is a few metres deep, so one point is enough.
                float tc = t0 + clamp(0, u0, u1);
                float eyePath = FogOpticalPath(tc, ray.y);
                float toEye = exp(-Scatter.w * tc - max(Fog.x, 0) * eyePath);
                sum += C.rgb * (max(integral, 0) * toEye);
            }
        }
    }
    return sum;
}
