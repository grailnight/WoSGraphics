#pragma once
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_Deferred/rsm_origin_staged/common_origin.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_Deferred/rsm_gather_runtime_staged/sample_data.h"
#include <cstring>
namespace WosRsmGather {
struct NativeMap {bool current=false;unsigned width=0,height=0;float vp[16]={};};
struct Settings {unsigned width=0,height=0;float radius=8,maxDistance=240,bounce=.6f,environment=.12f;bool visibility=true;};
struct Constants {float c[48][4]={};bool use[2]={};WosRsmOrigin::LightSpace maps[2];};
// Integer extent relation shared with pixel_mapping.hlsli; no inferred texture size.
inline bool QuarterDimensions(unsigned width,unsigned height,unsigned gatherWidth,unsigned gatherHeight){
 return width>0&&height>0&&width<=16384&&height<=16384&&gatherWidth==(width+3)/4&&gatherHeight==(height+3)/4;
}
inline bool Pack(const WosRsmOrigin::CameraSpace&camera,const NativeMap (&native)[2],const Settings&s,Constants&out){
 out={};if(!camera.valid||s.width<1||s.height<1||s.width>16384||s.height>16384||
  !std::isfinite(s.radius)||s.radius<.01f||s.radius>1000||!std::isfinite(s.maxDistance)||s.maxDistance<=0||s.maxDistance>10000||
  !std::isfinite(s.bounce)||s.bounce<0||s.bounce>4||!std::isfinite(s.environment)||s.environment<0||s.environment>1)return false;
 Constants p;unsigned width=0,height=0;
 for(unsigned i=0;i<2;++i){if(!native[i].current)continue;
  if(!WosRsmOrigin::BindLight(camera,native[i].vp,native[i].width,native[i].height,p.maps[i]))continue;
  if(width&&(native[i].width!=width||native[i].height!=height))continue;
  width=native[i].width;height=native[i].height;p.use[i]=true;
 }
 if(!p.use[0]&&!p.use[1])return false;
 // Unused sampler slots and matrices may safely alias the valid map; Options
 // alone controls ownership. Never publish zero/uninitialized inverse matrices.
 for(unsigned i=0;i<2;++i)if(!p.use[i])p.maps[i]=p.maps[1-i];
 std::memcpy(p.c,camera.mainInverseRows,64);
 for(unsigned i=0;i<2;++i){std::memcpy(p.c+(i?16:4),p.maps[i].inverseRows,64);std::memcpy(p.c+(i?20:8),p.maps[i].forwardRows,64);}
 p.c[26][0]=float(s.width);p.c[26][1]=float(s.height);p.c[26][2]=float((s.width+3)/4);p.c[26][3]=float((s.height+3)/4);
 p.c[12][0]=1.f/s.width;p.c[12][1]=1.f/s.height;p.c[12][2]=float(width);p.c[12][3]=float(height);
 constexpr double pi=3.1415926535897932384626433832795;
 p.c[13][2]=float(pi*s.radius*s.radius/16);p.c[13][3]=s.bounce;
 p.c[14][0]=s.radius*s.radius;p.c[14][2]=s.maxDistance;
 p.c[15][0]=p.use[0]?1.f:0.f;p.c[15][1]=p.use[1]?1.f:0.f;p.c[15][2]=s.environment;p.c[15][3]=s.visibility?1.f:0.f;
 for(unsigned i=0;i<2;++i){const auto&v=p.maps[i].projection;const double*inv=v.inverseRelativeVP;
  double worldWidth=2*std::sqrt(inv[0]*inv[0]+inv[1]*inv[1]+inv[2]*inv[2]);
  double worldHeight=2*std::sqrt(inv[4]*inv[4]+inv[5]*inv[5]+inv[6]*inv[6]);
  if(!(worldWidth>0&&worldHeight>0))return false;
  p.c[i?25:13][0]=float(s.radius/worldWidth);p.c[i?25:13][1]=float(s.radius/worldHeight);
  p.c[14][i?3:1]=float((std::max)(.0025,v.projectedTexelArea/pi));
  p.c[24][i]=float(v.depthSpan);p.c[24][i+2]=float((std::max)(.025,std::sqrt(v.projectedTexelArea)*.5));
 }
 std::memcpy(p.c+32,RsmDiskSamples,sizeof(RsmDiskSamples));
 for(auto&r:p.c)for(float f:r)if(!std::isfinite(f))return false;
 out=p;return true;
}
}
