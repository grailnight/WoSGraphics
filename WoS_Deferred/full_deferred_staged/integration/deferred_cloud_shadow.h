#pragma once
// Cloud shadows on the city.
//
// WosCloudLight already removes the right amount of sun from the light table
// when the sky is covered, but it removes the same amount everywhere: one
// scalar for the whole frame, no gaps, nothing that moves with the wind. This
// module is the other half of that coupling. It publishes the live layer to
// the resolve, which evaluates the identical procedural density along the ray
// from each receiver toward the sun and keeps only what gets through.
//
// Nothing is rendered here and no shadow map exists. The cloud field is
// procedural, so the deck the sky pass draws can be sampled again in any other
// pass for the cost of the noise alone; the only state that has to travel is
// the eight numbers the menu owns plus the camera origin, because the layer is
// authored in absolute world units and every deferred pass works relative to
// the eye.
//
// Two features now touch the same sun, and both of them scale its radiance:
// the uniform one in the light table, this one per pixel in the shader. Left
// alone they would multiply and an overcast sky would go black, so the
// compensation below hands the shader the reciprocal of what the table already
// lost and the march becomes the single authority on direct sunlight. That is
// also why the shader scales radiance rather than the visibility term: a pixel
// standing in a hole in the deck has to be able to exceed what the table left,
// and a visibility clamped at one could never give it back.
#include <cmath>
namespace WosCloudShadow {

// User settings, persisted through WosDeferred::SaveSettings.
static bool enabled=true;
static float strength=1.f;           // 0 restores the previous image exactly
static int steps=5;                  // density samples along the sun ray, 1..16
static float minimum=.25f;           // least direct sun a solid deck lets through
static bool compensate=true;         // undo the uniform attenuation of the table

// Published every frame by the integration from the live cloud menu values,
// in the same units the sky shader receives them in.
static float bottom=0.f,thickness=0.f,coverage=0.f,size=0.f,opacity=0.f,detail=0.f,wind=0.f;
static bool sourceValid=false;
// What WosCloudLight has already taken out of the directional radiance.
static float uniformTransmittance=1.f;

static unsigned binds=0,lastBinds=0,skips=0,failures=0;
static const char* status="Cloud shadows OFF";

static bool Armed(){
    return enabled&&sourceValid&&strength>0.f&&opacity>.001f&&coverage>0.f&&steps>0;
}
// How much radiance the table has already lost, so the shader can give it
// back. Clamped: an almost fully blocked table would ask for an unbounded
// multiplier, and the sun a hole in the deck reveals is a finite sun.
static float Compensation(){
    if(!compensate)return 1.f;
    const float applied=uniformTransmittance;
    if(!std::isfinite(applied)||!(applied>0.f))return 1.f;
    const float value=1.f/applied;
    if(!std::isfinite(value))return 1.f;
    return value<1.f?1.f:(value>8.f?8.f:value);
}
static float Steps(){const int n=steps<1?1:(steps>16?16:steps);return float(n);}
// The march removes every photon it scatters. Under a solid deck most of the
// daylight that reaches the ground is exactly what it removed, scattered
// onward rather than absorbed, so a march with no floor turns an overcast city
// black. This is an artistic stand-in for that light and not a second order.
static float Minimum(){
    if(!std::isfinite(minimum))return 0.f;
    return minimum<0.f?0.f:(minimum>1.f?1.f:minimum);
}
static float Strength(){return strength<0.f?0.f:(strength>1.f?1.f:strength);}

// Fills c32..c35 for the resolve. A false return leaves the step count at zero,
// which is the one constant the shader tests first, so every path that cannot
// describe the layer produces exactly the previous image rather than a guess.
static bool Bind(IDirect3DDevice9* d,const double origin[3]){
    if(!d)return false;
    const float off[4]={0.f,0.f,0.f,0.f};
    if(!Armed()||!origin){++skips;d->SetPixelShaderConstantF(34,off,1);return false;}
    float eye[3]={};
    for(unsigned i=0;i<3;++i){
        eye[i]=float(origin[i]);
        if(!std::isfinite(eye[i])||std::fabs(eye[i])>1e7f){++skips;d->SetPixelShaderConstantF(34,off,1);return false;}
    }
    const float layer[4]={bottom,coverage,size<20.f?20.f:size,opacity};
    const float shape[4]={thickness,detail,wind,0.f};
    const float march[4]={eye[0],eye[1],eye[2],Steps()};
    const float tune[4]={Strength(),Compensation(),Minimum(),0.f};
    for(unsigned i=0;i<4;++i)
        if(!std::isfinite(layer[i])||!std::isfinite(shape[i])||!std::isfinite(tune[i])){
            ++skips;d->SetPixelShaderConstantF(34,off,1);return false;
        }
    bool ok=SUCCEEDED(d->SetPixelShaderConstantF(32,layer,1));
    ok=SUCCEEDED(d->SetPixelShaderConstantF(33,shape,1))&&ok;
    ok=SUCCEEDED(d->SetPixelShaderConstantF(35,tune,1))&&ok;
    ok=SUCCEEDED(d->SetPixelShaderConstantF(34,march,1))&&ok;
    if(!ok){++failures;d->SetPixelShaderConstantF(34,off,1);return false;}
    ++binds;return true;
}
static void FrameEnd(){
    lastBinds=binds;binds=0;
    if(!enabled)status="Cloud shadows OFF";
    else if(!sourceValid)status="Cloud shadows: no cloud pass seen this frame";
    else if(opacity<=.001f||coverage<=0.f)status="Cloud shadows: the layer is empty this frame";
    else if(!lastBinds)status="Cloud shadows armed, no resolve this frame";
    else status="Cloud shadows ON (density marched toward the sun)";
}
static void Reset(){
    bottom=thickness=coverage=size=opacity=detail=wind=0.f;
    sourceValid=false;uniformTransmittance=1.f;
    binds=lastBinds=skips=failures=0;status="Cloud shadows OFF";
}
static const char* Status(){return status;}
}
