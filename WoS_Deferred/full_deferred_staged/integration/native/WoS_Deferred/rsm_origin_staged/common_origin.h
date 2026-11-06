#pragma once
// Staged CPU-only adapter. No engine reads, mutable globals, GPU or production changes.
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/shadow_cascades.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_Deferred/rsm_transport_staged/rsm_projection.h"
#include <cstring>
namespace WosRsmOrigin {
using Vec3=WosRsmProjection::Vec3;
struct CameraSpace {
 double origin[3]={}; // exact eye from the same inversion used for mainInverseRows
 double absoluteInverse[16]={},relativeForward[16]={};
 float mainInverseRows[16]={},mainForwardRows[16]={};
 float legacyFloatPosition[3]={}; // diagnostics only; never accepted as light origin
 bool valid=false,hasNativeMatrices=false;
};
struct LightSpace {
 WosRsmProjection::Projection projection;
 float inverseRows[16]={},forwardRows[16]={};
 bool valid=false;
};
struct Pair {CameraSpace main;LightSpace nearMap,farMap;};
inline bool SameOrigin(const CameraSpace&a,const CameraSpace&b){
 return a.valid&&b.valid&&a.origin[0]==b.origin[0]&&a.origin[1]==b.origin[1]&&a.origin[2]==b.origin[2];
}
// Deliberately follows current indirect_camera.h Decode gates and arithmetic.
// Retaining the unrounded eye is the only new source datum needed in production.
inline bool DecodeCamera(const float*nativeVP,CameraSpace&out,const char**reason=nullptr){
 auto reject=[&](const char*s){if(reason)*reason=s;return false;};if(!nativeVP)return reject("null main VP");
 WosShadowTransform::Matrix4 vp;std::memcpy(vp.v,nativeVP,64);CameraSpace c;
 if(!WosCascades::Inverse(vp,c.absoluteInverse))return reject("main inverse");
 const double*inv=c.absoluteInverse;
 double directionLength=std::sqrt(double(vp.v[3])*vp.v[3]+double(vp.v[7])*vp.v[7]+double(vp.v[11])*vp.v[11]);
 if(!std::isfinite(directionLength)||directionLength<.5||directionLength>2||std::abs(inv[11])<1e-10)return reject("main perspective gate");
 for(unsigned i=0;i<3;++i){c.origin[i]=inv[8+i]/inv[11];if(!std::isfinite(c.origin[i])||std::abs(c.origin[i])>1e7)return reject("main eye range");c.legacyFloatPosition[i]=float(c.origin[i]);}
 WosCascades::V3 nearPoint,midPoint;
 if(!WosCascades::Unproject(inv,0,0,0,nearPoint)||!WosCascades::Unproject(inv,0,0,.5,midPoint))return reject("main depth unprojection");
 auto w=[&](WosCascades::V3 p){return p.x*vp.v[3]+p.y*vp.v[7]+p.z*vp.v[11]+vp.v[15];};
 if(w(nearPoint)<=0||w(midPoint)<=w(nearPoint))return reject("main depth direction");
 for(unsigned r=0;r<4;++r)for(unsigned col=0;col<4;++col){
  double value=inv[col*4+r],relative=r<3?value-c.origin[r]*inv[col*4+3]:value;
  if(!std::isfinite(value)||std::abs(value)>1e10||!std::isfinite(relative)||std::abs(relative)>1e10)return reject("main inverse packing bounds");
  c.mainInverseRows[r*4+col]=float(relative);
 }
 for(unsigned i=0;i<16;++i)c.relativeForward[i]=vp.v[i];
 for(unsigned col=0;col<4;++col)c.relativeForward[12+col]+=c.origin[0]*vp.v[col]+c.origin[1]*vp.v[4+col]+c.origin[2]*vp.v[8+col];
 for(unsigned r=0;r<4;++r)for(unsigned col=0;col<4;++col){double q=c.relativeForward[col*4+r];if(!std::isfinite(q)||std::abs(q)>(std::numeric_limits<float>::max)())return reject("main forward packing bounds");c.mainForwardRows[r*4+col]=float(q);}
 c.valid=c.hasNativeMatrices=true;out=c;if(reason)*reason="ok";return true;
}
// Integration path for one already validated WosIndirectCamera::Camera snapshot.
// The typed array reference cannot accept float Camera.position. Both arrays must
// come from the same retained Camera object/frame. Absolute/forward matrices are
// intentionally unavailable here; only BindLight/PackGather may consume this form.
inline bool RetainMainSnapshot(const double (&exactOrigin)[3],const float (&mainInverseRows)[16],CameraSpace&out){
 CameraSpace c;
 for(unsigned i=0;i<3;++i){if(!std::isfinite(exactOrigin[i])||std::abs(exactOrigin[i])>1e7)return false;c.origin[i]=exactOrigin[i];c.legacyFloatPosition[i]=float(exactOrigin[i]);}
 for(unsigned i=0;i<16;++i){if(!std::isfinite(mainInverseRows[i])||std::abs(mainInverseRows[i])>1e10)return false;c.mainInverseRows[i]=mainInverseRows[i];}
 if(std::abs(mainInverseRows[14])<1e-10)return false;
 for(unsigned i=0;i<3;++i)if(std::abs(mainInverseRows[i*4+2])>1e-6*std::abs(mainInverseRows[14]))return false;
 c.valid=true;out=c;return true;
}
// All maps are rebased with the exact eye held by CameraSpace; callers cannot
// accidentally pass a float Camera.position array through this interface.
inline bool BindLight(const CameraSpace&camera,const float*nativeLightVP,unsigned width,unsigned height,LightSpace&out,const char**reason=nullptr){
 if(!camera.valid){if(reason)*reason="uninitialized common camera origin";return false;}
 LightSpace light;Vec3 origin{camera.origin[0],camera.origin[1],camera.origin[2]};
 if(!WosRsmProjection::Decode(nativeLightVP,width,height,origin,light.projection,reason))return false;
 if(!WosRsmProjection::PackHlslRows(light.projection,light.forwardRows,light.inverseRows)){if(reason)*reason="light float packing";return false;}
 light.valid=true;out=light;if(reason)*reason="ok";return true;
}
inline bool BuildPair(const float*mainVP,const float*nearVP,unsigned nearWidth,unsigned nearHeight,
 const float*farVP,unsigned farWidth,unsigned farHeight,Pair&out,const char**reason=nullptr){
 Pair p;if(!DecodeCamera(mainVP,p.main,reason)||!BindLight(p.main,nearVP,nearWidth,nearHeight,p.nearMap,reason)||!BindLight(p.main,farVP,farWidth,farHeight,p.farMap,reason))return false;out=p;return true;
}
// A native current world light vector is a required independent input.
// Increasing depth is an axis, not by itself a proof of light-direction sign.
inline bool ConfirmTowardSun(const LightSpace&light,Vec3 nativeTowardSun,Vec3&out,double tolerance=2e-5){
 using namespace WosRsmProjection;
 if(!light.valid||!Finite(nativeTowardSun)||!std::isfinite(tolerance)||tolerance<0||tolerance>.01)return false;
 double length=Length(nativeTowardSun);if(!(length>.5&&length<2))return false;
 Vec3 direction=Scale(nativeTowardSun,1/length);
 if(Dot(direction,light.projection.depthRay)>-1+tolerance)return false;
 out=direction;return true;
}
// Copies one HLSL upload block: c0..3 main inverse; c4..7 chosen light inverse;
// c8..11 chosen light VP. No additional origin subtraction is done in shader.
inline bool PackGather(const CameraSpace&camera,const LightSpace&light,float*rows48){
 if(!camera.valid||!light.valid||!rows48)return false;const Vec3&o=light.projection.worldOrigin;
 if(o.x!=camera.origin[0]||o.y!=camera.origin[1]||o.z!=camera.origin[2])return false;
 float result[48];std::memcpy(result,camera.mainInverseRows,64);std::memcpy(result+16,light.inverseRows,64);std::memcpy(result+32,light.forwardRows,64);std::memcpy(rows48,result,sizeof(result));return true;
}
}
