#pragma once
#include <cmath>
// Current frame's brightest valid native directional light, before cloud
// attenuation. LightSet rows already point toward the light in world space.
// Frames without a directional row (about 2.5% in play) hold the last native
// sun instead of dropping to the manual slider, which used to point below the
// horizon and flipped the cloud and fog lighting for those frames.
namespace WosCloudSunSync {
static float direction[3]={0,1,0}, color[3]={1,1,1}, score=-1;
static bool valid=false, held=false;
static void Reset(){if(valid)held=true;valid=false;score=-1;}
static void Observe(const float* row){
    if(!row || row[7]>.5f)return; // baked light is not a sun
    for(unsigned i=0;i<8;++i)if(!std::isfinite(row[i]))return;
    float len2=0;
    for(unsigned i=0;i<3;++i){len2+=row[i]*row[i];if(row[4+i]<0)return;}
    if(!std::isfinite(len2)||len2<1e-12f)return;
    const float luminance=row[4]*.2126f+row[5]*.7152f+row[6]*.0722f;
    if(luminance<0 || luminance<=score)return;
    const float inverse=1.f/std::sqrt(len2);
    for(unsigned i=0;i<3;++i){direction[i]=row[i]*inverse;color[i]=row[4+i];}
    score=luminance;valid=true;
}
static void Constants(const float* fallback,float focus,float* sun,float* radiance){
    const bool native=valid||held;
    float len2=0;
    for(unsigned i=0;i<3;++i){sun[i]=native?direction[i]:fallback[i];len2+=sun[i]*sun[i];}
    if(!std::isfinite(len2)||len2<1e-12f){sun[0]=0;sun[1]=1;sun[2]=0;}
    else {const float inverse=1.f/std::sqrt(len2);for(unsigned i=0;i<3;++i)sun[i]*=inverse;}
    sun[3]=focus;
    for(unsigned i=0;i<3;++i)radiance[i]=native?color[i]:1.f;
    radiance[3]=native?11.f:0.f;
}
}
