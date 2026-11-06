// Street steam (SM3 port): camera-facing quads built on the CPU in the
// camera-relative frame (the eye is the origin, as in the other bloom-time
// passes). This only projects them and hands the view depth to the pixel shader.
float4 ViewProj[4] : register(c0);   // camera-relative world -> clip, rows
float4 Forward     : register(c4);   // view direction, unit length

struct In  { float3 position : POSITION; float4 data : TEXCOORD0; };          // u, v, alpha, grey
struct Out { float4 position : POSITION; float4 data : TEXCOORD0; float grey : TEXCOORD1; };  // u, v, alpha, view depth

Out main(In i)
{
    Out o;
    float4 p = float4(i.position, 1);
    o.position = float4(dot(ViewProj[0], p), dot(ViewProj[1], p), dot(ViewProj[2], p), dot(ViewProj[3], p));
    o.data = float4(i.data.xyz, dot(i.position, Forward.xyz));
    o.grey = i.data.w;
    return o;
}
