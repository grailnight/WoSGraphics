#pragma once
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/shadow_transform.h"
#include <array>
#include <algorithm>
namespace WosCascades {
using WosShadowTransform::Matrix4;
struct V3 { double x,y,z; V3 operator+(V3 b)const{return{x+b.x,y+b.y,z+b.z};} V3 operator-(V3 b)const{return{x-b.x,y-b.y,z-b.z};} V3 operator*(double b)const{return{x*b,y*b,z*b};} };
inline double Dot(V3 a,V3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline bool Unit(V3 &v){double l=std::sqrt(Dot(v,v));if(!std::isfinite(l)||l<1e-12)return false;v=v*(1/l);return true;}
struct Config { double distance=120,logWeight=0.65,casterExtrusion=400,depthPadding=10;unsigned resolution=2048; };
struct Cascade { Matrix4 vp;double nearDepth,farDepth,texelWorld,depthMin,depthMax;std::array<V3,8> corners; };
struct Plan { std::array<Cascade,4> cascades;double cameraNear,cameraFar; };
inline bool Inverse(const Matrix4 &m,double out[16]) {
 double a[4][8]={};
 for(int r=0;r<4;++r)for(int c=0;c<4;++c){if(!std::isfinite(m.v[r*4+c]))return false;a[r][c]=m.v[r*4+c];a[r][c+4]=r==c?1:0;}
 for(int c=0;c<4;++c){int pivot=c;for(int r=c+1;r<4;++r)if(std::abs(a[r][c])>std::abs(a[pivot][c]))pivot=r;
  if(std::abs(a[pivot][c])<1e-14)return false;
  for(int j=0;j<8;++j)std::swap(a[c][j],a[pivot][j]);double k=a[c][c];for(double &x:a[c])x/=k;
  for(int r=0;r<4;++r)if(r!=c){double f=a[r][c];for(int j=0;j<8;++j)a[r][j]-=f*a[c][j];}
 }
 for(int r=0;r<4;++r)for(int c=0;c<4;++c)out[r*4+c]=a[r][c+4];return true;
}
inline bool Unproject(const double inv[16],double x,double y,double z,V3 &out){
 double p[4]={x,y,z,1},v[4]={};for(int j=0;j<4;++j)for(int k=0;k<4;++k)v[j]+=p[k]*inv[k*4+j];
 if(!std::isfinite(v[3])||std::abs(v[3])<1e-12)return false;
 out={v[0]/v[3],v[1]/v[3],v[2]/v[3]};return std::isfinite(out.x)&&std::isfinite(out.y)&&std::isfinite(out.z);
}
// D3D clip depth 0..1. Light orientation is taken from the verified native
// orthographic shadow VP; finite forward-depth perspective cameras only.
inline bool Build(const Matrix4 &cameraVP,const Matrix4 &lightVP,const Config &cfg,Plan &out){
 if(!std::isfinite(cfg.distance)||!std::isfinite(cfg.logWeight)||!std::isfinite(cfg.casterExtrusion)||!std::isfinite(cfg.depthPadding)||cfg.distance<=0||cfg.logWeight<0||cfg.logWeight>1||cfg.casterExtrusion<0||cfg.depthPadding<0||cfg.resolution<16||cfg.resolution>16384)return false;
 double inv[16];if(!Inverse(cameraVP,inv))return false;
 std::array<V3,4> n,f;V3 nc={},fc={};
 for(int i=0;i<4;++i){double x=(i&1)?1:-1,y=(i&2)?1:-1;if(!Unproject(inv,x,y,0,n[i])||!Unproject(inv,x,y,1,f[i]))return false;nc=nc+n[i]*.25;fc=fc+f[i]*.25;}
 double nw=std::sqrt(Dot(n[1]-n[0],n[1]-n[0])),fw=std::sqrt(Dot(f[1]-f[0],f[1]-f[0]));
 if(!std::isfinite(fw)||fw<1e-9)return false;double ratio=nw/fw,d=std::sqrt(Dot(fc-nc,fc-nc));
 if(ratio<=0||ratio>=.99999||d<1e-6)return false;
 // A float VP can yield a slightly tilted unprojected far plane. Fit each ray
 // at actual normalized clip-W depth instead of one shared lerp parameter.
 double planeLength=std::sqrt(double(cameraVP.v[3])*cameraVP.v[3]+double(cameraVP.v[7])*cameraVP.v[7]+double(cameraVP.v[11])*cameraVP.v[11]);
 if(!std::isfinite(planeLength)||planeLength<1e-8)return false;
 auto eyeDepth=[&](V3 p){return(p.x*cameraVP.v[3]+p.y*cameraVP.v[7]+p.z*cameraVP.v[11]+cameraVP.v[15])/planeLength;};
 Plan plan;plan.cameraNear=eyeDepth(nc);plan.cameraFar=eyeDepth(fc);
 if(!std::isfinite(plan.cameraNear)||!std::isfinite(plan.cameraFar)||plan.cameraNear<=0||plan.cameraFar<=plan.cameraNear)return false;
 double end=(std::min)(cfg.distance,plan.cameraFar);if(end<=plan.cameraNear)return false;
 V3 x={lightVP.v[0],lightVP.v[4],lightVP.v[8]},y={lightVP.v[1],lightVP.v[5],lightVP.v[9]},z={lightVP.v[2],lightVP.v[6],lightVP.v[10]};
 if(!Unit(x)||!Unit(y)||!Unit(z)||std::abs(Dot(x,y))>1e-3||std::abs(Dot(x,z))>1e-3||std::abs(Dot(y,z))>1e-3)return false;
 double start=plan.cameraNear;
 for(unsigned i=0;i<4;++i){auto &c=plan.cascades[i];double t=(i+1)/4.0;
  c.nearDepth=start;c.farDepth=(1-cfg.logWeight)*(plan.cameraNear+(end-plan.cameraNear)*t)+cfg.logWeight*plan.cameraNear*std::pow(end/plan.cameraNear,t);
  if(i==3)c.farDepth=end;V3 center={};
  for(unsigned j=0;j<8;++j){double depth=j<4?start:c.farDepth;double rayNear=eyeDepth(n[j%4]),rayFar=eyeDepth(f[j%4]);if(rayFar<=rayNear)return false;double a=(depth-rayNear)/(rayFar-rayNear);c.corners[j]=n[j%4]+(f[j%4]-n[j%4])*a;center=center+c.corners[j]*(1.0/8);}
  double radius=0;for(V3 p:c.corners)radius=(std::max)(radius,std::sqrt(Dot(p-center,p-center)));
  radius=std::ceil(radius*16)/16.0;if(radius<=0||!std::isfinite(radius))return false;
  // Half a texel of margin prevents center snapping from clipping the receiver volume.
  double half=radius*cfg.resolution/(cfg.resolution-1.0);c.texelWorld=2*half/cfg.resolution;
  double cx=std::round(Dot(center,x)/c.texelWorld)*c.texelWorld,cy=std::round(Dot(center,y)/c.texelWorld)*c.texelWorld;
  double lo=Dot(c.corners[0],z),hi=lo;for(V3 p:c.corners){lo=(std::min)(lo,Dot(p,z));hi=(std::max)(hi,Dot(p,z));}
  lo-=cfg.casterExtrusion+cfg.depthPadding;hi+=cfg.depthPadding;if(hi-lo<1e-6)return false;c.depthMin=lo;c.depthMax=hi;
  c.vp={{static_cast<float>(x.x/half),static_cast<float>(y.x/half),static_cast<float>(z.x/(hi-lo)),0,
         static_cast<float>(x.y/half),static_cast<float>(y.y/half),static_cast<float>(z.y/(hi-lo)),0,
         static_cast<float>(x.z/half),static_cast<float>(y.z/half),static_cast<float>(z.z/(hi-lo)),0,
         static_cast<float>(-cx/half),static_cast<float>(-cy/half),static_cast<float>(-lo/(hi-lo)),1}};
  for(float value:c.vp.v)if(!std::isfinite(value))return false;
  start=c.farDepth;
 }
 out=plan;return true;
}
}
