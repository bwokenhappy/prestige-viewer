// Physical main-view consumers use CameraTransport L/RGB-T from cmAt. The
// scalar volFogAt helper is retained only for physical-off extra fog. Physical
// opaque transport is a storage-image compute composite before transparents;
// capture/secondary rays integrate their own bounded segment, never this grid.
#ifndef CLEAN_VOL_APPLY_GLSL
#define CLEAN_VOL_APPLY_GLSL

#include "clean_vol_common.glsl"

layout(set = 0, binding = 45) uniform sampler3D uVolFog;   // rgb in-scatter, a extra-medium T

// (#1608) LAYERED STEREO: the legacy grid of THIS view -- view 0 the vol_* block / binding 45 (as before), view v > 0
// its own eye's instance (ssr.medium[v] / binding 59 [v], uMediumL; ensureVolFog). Not the cm-aware accessors: their
// cmOn() select would change what a caller that reaches here under the camera medium reads. One view: the reads of
// before (MEDIUM_EYE folds to 0).
vec4 vfaEye() { return MEDIUM_EYE != 0u ? ssr.medium[MEDIUM_EYE].eye : ssr.vol_eye; }
mat4 vfaVP() { return MEDIUM_EYE != 0u ? ssr.medium[MEDIUM_EYE].vp : ssr.vol_vp; }
mat4 vfaInvVP() { return MEDIUM_EYE != 0u ? ssr.medium[MEDIUM_EYE].inv_vp : ssr.vol_inv_vp; }
vec4 vfaGrid() { return MEDIUM_EYE != 0u ? ssr.medium[MEDIUM_EYE].grid : ssr.vol_grid; }
vec4 vfaAlbedo() { return MEDIUM_EYE != 0u ? ssr.medium[MEDIUM_EYE].albedo : ssr.vol_albedo; }
vec4 vfaMisc() { return MEDIUM_EYE != 0u ? ssr.medium[MEDIUM_EYE].misc : ssr.vol_misc; }
vec4 vfaSample(vec3 tc) { return MEDIUM_EYE != 0u ? textureLod(uMediumL[MEDIUM_EYE], tc, 0.0) : textureLod(uVolFog, tc, 0.0); }

// (in-scatter, transmittance) between the grid's eye and world point P.
vec4 volFogAt(vec3 P)
{
    const vec3 rel = P - vfaEye().xyz;
    const vec4 clip = vfaVP() * vec4(rel, 1.0);
    const float vz = clip.w;
    if (!(vz > 1e-4)) return vec4(0.0, 0.0, 0.0, 1.0);
    const vec2 uv = clamp(clip.xy / vz * 0.5 + 0.5, vec2(0.0), vec2(1.0));
    const float D = vfaGrid().z;
    const float far = volFar();
    // Stored index i = the integral up to slice i's FAR boundary: depth vz sits at the
    // continuous index c = u(vz) D - 1 (texel centre (c + 0.5) / D). In front of the first
    // boundary, blend from the identity (nothing integrated at the eye).
    const float c = volSliceOfDepth(min(vz, far)) * D - 1.0;
    vec4 s = vfaSample(vec3(uv, (max(c, 0.0) + 0.5) / D));
    if (c < 0.0) s = mix(vec4(0.0, 0.0, 0.0, 1.0), s, clamp(c + 1.0, 0.0, 1.0));
    // Past the far plane: the height fog analytically (unshadowed, no local lights / volumes),
    // seen through the grid's transmittance and the haze over [eye, far].
    if (vz > far && vfaAlbedo().w > 0.5)
    {
        const float f = far / vz;
        const vec3 A = vfaEye().xyz + rel * f;
        const float Tf = exp(-volFogOpticalDepth(A, P));
        const float len = length(rel);
        const vec3 V = rel / max(len, 1e-6);
        const float Th = exp(-volHazeExt() * len * f);
        s.rgb += s.a * Th * volFogSource(V) * (1.0 - Tf);
        s.a *= Tf;
    }
    // #954 PRECIPITATION past the far plane (clean_weather_veil.glsl: the froxels carry it inside
    // the grid): uniform, open, below its 1.5 km ceiling, lit as the fog's medium (unshadowed).
    const float sp = ssr.weather[16].x;
    if (vz > far && sp > 0.0)
    {
        const float len = length(rel);
        const vec3 V = rel / max(len, 1e-6);
        float seg = len * (1.0 - far / vz);
        if (V.z > 0.0) seg = min(seg, max(1500.0 / V.z - len * far / vz, 0.0));
        const float Tp = exp(-sp * seg);
        const vec3 L = atmosLightDir();
        vec3 src = volAmbient();
        if (L.z > 0.0) src += ATMOS_PI * volBeam() * volPhaseHG(dot(V, L), 0.3);
        s.rgb += s.a * src * mix(0.85, 0.95, ssr.weather[16].z) * (1.0 - Tp);
        s.a *= Tp;
    }
    return s;
}

vec3 volFogApply(vec3 C, vec3 P)
{
    if(cmOn()) return cmApply(C,cmAt(P,CLEAN_VIEW));
    if (!volOn()) return C;
    const vec4 f = volFogAt(P);
    return C * f.a + f.rgb;
}

// The inverse (water refraction: the solid scene behind the surface already took the
// air segment in front of it -- the extra fog is zero below the water, so P's (L, T) is
// what the scene sample got, up to the refraction's screen offset).
vec3 volFogRemove(vec3 C, vec3 P)
{
    if(cmOn()) return cmRemove(C,cmAt(P,CLEAN_VIEW),atmosSrgbToLinear(ssr.water[4].rgb));
    if (!volOn()) return C;
    const vec4 f = volFogAt(P);
    return max(C - f.rgb, vec3(0.0)) / max(f.a, 1e-3);
}

#endif
