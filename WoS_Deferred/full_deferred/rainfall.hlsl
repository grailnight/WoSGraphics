// Native HDR additive rain volume, evaluated once before bloom.
sampler2D Depth : register(s4);
float4 Inv[4] : register(c0);
float4 OriginTime : register(c4);
float4 Rain : register(c5); // intensity, distance, width, speed
float HashRain(float3 p){return frac(sin(dot(p,float3(127.1,311.7,74.7)))*43758.5453);}
float4 main(float2 uv:TEXCOORD0):COLOR0 {
    float z=tex2D(Depth,uv).r;
    float4 q=float4(uv.x*2-1,1-uv.y*2,min(z,.99999),1);
    float w=dot(Inv[3],q);
    if(abs(w)<1e-8)return 0;
    float3 p=float3(dot(Inv[0],q),dot(Inv[1],q),dot(Inv[2],q))/w;
    float distance=length(p);
    float3 ray=p/max(distance,.001);
    float limit=min(distance,Rain.y),sum=0;
    float jitter=HashRain(float3(floor(uv*float2(1920,1080)),0));
    [loop]for(int i=0;i<32;++i){
        float t=(i+jitter)/32*limit;
        float3 world=OriginTime.xyz+ray*t;
        // Advection downward, with a small horizontal wind.
        world+=float3(-.7,Rain.w,.2)*OriginTime.w;
        float3 grid=world*float3(1.8,.6,1.8);
        float3 cell=floor(grid),f=frac(grid);
        float seed=HashRain(cell);
        float2 center=float2(HashRain(cell+17),HashRain(cell+59))*.7+.15;
        float radius=max(Rain.z,t*.0008);
        float drop=(1-smoothstep(radius,radius*2,length(f.xz-center)))*
            smoothstep(0,.12,f.y)*(1-smoothstep(.65,.95,f.y))*step(.72,seed);
        sum+=drop*(1-t/Rain.y);
    }
    return float4(float3(.45,.52,.62)*min(sum*.15, .65)*Rain.x,0);
}
