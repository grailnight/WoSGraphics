// Cloud shadows: the transmittance of the mod's own cloud layer along the ray
// from a receiver toward the sun.
//
// The layer is a procedural field, not a texture, so the deck that is drawn in
// the sky pass can be evaluated again anywhere else in the frame for the cost
// of the noise. The density below is the same function as
// WoS_VolumetricClouds/native/wos_clouds.hlsli, register for register and
// constant for constant: a shadow that did not use the identical field would
// not be the shadow of the cloud the player is looking at.
//
// What differs from that file is only the ray. There the march goes from the
// eye outward and accumulates scattered light; here it goes from the receiver
// toward the sun and accumulates optical depth, which is what the sun has to
// pass through to reach the pixel.
#ifndef WOS_CLOUD_SHADOW_INCLUDED
#define WOS_CLOUD_SHADOW_INCLUDED

// layer bottom, coverage, cloud size, layer opacity  (mirrors gCloudParams)
float4 CloudLayer : register(c32);
// layer thickness, detail, wind phase, reserved      (mirrors gCloudShape)
float4 CloudShape : register(c33);
// camera world origin xyz, march steps; zero steps disables the whole term
float4 CloudMarch : register(c34);
// strength, compensation for the uniform table attenuation, the least sun a
// solid deck lets through, reserved
float4 CloudTune : register(c35);

#include "wos_cloud_field.hlsli"
float WoSCloudShadowDensity(float3 world, float bottom, float top, float3 wind)
{ return WoSSharedCloudDensity(world,bottom,top,wind,CloudLayer,CloudShape); }

// Scale for the direct sun radiance at this receiver.
//
// It is a radiance scale and not a visibility, which is why it may come back
// above one: the light table has already been dimmed by the uniform coupling,
// and a pixel standing in a hole in the deck has to get that back. Handing
// this to the visibility term instead would clamp every hole to whatever the
// table had left and the pattern would disappear into a flat image.
// Position is camera-relative, the frame every deferred pass already works in;
// the layer is authored in absolute world units, so the camera origin the CPU
// publishes in CloudMarch.xyz is what joins the two.
//
// Returns 1 for every case where the layer cannot be between the receiver and
// the sun: no layer this frame, a transparent layer, a sun at or below the
// horizon, and a receiver already above the deck.
float WoSCloudShadow(float3 relativePosition, float3 sunDirection)
{
    float steps = CloudMarch.w;
    if (steps < 0.5)
        return 1.0;
    float opacity = saturate(CloudLayer.w);
    if (opacity <= 0.001)
        return 1.0;

    float bottom = CloudLayer.x;
    float top = bottom + max(CloudShape.x, 10.0);
    float3 world = relativePosition + CloudMarch.xyz;

    // A grazing sun would stretch the column across the whole map and cost a
    // march no step count can resolve; the floor is the same order as the
    // layer's own horizon fade.
    float vertical = sunDirection.y;
    if (vertical <= 0.02)
        return 1.0;
    // Use the same camera-relative horizon dome as the visible layer.
    float curvature = smoothstep(0.0, 400.0, bottom - CloudMarch.y) / 80000.0;
    float quadratic = curvature * dot(sunDirection.xz, sunDirection.xz);
    float2 offset = world.xz - CloudMarch.xz;
    float bentHeight = world.y + curvature * dot(offset, offset);
    float slope = vertical + 2.0 * curvature * dot(offset, sunDirection.xz);
    float enter = max((bottom - bentHeight) / vertical, 0.0);
    float leave = (top - bentHeight) / vertical;
    if (quadratic > 1.0e-10)
    {
        float discriminant = slope*slope + 4.0*quadratic*(top-bentHeight);
        if (discriminant <= 0.0) return 1.0;
        float root = sqrt(discriminant);
        enter = max((-slope-root)/(2.0*quadratic),0.0);
        leave = (-slope+root)/(2.0*quadratic);
        if (bentHeight < bottom)
            enter = WoSCloudDomeIntersection(bottom-bentHeight,slope,quadratic);
    }
    if (leave <= enter) return 1.0;
    float span = min(leave - enter, 20000.0);
    float stepSize = span / steps;
    float3 wind = float3(CloudShape.z, 0.0, CloudShape.z * 0.35);
    float3 p = world + sunDirection * (enter + stepSize * 0.5);

    float optical = 0.0;
    [loop]
    for (int i = 0; i < 16; ++i)
    {
        if (i >= steps)
            break;
        optical += WoSCloudShadowDensity(WoSCloudDomePoint(p,CloudMarch.xyz,curvature), bottom, top, wind);
        p += sunDirection * stepSize;
    }

    // Same extinction coefficient and the same opacity fade as the layer that
    // is drawn, so a deck that hides the sun from the camera hides it from the
    // ground by the same amount.
    float transmittance = exp(-optical * stepSize * 0.02);
    transmittance = lerp(1.0, transmittance, opacity);
    // A single-scattering march drops every photon it removes. Real daylight
    // under a solid deck is mostly light that scattered inside the cloud and
    // carried on downward, so without a floor an overcast city goes black
    // rather than grey and the pattern is lost in the dark. The floor is an
    // artistic stand-in for that light, not a second scattering order.
    transmittance = lerp(saturate(CloudTune.z), 1.0, transmittance);
    // The light table has already been dimmed by the uniform cloud coupling.
    // Undoing that here is what keeps the two features from darkening the sun
    // twice; with compensation at 1 they simply multiply.
    transmittance *= CloudTune.y;
    return lerp(1.0, transmittance, saturate(CloudTune.x));
}

#endif // WOS_CLOUD_SHADOW_INCLUDED
