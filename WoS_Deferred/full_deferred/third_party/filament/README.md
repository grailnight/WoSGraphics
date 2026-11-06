# Filament BRDF source and WoS compatibility port

Only three shader math functions are ported. No Filament engine, renderer, binary, material system, package manager, or runtime dependency is included.

Upstream project: https://github.com/google/filament
Pinned commit: f840f1e6aa65ec915f6bc8a2c67869f5ad631e35
Source: https://github.com/google/filament/blob/f840f1e6aa65ec915f6bc8a2c67869f5ad631e35/shaders/src/surface_brdf.fs
Documentation: https://google.github.io/filament/main/filament.html
License: Apache-2.0, exact upstream LICENSE retained here. The upstream root directory at this commit has no NOTICE file. The copied shader contains no separate copyright header; its original content and references are preserved in surface_brdf.upstream.fs.

provenance.json contains download URLs, commit, date and SHA256. surface_brdf.upstream.fs is an unmodified reference copy. The build uses ../../deferred_brdf.hlsl, a prominently marked modified HLSL port of desktop D_GGX, V_SmithGGXCorrelated and the vector F_Schlick(f0,f90,VoH). GLSL vectors were translated, global macro dependencies were replaced with literals/local constants, and pow5 was expanded to multiplies. The desktop distribution preserves the upstream fp16 overflow cap. The correlated Smith term preserves its minimum denominator.

The local WoS_* helpers are not claimed to be Filament material conversion. Existing RT3 RGB is an artistic native specular coefficient with material alpha/window factors already multiplied into it. It is not known to be Fresnel F0 and can exceed one. Blindly using it as F0 both changes highlight brightness and lets Schlick's white grazing term restore specular on zero-spec pixels. No metallic/IOR values can be recovered from this data alone.

The recommended first integration uses WoS_GGXSpecularLobe as an intentional peak-normalized angular replacement. It retains native coefficient RGB as amplitude/tint, uses a generic 4% dielectric Fresnel shape, and normalizes to native amplitude when N=V=L. Thus specCoeff=0 remains exactly zero, partial window/material weights remain proportional, diffuse keeps its established native intensity, and no arbitrary new white albedo/F0 is written into the G-buffer. This adapter is a legacy-compatible microfacet lobe, not energy-conserving or physically calibrated PBR. Native IBL/specular residual stays under the old model; matching that environment response would need additional work.

Suggested resolve change (root owns resolve): replace the existing specular pow/Blinn term in Evaluate with WoS_GGXSpecularLobe(n,v,l,specular,power), then return nativeLightRGB*NoL*(max(rho,0)+ggxSpecular*Settings.y). Keep native visibility outside the BRDF, and keep existing fog multiplication and per-light attenuation. Include deferred_brdf.hlsl through compiler include handling or concatenate its source at compile time; this file does not modify the resolve or active runtime.

Width mapping: native reflection-vector Phong power p is converted to half-vector power about4p, then alpha=sqrt(2/(4p+2)); perceptualRoughness=sqrt(alpha). It is a heuristic lobe-width conversion, not an equivalence to GGX. The alpha floor0.02 bounds sharp grazing responses under legacy targets.

If physically normalized GGX is desired later, use D*V*F without peak normalization, explicitly determine F0 in[0,1] (commonly low achromatic values for dielectric materials), and calibrate light intensities. Native diffuse is rho*lightColor*NoL, without 1/PI. To preserve its intensity while using a normalized Lambert+GGX BRDF, choose effective incident light PI*nativeLightColor, yielding nativeLightColor*NoL*(rho+PI*D*V*F). That has a different highlight energy/peak from the old arbitrary specular coefficients and needs visual validation. Do not multiply the peak-normalized compatibility adapter by PI as well.
