#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require
#extension GL_EXT_nonuniform_qualifier : require
#define CLEAN_SKY_LUT   // (#819) skyColor() reads the sky-view LUT (draw set 0 binding 52) when valid
// FORWARD FROXEL WALK (clean_draw_common.glsl localLights), as the forward lit FS.
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_KHR_shader_subgroup_arithmetic : require
#extension GL_KHR_shader_subgroup_ballot : require
#define CLEAN_FWD_FROXEL
#ifdef CLEAN_PROJ_SHADOW_RAYQUERY
#extension GL_EXT_ray_query : require   // the ray-query twin: the front layer's own sun ray (one per covered pixel)
#endif
// vkclean -- SPLIT TRANSPARENCY, STAGE C: THE FRONT LAYER'S LIGHT (RenderVKBlendSplit; clean_bs_common.glsl,
// LLCleanBlendSplitVK).
//
// A fullscreen triangle over the front surface stage B wrote (F0-F4 + its depth): every pixel with a front layer
// lights that surface ONCE with the forward lighting -- lightSurface() of clean_surface_light.glsl, the code the
// alpha-blend FS, the WBOIT resolve and the deferred kernels share: sun / moon (the ray-query twin traces one ray;
// the plain one the cascades), the clustered local lights (forward froxel walk), the probes, the GI radiance cache
// (the front layer's resolved read of pass 14 where it matches), SSR, the RT forward reflection of pass 14 (its
// half-resolution, temporally resolved reflection of this same front layer), then the alpha tail (surfacePost, the
// froxel fog, the veil). Output: the layer's final colour + its lit coverage (legacy glare), which stage D composites
// in the layer's own place in the blend order. Lit as an alpha-blend surface (v_flags 2: no SSAO, the transparent
// reflection rules), as the forward FS lit it. Position: the front depth through the jittered inverse projection.
// Pixels without a front layer leave at once (one 8-byte read).

#include "clean_view.glsl"
#define CLEAN_DEBUG_CONST
const bool kDebugViews = false;   // (no debug twin: the debug views are the module's own, RenderVKBlendSplitDebug)
#define CLEAN_SPEC_SETTINGS       // graphics-setting specialization constants 8-17 (the live key's)
#define CLEAN_COMPUTE_LIGHT       // no vertex inputs (clean_draw_common.glsl) ...
#define CLEAN_COMPUTE_V_FLAGS 2u  // ... and v_flags = alpha-blend: the forward alpha FS's lighting rules
#define CLEAN_BS_LIGHT            // (clean_draw_common.glsl: the RT forward reflection reader, as the forward FS)
#ifdef CLEAN_PROJ_SHADOW_RAYQUERY
// THE LAYER'S OWN LIGHT (RenderVKBlendHitLighting / RenderVKBlendFrontLight, clean_draw.frag CLEAN_BLEND_HIT): the front
// layer is a real surface -- pass 14's resolved terms where they match it (blendFrontIn), else its own hit-style walk
// and occlusion ray, as the sorted FS lit it; once per pixel here.
#define CLEAN_BLEND_HIT
#endif
// SUN VISIBILITY (clean_surface_light.glsl hook): bsSunVis, at g_bs_P / g_bs_Ng.
#define CLEAN_SUN_VIS_EXTERNAL bsSunVis
#include "clean_draw_common.glsl"
#define CLEAN_GI_FORWARD          // (#87) GI for the forward lit surfaces, as the forward lit FS
#define GI_STATE_SET 0
#define GI_STATE_BINDING 53
#define GI_STATE_ACCESS readonly
#define GI_STATE_READONLY
#include "clean_gi_common.glsl"
#define BS_READ_FRONT
#include "clean_bs_common.glsl"

vec3 g_bs_P = vec3(0.0);
vec3 g_bs_Ng = vec3(0.0, 0.0, 1.0);
// The front layer's sun / moon visibility: the ray-query twin traces (sunShadowT: one ray, the forward alpha FS's
// receiver rules); the plain twin takes the cascades, and in a traced frame (no cascades rendered; the RT shadow
// mask holds the OPAQUE surface behind the layer) the clouds only.
float bsSunVis(out vec3 tint)
{
    tint = vec3(1.0);
#ifndef CLEAN_PROJ_SHADOW_RAYQUERY
    if (specSunOn() && specSunTraced()) return cloudShadowMap(g_bs_P);
#endif
    return sunShadowT(g_bs_P, g_bs_Ng, tint);
}

#include "clean_surface_light.glsl"
#include "clean_gbuffer.glsl"       // (gbOctDecode)
#include "clean_vol_apply.glsl"     // volumetric fog consumer (#718)
#include "clean_weather_veil.glsl"  // precipitation veil consumer (#954)

layout(location = 0) out vec4 outLit;

void main()
{
    const ivec2 px = ivec2(gl_FragCoord.xy);
    const uvec2 key = texelFetch(uBsKey, px, 0).xy;
    if ((key.x & BSK_VALID) == 0u) discard;
    const float z = texelFetch(uBsDepth, px, 0).r;
    const vec4 f0 = texelFetch(uBsF0, px, 0);
    const vec4 f1 = texelFetch(uBsF1, px, 0);
    const vec4 f2 = texelFetch(uBsF2, px, 0);
    const vec4 f3 = texelFetch(uBsF3, px, 0);
    const vec2 res = vec2(float(bs.flags.w & 0xFFFFu), float(bs.flags.w >> 16u));
    // (camera-relative, as the deferred lighting rebuilds a pixel: the module's inverse of the unjittered main
    //  view-projection -- the jitter moves the ray by under half a pixel; a world-space float inverse would not hold
    //  the position far from the origin)
    const vec4 h = bs.ivp * vec4((vec2(px) + 0.5) / res * 2.0 - 1.0, z, 1.0);
    const vec3 P = bs.cam.xyz + (abs(h.w) > 1e-12 ? h.xyz / h.w : vec3(0.0));
    SurfaceLit s = surfaceInit();
    s.model = (key.x >> 18u) & 3u;
    s.cel = ((key.x >> 20u) & 1u) != 0u;
    s.skin = ((key.x >> 21u) & 1u) != 0u;
    s.albedo = f0.rgb;
    s.N = gbOctDecode(f1.xy);
    s.Ng = gbOctDecode(f1.zw);
    if (s.model == SURF_PBR)
    {
        s.rough = f2.x; s.metal = f2.y; s.ao = f2.z; s.emissive = f3.rgb;
    }
    else if (s.model == SURF_LEGACY)
    {
        s.gloss = f2.x; s.env = f2.z; s.glareA = f2.w; s.spec = f3.rgb;
        if (((key.x >> 22u) & 1u) != 0u) { s.emissive = f3.rgb; s.spec = vec3(0.0); }   // <GMDX> rez effect emission
    }
    else s.env = f2.z;
    g_bs_P = P;
    g_bs_Ng = s.Ng;
    const vec4 lit = lightSurface(s, P, f0.a);
    vec3 c = lit.rgb;
    // The forward alpha FS's tail (clean_draw.frag main): the post (fullbright takes its haze here), the froxel fog,
    // the veil.
    g_medium_inline = true;
    c = surfacePost(s.model == SURF_FULLBRIGHT ? SURF_FULLBRIGHT : SURF_LEGACY, false, c, P);
    if (!cmOn() && volOn()) c = volFogApply(c, P);
    c = weatherVeilApply(c, P);
    c = lgFinite(c, LG_OUT);
    outLit = vec4(c, clamp(lit.a, 0.0, 1.0));
}
