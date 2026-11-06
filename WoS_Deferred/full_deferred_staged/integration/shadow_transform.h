#pragma once
// Verified against captured NGL shadow instances. No engine memory access or GPU calls.
#include <cmath>
#include <limits>
namespace WosShadowTransform {
struct Matrix4 { float v[16]; };
// World stores four xyz vectors. Their fourth components are not used by this
// NGL affine path (some captured translation.w values are zero).
// Scene VP is row-major at scene+0x190; output is transposed for shader constants.
inline bool Build(const Matrix4 &world,const Matrix4 &sceneVP,Matrix4 &shaderWVP) noexcept {
    for(unsigned row=0;row<4;++row)
        for(unsigned k=0;k<3;++k)if(!std::isfinite(world.v[row*4+k]))return false;
    for(float x:sceneVP.v)if(!std::isfinite(x))return false;
    Matrix4 result={};
    for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col) {
        double sum=row==3?static_cast<double>(sceneVP.v[12+col]):0.0;
        for(unsigned k=0;k<3;++k)sum+=static_cast<double>(world.v[row*4+k])*sceneVP.v[k*4+col];
        if(!std::isfinite(sum) || std::abs(sum)>(std::numeric_limits<float>::max)())return false;
        result.v[col*4+row]=static_cast<float>(sum);
    }
    shaderWVP=result;return true;
}
}
