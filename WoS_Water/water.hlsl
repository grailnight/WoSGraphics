// D3D9 / SM3. Linear HDR inputs; world Y is up.
// Explicit water geometry only. Not a full-screen scene filter.
float4x4 WorldViewProjection : register(c0);
float4x4 World : register(c4);
float4 EyeTime : register(c8);
// wave slope, spatial frequency, speed, roughness
float4 Waves : register(c9);
// direction toward sun; W = visibility (shadow factor)
float4 SunDirection : register(c10);
float4 SunRadiance : register(c11);
// RGB absorption per world unit; W caustics intensity
float4 Absorption : register(c12);
// RGB deep-water scattering; W maximum caustics depth
float4 DeepColor : register(c13);
// XY screen texel size; Z refraction displacement; W caustics frequency
float4 Screen : register(c14);
sampler2D Scene : register(s0); // scene copy BEFORE drawing water
samplerCUBE Environment : register(s1); // linear reflection cubemap
sampler2D Thickness : register(s2); // positive water thickness in world units

struct Input { float4 position:POSITION0; };
struct Varying { float4 position:POSITION0; float3 world:TEXCOORD0; float4 clip:TEXCOORD1; };
Varying WaterVS(Input i) {
    Varying o;
    o.world=mul(i.position,World).xyz;
    o.position=mul(i.position,WorldViewProjection);
    o.clip=o.position;
    return o;
}
float3 Unit(float3 v) { return v*rsqrt(max(dot(v,v),1e-8)); }
float2 Slope(float2 p,float t) {
    float2 a=float2(.94,.342),b=float2(-.39,.921),c=float2(.707,-.707);
    return a*cos(dot(p,a)+t)*.57 + b*cos(dot(p,b)*1.73-t*.83)*.28
         + c*cos(dot(p,c)*3.11+t*1.27)*.15;
}
float3 Normal(float2 p) {
    float2 q=p*max(Waves.y,.001);
    // Fade unresolved wave detail to suppress distant sparkle.
    float footprint=max(length(ddx(q)),length(ddy(q)));
    float2 slope=Slope(q,EyeTime.w*Waves.z)*max(Waves.x,0)/(1+footprint*footprint*4);
    return Unit(float3(-slope.x,1,-slope.y));
}
float CausticPattern(float2 p,float depth) {
    float2 q=p*max(Screen.w,.001);
    float t=EyeTime.w*Waves.z;
    q+=Slope(q*.61,t*.57)*.65;
    float a=sin(q.x+t*.42)+sin(q.y*1.13-t*.31);
    float b=sin(q.x*.73-q.y*.84-t*.37)+sin(q.y*.68+q.x*.81+t*.29);
    float aa=max(fwidth(a),.025),bb=max(fwidth(b),.025);
    float pattern=(1-smoothstep(.07,.07+aa,abs(a)))*(1-smoothstep(.16,.16+bb,abs(b)));
    return pattern*exp(-max(depth,0)*.22)*saturate(1-depth/max(DeepColor.w,.001));
}
float4 WaterSurface(Varying i):COLOR0 {
    float2 uv=i.clip.xy/max(i.clip.w,1e-6)*float2(.5,-.5)+.5;
    uv+=Screen.xy*.5;
    float depth=max(tex2D(Thickness,uv).r,0);
    float3 n=Normal(i.world.xz),v=Unit(EyeTime.xyz-i.world),l=Unit(SunDirection.xyz);
    float nv=saturate(dot(n,v)),nl=saturate(dot(n,l));
    float fresnel=.02037+.97963*pow(1-nv,5);
    float2 displaced=clamp(uv+n.xz*Screen.z*saturate(depth),Screen.xy*.5,1-Screen.xy*.5);
    // Reject distortion across foreground/shore boundaries.
    float valid=smoothstep(0,.25,max(tex2D(Thickness,displaced).r,0));
    displaced=lerp(uv,displaced,valid);
    float3 transmitted=tex2D(Scene,displaced).rgb;
    float3 attenuation=exp(-max(Absorption.rgb,0)*depth);
    float caustic=CausticPattern(i.world.xz,depth)*max(Absorption.w,0)*saturate(SunDirection.w)*nl;
    transmitted=transmitted*attenuation*(1+caustic)+DeepColor.rgb*(1-attenuation);
    float3 reflection=texCUBE(Environment,reflect(-v,n)).rgb;
    float3 h=Unit(v+l);
    float nh=saturate(dot(n,h)),vh=saturate(dot(v,h));
    float rough=clamp(Waves.w,.08,1),alpha=rough*rough,a2=alpha*alpha;
    float d=nh*nh*(a2-1)+1;
    float distribution=a2/max(3.14159265*d*d,1e-7);
    float visibility=.5/max(nl*sqrt(nv*nv*(1-a2)+a2)+nv*sqrt(nl*nl*(1-a2)+a2),1e-4);
    float f=.02037+.97963*pow(1-vh,5);
    float3 sun=SunRadiance.rgb*saturate(SunDirection.w)*nl*min(distribution*visibility*f,16);
    return float4(lerp(transmitted,reflection,fresnel)+sun,1);
}
// Receiver geometry: pass underwater WORLD position, depth in TEXCOORD1,
// and a validated water-volume membership/shadow mask in TEXCOORD2.
float4 WaterCaustics(float3 world:TEXCOORD0,float depth:TEXCOORD1,float mask:TEXCOORD2):COLOR0 {
    float c=CausticPattern(world.xz,depth)*max(Absorption.w,0)*saturate(mask);
    c*=step(0,depth)*saturate(SunDirection.w)*saturate(Unit(SunDirection.xyz).y);
    return float4(SunRadiance.rgb*c*exp(-max(Absorption.rgb,0)*max(depth,0)),0);
}
