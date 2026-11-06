// DLAA inputs from the scene INTZ and the engine camera of both frames.
//
// COLOR0 (G16R16F): motion as a UV delta from this pixel's surface point to
// where it was on the previous frame, jitter excluded (DLSS MVJittered = 0;
// the runtime scales it to pixels). Same reprojection as taa_resolve.hlsl,
// without the 3x3 nearest-depth dilation: DLSS dilates on its own.
// COLOR1 (R32F): the hardware depth, 0 = near (DepthInverted = 0).
// The game draws nothing that moves on its own into motion, so characters
// and cars carry the camera's motion only.
//
// Compile: fxc /T ps_3_0 /E main /O3 /Fo dlss_prep.cso dlss_prep.hlsl
sampler2D SceneDepth : register(s0); // point, INTZ
float4 InvVP[4] : register(c0);      // clip -> camera-relative world, rows
float4 PrevVP[4] : register(c4);     // camera-relative world -> previous clip, rows
float4 Metrics : register(c8);       // 1/w, 1/h, w, h
float4 Jitter : register(c9);        // current jitter in clip xy, history valid

struct Output
{
    float4 motion : COLOR0;
    float4 depth : COLOR1;
};

Output main(float2 uv : TEXCOORD0)
{
    Output o;
    float z = tex2Dlod(SceneDepth, float4(uv, 0, 0)).r;
    o.depth = float4(z, 0.0, 0.0, 1.0);
    o.motion = float4(0.0, 0.0, 0.0, 1.0);
    if (Jitter.z < 0.5)
        return o;

    // The pixel shows the surface point at its own position minus the jitter.
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0) - Jitter.xy;
    float4 clip = float4(ndc, min(z, 0.9999999), 1.0);
    float4 world = float4(dot(InvVP[0], clip), dot(InvVP[1], clip), dot(InvVP[2], clip), dot(InvVP[3], clip));
    if (!(abs(world.w) > 1e-12))
        return o;
    float4 p = float4(world.xyz / world.w, 1.0);
    float4 previous = float4(dot(PrevVP[0], p), dot(PrevVP[1], p), dot(PrevVP[2], p), dot(PrevVP[3], p));
    if (!(previous.w > 1e-8))
        return o;
    float2 previousNdc = previous.xy / previous.w;
    float2 previousUV = float2(previousNdc.x * 0.5 + 0.5, 0.5 - previousNdc.y * 0.5);
    float2 currentUV = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    o.motion = float4(previousUV - currentUV, 0.0, 1.0);
    return o;
}
