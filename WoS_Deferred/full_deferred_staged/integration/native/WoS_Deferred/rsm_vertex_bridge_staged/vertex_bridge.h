#pragma once
// Staged pure adapter for exact native ShadowVS2 -> native skinned VS70/VS73.
// Caller must prove exact VS/PS family, current native declaration and current draw/frame.
#include <cstdint>
#include <cstring>
#include <cmath>
namespace WosVertexBridge {
enum class Family { Tint69, Transform72 };
struct Material {Family family=Family::Tint69;float tint[4]={};float animationTime=0;float diffuseU[4]={},diffuseV[4]={};};
struct Element {uint16_t stream,offset;uint8_t type,method,usage,index;};
static_assert(sizeof(Element)==8,"D3DVERTEXELEMENT9 byte layout");
static const Element TintDecl[]={{0,0,15,0,5,0},{0,4,16,0,6,0},{0,12,10,0,0,0},{0,20,16,0,3,0},{0,28,4,0,10,0},{0,32,8,0,1,0},{0,36,5,0,2,0},{0,40,16,0,7,0},{255,0,17,0,0,0}};
static const Element TransformDecl[]={{0,0,15,0,5,0},{0,4,16,0,6,0},{0,12,10,0,0,0},{0,20,16,0,3,0},{0,28,8,0,1,0},{0,32,5,0,2,0},{0,36,16,0,7,0},{255,0,17,0,0,0}};
inline bool ValidFamily(Family f){return f==Family::Tint69||f==Family::Transform72;}
inline bool Declaration(Family f,const Element* e,unsigned count,unsigned stride) {
 if(!e||!ValidFamily(f))return false;
 return f==Family::Tint69?count==9&&stride==48&&!std::memcmp(e,TintDecl,sizeof(TintDecl)):
     count==8&&stride==44&&!std::memcmp(e,TransformDecl,sizeof(TransformDecl));
}
inline bool Finite(const float* f,unsigned n){for(unsigned i=0;i<n;++i)if(!std::isfinite(f[i]))return false;return true;}
// Writes only on success, including when destination aliases source.
// Exact current palette/projection/decomp from native ShadowVS2; never past main-pass constants.
inline bool Pack(const float (&shadow)[256][4],const Material& material,float (&output)[256][4]) {
 if(!ValidFamily(material.family)||!Finite(shadow[0],150*4))return false;
 if(material.family==Family::Tint69){if(!Finite(material.tint,4))return false;}
 else if(!std::isfinite(material.animationTime)||!Finite(material.diffuseU,4)||!Finite(material.diffuseV,4))return false;
 float result[256][4]={};
 std::memcpy(result,shadow,148*16); // current bones and exact world-to-light clip projection
 if(material.family==Family::Tint69){
  std::memcpy(result[156],material.tint,16);std::memcpy(result[157],shadow[148],16);std::memcpy(result[158],shadow[149],16);
 } else {
  result[156][0]=material.animationTime;std::memcpy(result[157],material.diffuseU,16);std::memcpy(result[158],material.diffuseV,16);
  std::memcpy(result[163],shadow[148],16);std::memcpy(result[164],shadow[149],16);
 }
 // Unused shadow lookup/normal/spec UV outputs are zeroed; flux PS reads only uv0/world-normal/tint.
 // Native shader-local DEF constants remain part of exact VS bytecode, never host state.
 std::memcpy(output,result,sizeof(result));return true;
}
}
