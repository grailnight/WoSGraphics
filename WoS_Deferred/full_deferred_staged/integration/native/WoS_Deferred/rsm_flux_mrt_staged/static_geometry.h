#pragma once
// Restricted exact PS111/VS112 on native shadow VS3. No inferred fourth row.
#include <cmath>
#include <cstdint>
#include <cstring>
namespace WosStatic111 {
struct Element {uint16_t stream,offset;uint8_t type,method,usage,index;};
static_assert(sizeof(Element)==8,"native declaration layout");
inline constexpr Element declaration[]={{0,0,15,0,5,0},{0,4,10,0,0,0},{0,12,16,0,3,0},{255,0,17,0,0,0}};
inline bool Declaration(const void* elements,unsigned count,unsigned stride){
 return elements&&count==4&&stride==20&&!std::memcmp(elements,declaration,sizeof(declaration));
}
inline bool Finite(const float* p,unsigned n){if(!p)return false;for(unsigned i=0;i<n;++i)if(!std::isfinite(p[i]))return false;return true;}
// Full row-vector instance * row-vector scene VP, rounded once to uploaded fp32.
// Volatile double intermediates specify the strict sufficient association used
// by this candidate; another native rounding path is rejected, never tolerated.
inline float Product(const float* world,const float* vp,unsigned r,unsigned c){
 volatile double p0=double(world[r*4])*double(vp[c]);
 volatile double p1=double(world[r*4+1])*double(vp[4+c]);
 volatile double p2=double(world[r*4+2])*double(vp[8+c]);
 volatile double p3=double(world[r*4+3])*double(vp[12+c]);
 volatile double s0=p0+p1;volatile double s1=s0+p2;volatile double s2=s1+p3;
 volatile float result=static_cast<float>(s2);return result;
}
inline bool Projection(const float* world,const float* vp,const float* shadow){
 if(!Finite(world,16)||!Finite(vp,16)||!Finite(shadow,40))return false;
 // Actual complete row-vector matrix, not reconstruction from VS c4..6.
 if(world[3]!=0||world[7]!=0||world[11]!=0||world[15]!=1||shadow[7*4+3]!=0||shadow[8*4+3]!=1)return false;
 for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c)
  if(std::memcmp(shadow+16+r*4+c,world+c*4+r,4))return false;
 for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c){
  const float expected=Product(world,vp,r,c);
  if(!std::isfinite(expected)||std::memcmp(&expected,shadow+c*4+r,4))return false;
 }return true;
}
// Engine is the existing verified read-only adapter. Fresh same-frame globals,
// scene VP, complete instance and current uploads are rechecked after queries.
template<class Engine,class Device>bool CurrentProjection(Engine&engine,Device*device,uint32_t scene,uint32_t frame,uint32_t instance,const float*vp){
 if(!device||instance<0x10000||uint64_t(instance)+64>0x100000000ull)return false;
 float world[16]{},after[16]{},rows[40]{},rowsAfter[40]{},vpAfter[16]{};uint32_t sceneAfter=0,frameAfter=0;
 if(!engine.Read(instance,world,sizeof(world))||device->GetVertexShaderConstantF(0,rows,10)<0||!Projection(world,vp,rows))return false;
 return engine.Read(instance,after,sizeof(after))&&!std::memcmp(world,after,sizeof(world))&&
  engine.Read(scene+0x190,vpAfter,sizeof(vpAfter))&&!std::memcmp(vp,vpAfter,sizeof(vpAfter))&&
  engine.Read(0x01115c54,&sceneAfter,4)&&engine.Read(0x011184f4,&frameAfter,4)&&sceneAfter==scene&&frameAfter==frame&&
  device->GetVertexShaderConstantF(0,rowsAfter,10)>=0&&!std::memcmp(rows,rowsAfter,sizeof(rows));
}
}
