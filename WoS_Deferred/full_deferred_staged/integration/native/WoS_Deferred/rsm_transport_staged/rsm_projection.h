#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

// CPU-only orthographic shadow-map geometry. Native matrices use row vectors:
// clip = float4(world,1) * nativeVP; D3D depth is z/w in [0,1].
// UV has top-left origin: u=.5*x/w+.5, v=.5-.5*y/w. Pixel centers are
// ((x+.5)/width,(y+.5)/height), with no extra half-texel translation here.
namespace WosRsmProjection {
struct Vec3 {double x=0,y=0,z=0;};
struct UVDepth {double u=0,v=0,depth=0;bool inside=false;};
struct Projection {
    double nativeVP[16]={},relativeVP[16]={},inverseRelativeVP[16]={};
    Vec3 worldOrigin,depthRay;
    unsigned width=0,height=0;
    double projectedMapArea=0,projectedTexelArea=0,depthSpan=0;
};
static Vec3 Add(Vec3 a,Vec3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
static Vec3 Sub(Vec3 a,Vec3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
static Vec3 Scale(Vec3 a,double x){return {a.x*x,a.y*x,a.z*x};}
static double Dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
static Vec3 Cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
static double Length(Vec3 a){return std::sqrt(Dot(a,a));}
static bool Finite(Vec3 a){return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z);}
static bool Homogeneous(const double matrix[16],Vec3 p,Vec3&out){
    if(!Finite(p))return false;
    double q[4];for(unsigned col=0;col<4;++col)q[col]=p.x*matrix[col]+p.y*matrix[4+col]+p.z*matrix[8+col]+matrix[12+col];
    if(!std::isfinite(q[3])||q[3]<=0)return false;
    Vec3 result={q[0]/q[3],q[1]/q[3],q[2]/q[3]};if(!Finite(result))return false;out=result;return true;
}
// All public conversions leave the output unchanged on failure. Relative
// positions keep native world axes; only the caller-provided origin is removed.
static bool Project(const Projection&p,Vec3 relativePosition,UVDepth&out){
    if(!p.width||!p.height)return false;Vec3 clip;
    if(!Homogeneous(p.relativeVP,relativePosition,clip))return false;
    UVDepth q={clip.x*.5+.5,.5-clip.y*.5,clip.z,false};
    q.inside=q.u>=0&&q.u<=1&&q.v>=0&&q.v<=1&&q.depth>=0&&q.depth<=1;out=q;return true;
}
static bool Unproject(const Projection&p,double u,double v,double depth,Vec3&out){
    if(!p.width||!p.height||!std::isfinite(u)||!std::isfinite(v)||!std::isfinite(depth)||
       u<0||u>1||v<0||v>1||depth<0||depth>1)return false;
    return Homogeneous(p.inverseRelativeVP,{u*2-1,1-v*2,depth},out);
}
static bool TexelPosition(const Projection&p,unsigned x,unsigned y,double depth,Vec3&out){
    if(x>=p.width||y>=p.height)return false;
    return Unproject(p,(double(x)+.5)/p.width,(double(y)+.5)/p.height,depth,out);
}
// Pack row-vector matrices into four dot-product rows for HLSL:
// clip_i = dot(relativeVPRows[i],float4(relativePosition,1));
// relativePosition_i = dot(inverseRows[i],float4(2u-1,1-2v,depth,1)).
static bool PackHlslRows(const Projection&p,float relativeVPRows[16],float inverseRows[16]){
    if(!p.width||!p.height)return false;float forward[16],inverse[16];
    for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col){
        double a=p.relativeVP[col*4+row],b=p.inverseRelativeVP[col*4+row];
        if(!std::isfinite(a)||!std::isfinite(b)||std::abs(a)>std::numeric_limits<float>::max()||std::abs(b)>std::numeric_limits<float>::max())return false;
        forward[row*4+col]=float(a);inverse[row*4+col]=float(b);
    }
    for(unsigned i=0;i<16;++i){relativeVPRows[i]=forward[i];inverseRows[i]=inverse[i];}return true;
}
static bool Decode(const float nativeVP[16],unsigned width,unsigned height,Vec3 worldOrigin,Projection&out,const char**failure=nullptr){
    auto reject=[&](const char*why){if(failure)*failure=why;return false;};
    if(!nativeVP||!width||!height||width>16384||height>16384||!Finite(worldOrigin))return reject("invalid dimensions or origin");
    for(unsigned i=0;i<16;++i)if(!std::isfinite(nativeVP[i]))return reject("nonfinite light matrix");
    // Products of an affine view and an orthographic projection have an exact
    // zero spatial w-column. Even a small nonzero perspective term is rejected.
    if(nativeVP[3]!=0||nativeVP[7]!=0||nativeVP[11]!=0||nativeVP[15]<=0)return reject("perspective or nonpositive clip w");
    Projection p;p.width=width;p.height=height;p.worldOrigin=worldOrigin;
    for(unsigned i=0;i<16;++i)p.nativeVP[i]=p.relativeVP[i]=double(nativeVP[i])/nativeVP[15];
    for(unsigned col=0;col<3;++col)p.relativeVP[12+col]+=worldOrigin.x*p.relativeVP[col]+worldOrigin.y*p.relativeVP[4+col]+worldOrigin.z*p.relativeVP[8+col];
    Vec3 axes[3];double lengths[3];
    for(unsigned i=0;i<3;++i){axes[i]={p.nativeVP[i],p.nativeVP[4+i],p.nativeVP[8+i]};lengths[i]=Length(axes[i]);if(!std::isfinite(lengths[i])||lengths[i]<=0)return reject("singular light matrix");}
    // Exclude affine shear/oblique projections: this decoder describes a
    // directional-light orthographic view with perpendicular camera axes.
    for(unsigned i=0;i<3;++i)for(unsigned j=i+1;j<3;++j)
        if(std::abs(Dot(Scale(axes[i],1/lengths[i]),Scale(axes[j],1/lengths[j])))>2e-5)return reject("nonorthogonal light axes");
    // Invert only the 3x3 linear part. Large absolute translations never enter
    // the elimination, and the inverse translation is formed after rebasing.
    double a[3][6]={};
    for(unsigned row=0;row<3;++row)for(unsigned col=0;col<3;++col){a[row][col]=p.relativeVP[row*4+col];a[row][col+3]=row==col?1:0;}
    for(unsigned col=0;col<3;++col){unsigned pivot=col;for(unsigned row=col+1;row<3;++row)if(std::abs(a[row][col])>std::abs(a[pivot][col]))pivot=row;
        if(a[pivot][col]==0||!std::isfinite(a[pivot][col]))return reject("singular light matrix");
        for(unsigned k=0;k<6;++k)std::swap(a[col][k],a[pivot][k]);double divisor=a[col][col];for(double&v:a[col])v/=divisor;
        for(unsigned row=0;row<3;++row)if(row!=col){double factor=a[row][col];for(unsigned k=0;k<6;++k)a[row][k]-=factor*a[col][k];}
    }
    p.inverseRelativeVP[15]=1;
    for(unsigned row=0;row<3;++row)for(unsigned col=0;col<3;++col)p.inverseRelativeVP[row*4+col]=a[row][col+3];
    for(unsigned col=0;col<3;++col)for(unsigned row=0;row<3;++row)p.inverseRelativeVP[12+col]-=p.relativeVP[12+row]*p.inverseRelativeVP[row*4+col];
    for(unsigned i=0;i<16;++i)if(!std::isfinite(p.relativeVP[i])||!std::isfinite(p.inverseRelativeVP[i]))return reject("nonfinite inverse or rebased matrix");
    Vec3 du=Scale({p.inverseRelativeVP[0],p.inverseRelativeVP[1],p.inverseRelativeVP[2]},2);
    Vec3 dv=Scale({p.inverseRelativeVP[4],p.inverseRelativeVP[5],p.inverseRelativeVP[6]},-2);
    Vec3 dz={p.inverseRelativeVP[8],p.inverseRelativeVP[9],p.inverseRelativeVP[10]};
    p.depthSpan=Length(dz);if(!std::isfinite(p.depthSpan)||p.depthSpan<=0)return reject("invalid depth span");
    p.depthRay=Scale(dz,1/p.depthSpan);p.projectedMapArea=std::abs(Dot(Cross(du,dv),p.depthRay));
    p.projectedTexelArea=p.projectedMapArea/double(width)/double(height);
    if(!std::isfinite(p.projectedMapArea)||!std::isfinite(p.projectedTexelArea)||p.projectedTexelArea<=0)return reject("invalid projected area");
    out=p;if(failure)*failure="ok";return true;
}
// projectedTexelArea is in engine-world-unit^2, perpendicular to increasing
// shadow depth. It is neither surface area nor a solid angle. A depth of 1 can
// also be the untouched INTZ clear value: geometry/normal validity must be
// supplied separately. No albedo, irradiance, cosines, pi factors, unit-to-metre
// scale, flux, or light-direction sign convention is inferred by this decoder.
}
