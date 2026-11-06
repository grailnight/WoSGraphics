// Perspective cubemap atlas, matching FaceMatrix in deferred_local_shadow.h.
sampler2D LocalAtlas : register(s9);
sampler2D LocalStatic : register(s10); // static caster layer, read only when LocalTune.w is set
float4 LocalParams : register(c36); // count, tile edge, emitter radius, strength
float4 LocalTune : register(c37);   // world bias, normal bias, near plane, static layer bound
float4 LocalPosition[2] : register(c38);
float LocalDepth(float3 direction, float slot, float farPlane)
{
    float3 a=abs(direction);float face,x,y,z;
    if(a.x>=a.y&&a.x>=a.z){face=direction.x>=0?0:1;x=direction.x>=0?-direction.z:direction.z;y=direction.y;z=a.x;}
    else if(a.y>=a.z){face=direction.y>=0?2:3;x=direction.x;y=direction.y>=0?direction.z:-direction.z;z=a.y;}
    else {face=direction.z>=0?4:5;x=direction.z>=0?direction.x:-direction.x;y=direction.y;z=a.z;}
    float2 local=float2(x,-y)/max(z,1e-8)*.5+.5;
    local=clamp(local,.5/LocalParams.y,1-.5/LocalParams.y);
    float tile=slot*6+face;
    float2 uv=(float2(fmod(tile,3),floor(tile/3))+local)/float2(3,4);
    float d=tex2Dlod(LocalAtlas,float4(uv,0,0)).r;
    // Rigid casters that have not moved live in a second atlas of the same
    // layout; the nearest of the two is the depth one atlas would have held.
    float held=tex2Dlod(LocalStatic,float4(uv,0,0)).r;
    d=LocalTune.w>.5?min(d,held):d;
    float axial=LocalTune.z*farPlane/max(farPlane-d*(farPlane-LocalTune.z),1e-6);
    return axial*length(direction)/max(z,1e-8);
}
float LocalMapShadow(float3 position,float3 normal,float4 light,float2 uv)
{
    if(LocalParams.x<.5||LocalParams.w<=0)return 1;
    float slot=-1;
    [unroll]for(int j=0;j<2;++j){
        float3 d=LocalPosition[j].xyz-light.xyz;
        if(j<LocalParams.x&&dot(d,d)<.0004&&abs(LocalPosition[j].w-light.w)<.00001)slot=j;
    }
    if(slot<0)return 1;
    float3 delta=position+normal*LocalTune.y-light.xyz;
    float distance=length(delta),farPlane=min(1/max(light.w,1e-6),500);
    if(distance<=LocalTune.z*2||distance>=farPlane)return 1;
    float3 ray=delta/distance;
    float3 tangent=normalize(cross(ray,abs(ray.y)<.9?float3(0,1,0):float3(1,0,0)));
    float3 bitangent=cross(ray,tangent);
    float search=min(LocalParams.z/max(distance,.1),.15);
    float blocker=0,hits=0;
    [loop]for(int i=0;i<8;++i){
        float2 disk=BlockerDisk[i]*search;
        float depth=LocalDepth(ray+tangent*disk.x+bitangent*disk.y,slot,farPlane);
        if(depth<distance-LocalTune.x){blocker+=depth;hits+=1;}
    }
    if(hits<.5)return 1;
    blocker/=hits;
    float radius=clamp(LocalParams.z*(distance-blocker)/max(blocker*distance,.01),1/LocalParams.y,.1);
    float visibility=0;
    [loop]for(int k=0;k<16;++k){
        float2 disk=FilterDisk[k]*radius;
        float depth=LocalDepth(ray+tangent*disk.x+bitangent*disk.y,slot,farPlane);
        visibility+=depth>=distance-LocalTune.x?1:0;
    }
    return lerp(1,visibility/16,saturate(LocalParams.w));
}

