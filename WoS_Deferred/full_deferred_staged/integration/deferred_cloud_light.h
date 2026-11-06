#pragma once
// Cloud cover as an occluder of the sun.
//
// The volumetric cloud layer is a mod-side raymarch in the sky pass, so the
// engine's own light table knows nothing about it: at full coverage the game
// still reports a clear-sky sun, and every consumer of that table draws a
// sunlit city under an overcast sky. This module closes that gap at the one
// point where the sun radiance is mod-owned data, before it is interned.
//
// Scaling the table rather than any shader is deliberate. The deferred direct
// light, its GGX highlight and the volumetric shafts all read the same rows, so
// one multiply moves all three together and no compiled shader changes. The
// cascade maps need no change either: a sun scaled to nothing casts nothing
// worth shadowing, and the shadow term stays a ratio of whatever is left.
//
// This is an artistic coupling, not a radiative transfer result. The layer's
// real optical depth along the sun ray is never sampled; the curve below is a
// shape chosen so that a half-covered sky still reads as sunny and a fully
// covered one does not. A physical version would march the same density field
// the cloud shader uses and would also give moving cloud shadows, which this
// cannot.
#include <cmath>
namespace WosCloudLight {

// User settings, persisted through WosDeferred::SaveSettings. Blocking zero
// restores exactly the previous behaviour: the table is passed through.
static bool enabled=true;
static float blocking=.9f;           // fraction of direct sun removed at full cover

// Published every frame by the integration from the live cloud menu values.
static float coverage=0.f,opacity=0.f;
static bool sourceValid=false;

// Last value applied, for the status file.
static float transmittance=1.f;
static unsigned applied=0,lastApplied=0;

static float Transmittance(){
    if(!enabled||!sourceValid)return 1.f;
    const float cover=coverage<0?0:(coverage>1?1:coverage);
    const float alpha=opacity<0?0:(opacity>1?1:opacity);
    const float strength=blocking<0?0:(blocking>1?1:blocking);
    // Quadratic, so half cover keeps most of the sun: the layer only becomes a
    // continuous lid near the top of the range, which is where the shader's own
    // density threshold closes as well.
    const float blocked=cover*alpha;
    const float value=1.f-strength*blocked*blocked;
    return std::isfinite(value)?(value<0?0:(value>1?1:value)):1.f;
}
// Scale the directional radiance of one light set. Baked vertex illumination
// carries its colour in the same texels with a set marker and no direction;
// it is not the sun and is left alone.
static void ApplyToSun(float* texels){
    if(!texels)return;
    transmittance=Transmittance();
    if(transmittance>=1.f)return;
    if(texels[7]>.5f)return;                                  // baked directional
    const float lengthSquared=texels[0]*texels[0]+texels[1]*texels[1]+texels[2]*texels[2];
    if(!(lengthSquared>1e-12f))return;                        // no directional light
    for(unsigned i=4;i<7;++i){
        const float scaled=texels[i]*transmittance;
        if(!std::isfinite(scaled))return;
    }
    for(unsigned i=4;i<7;++i)texels[i]*=transmittance;
    ++applied;
}
static void FrameEnd(){lastApplied=applied;applied=0;}
static void Reset(){coverage=opacity=0.f;sourceValid=false;transmittance=1.f;applied=lastApplied=0;}
static const char* Status(){
    if(!enabled)return "Cloud sun blocking OFF";
    if(!sourceValid)return "Cloud sun blocking: no cloud pass seen this frame";
    return "Cloud cover attenuates the direct sun";
}
}
