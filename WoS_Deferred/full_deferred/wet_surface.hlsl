// Native D3D9 wet surfaces. World Y is up. No ReShade inputs.
// Controls: enabled, wetness, puddle coverage, ripple strength.
float4 RainSurface : register(c40);
// Absolute camera origin and animation time in seconds.
float4 RainOrigin : register(c41);
// Puddle size in world units, SSR strength, ray distance, thickness.
float4 RainReflection : register(c42);

float RainHash(float2 p) { return frac(sin(dot(p,float2(127.1,311.7)))*43758.5453); }
float RainNoise(float2 p) {
    float2 i=floor(p),f=frac(p); f=f*f*(3-2*f);
    return lerp(lerp(RainHash(i),RainHash(i+float2(1,0)),f.x),
                lerp(RainHash(i+float2(0,1)),RainHash(i+1),f.x),f.y);
}
// Class 1 = explicit road shader family. Class 2 = body material family.
// Unknown, generic, grass, facade and skin/subsurface are deliberately excluded.
float2 RainMask(float3 absolutePosition,float3 normal,float materialClass) {
    if(RainSurface.x<.5 || RainSurface.y<=0) return 0;
    float road=1-step(.25,abs(materialClass-1));
    float body=1-step(.25,abs(materialClass-2));
    float wet=saturate(RainSurface.y)*saturate(road+body);
    // Normal maps perturb an otherwise flat road; don't reject its bumps.
    float flat=smoothstep(.65,.92,normal.y);
    float2 p=absolutePosition.xz/max(RainReflection.x,.2);
    float field=.7*RainNoise(p)+.3*RainNoise(p*2.71+17.3);
    float threshold=lerp(1.05,.25,saturate(RainSurface.z));
    float puddle=smoothstep(threshold,threshold+.09,field)*flat*road*wet;
    return float2(wet,puddle);
}
float3 RainCoat(float3 n,float3 v,float3 l,float3 radiance,float puddle) {
    float3 h=v+l;float hh=dot(h,h);
    if(hh<1e-8||puddle<=0)return 0;
    h*=rsqrt(hh);
    float nv=saturate(dot(n,v)),nl=saturate(dot(n,l));
    float nh=saturate(dot(n,h)),vh=saturate(dot(v,h));
    float alpha=.075, a2=alpha*alpha;
    float d=a2/(3.14159265*pow(nh*nh*(a2-1)+1,2));
    float visibility=.5/max(nl*sqrt(nv*nv*(1-a2)+a2)+nv*sqrt(nl*nl*(1-a2)+a2),.0001);
    float f=.02+.98*pow(1-vh,5);
    return radiance*nl*min(d*visibility*f,8)*puddle;
}
float3 RainNormal(float3 world,float3 n,float puddle) {
    float2 p=world.xz*2.3;
    float2 cell=floor(p),delta=frac(p)-.5;
    float radius=length(delta),phase=frac(RainOrigin.w*.65+RainHash(cell));
    float ring=sin(45*(radius-phase*.65))*exp(-70*pow(radius-phase*.65,2));
    float2 ripple=delta/max(radius,.01)*ring*(1-phase)*.035*RainSurface.w;
    return normalize(lerp(n,float3(ripple.x,1,ripple.y),puddle));
}
// Source is the batch HDR snapshot. No history, no off-screen scene lookup.
// Confidence falls to zero at screen borders and depth discontinuities.
float4 RainSSR(float3 position,float3 n,float3 v) {
    float3 ray=reflect(-v,n);
    float3 start=position+n*.08;
    float previous=-1;
    [loop] for(int i=0;i<24;++i) {
        float t=(i+1)/24.0;
        float3 q=start+ray*(.15+t*t*RainReflection.z);
        float4 q4=float4(q,1);
        float4 c=float4(dot(ForwardVP[0],q4),dot(ForwardVP[1],q4),dot(ForwardVP[2],q4),dot(ForwardVP[3],q4));
        if(c.w<=.001) break;
        float2 uv=float2(c.x,-c.y)/c.w*.5+.5;
        if(any(uv<=0)||any(uv>=1)||c.z/c.w<=0||c.z/c.w>=1) break;
        float z=tex2Dlod(SceneDepth,float4(uv,0,0)).r;
        if(z>=.99999) {previous=-1;continue;}
        float4 ndc=float4(uv.x*2-1,1-uv.y*2,z,1);
        float w=dot(InverseVP[3],ndc);
        if(abs(w)<1e-8) {previous=-1;continue;}
        float3 surface=float3(dot(InverseVP[0],ndc),dot(InverseVP[1],ndc),dot(InverseVP[2],ndc))/w;
        float gap=length(q)-length(surface);
        if(i>1 && previous<=0 && gap>0 && gap<RainReflection.w) {
            float border=saturate(min(min(uv.x,uv.y),min(1-uv.x,1-uv.y))*12);
            float confidence=border*(1-t)*saturate(1-gap/RainReflection.w);
            return float4(max(tex2Dlod(NativeScene,float4(uv,0,0)).rgb,0),confidence);
        }
        previous=gap;
    }
    return 0;
}
