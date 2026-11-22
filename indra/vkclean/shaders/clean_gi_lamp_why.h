// vkclean -- RenderVKGIDebug 11 (GI_DBG_LAMP): WHY A GI HIT'S LAMP RAY PASSED. Shared by clean_deferred_light.comp
// (the GI trace, the visibility queue's walk) and clean_vis_walk.glsl (the walk notes its reasons under
// RT_WALK_WHY), and by the CPU fixture scripts/check/test_gi_lamp_why.cpp (scripts/check/checks/gi_lamp_why.py).
//
// A GI hit's local lights reach it only through shadow rays (RenderVKGIHitPointShadows; clean_hit_nee.h). Every
// such ray that comes back with light (visibility > 0) carries the rules that let it pass, collected while it
// walked (rt_walk_why):
//   GLW_PASSED  the ray came back with light (set by its caller; the other bits mean nothing without it)
//   GLW_FIX     the light's OWN FIXTURE rule: a caster holding the light (or the light's own prim) ended a walk
//               and the ray was traced again past it (rtWalkBlocked's box entry, rtFixNext), or a fixture
//               candidate was passed over in its stage 2
//   GLW_ALPHA   a candidate passed its ALPHA TEST: a hole in an alpha-mask cut-out, a translucent / thin blend
//               caster, a blend layer lying on the hit (RT_LAYER_SKIP)
//   GLW_FACING  the FACING rule: a single-sided rigged / worn / leaf face seen from behind passed (RT_FOL_PASS /
//               a transmissive leaf), or -- re-traced in the debug view with the facing cull off -- a face the
//               hardware cull dropped would have blocked it
//   GLW_SELF    the no-self-shadow rule: a throttled character's own stale geometry (rtSelfCandidate 1)
//   GLW_EMIT    a qualified emitter's own geometry (rtEmissiveNoShadow)
//   (no reason bit) CLEAN: nothing on the ray's segment at all -- the light is in plain view from the hit
// The view (gi.cfg.w == GI_DBG_LAMP) shows, per GI texel whose FRESH hit received lamp light through such a ray,
// the colour of its first reason by GLW_PRIORITY; every other texel dim grey (its GI).
#ifndef CLEAN_GI_LAMP_WHY_H
#define CLEAN_GI_LAMP_WHY_H
#ifdef __cplusplus
#define GLW_FN inline
#else
#define GLW_FN
#endif
#define GLW_PASSED 1u
#define GLW_FIX    2u
#define GLW_ALPHA  4u
#define GLW_FACING 8u
#define GLW_SELF   16u
#define GLW_EMIT   32u
#define GLW_MASK   63u
// The legend (first match wins): FIX magenta, SELF orange, EMIT blue, FACING cyan, ALPHA yellow, CLEAN white;
// 0: no lamp light passed (the caller draws the texel's GI dim grey).
GLW_FN vec3 giLampWhyColour(uint why)
{
    if ((why & GLW_PASSED) == 0u) return vec3(0.0f);
    if ((why & GLW_FIX) != 0u)    return vec3(1.0f, 0.0f, 1.0f);
    if ((why & GLW_SELF) != 0u)   return vec3(1.0f, 0.5f, 0.0f);
    if ((why & GLW_EMIT) != 0u)   return vec3(0.0f, 0.3f, 1.0f);
    if ((why & GLW_FACING) != 0u) return vec3(0.0f, 1.0f, 1.0f);
    if ((why & GLW_ALPHA) != 0u)  return vec3(1.0f, 1.0f, 0.0f);
    return vec3(1.0f, 1.0f, 1.0f);
}
// RenderVKGIDebug 11: one GI texel's colour. outlier: its estimate is a luminance outlier of its block (the speck
// test); src: where its estimate's sample came from (0 this frame's fresh ray, 1 last frame, 2 a neighbour); why:
// its fresh hit's lamp-ray reasons. An outlier at FULL intensity: its fresh hit's lamp reason (giLampWhyColour) when
// the fresh sample is the one shown and lamp light passed, RED a fresh sample without lamp light through a ray
// (sun, sky, cache, emission), GREEN a REUSED sample (last frame's or a neighbour's: the speck's light came from
// another texel / frame). Not an outlier: its lamp reason at a fifth (where lamp rays pass at all), else BLACK.
GLW_FN vec3 giLampSpeckColour(bool outlier, uint src, uint why)
{
    const bool lamp = (why & GLW_PASSED) != 0u;
    if (outlier)
    {
        if (src != 0u) return vec3(0.0f, 1.0f, 0.0f);
        return lamp ? giLampWhyColour(why) : vec3(1.0f, 0.0f, 0.0f);
    }
    return lamp ? giLampWhyColour(why) * 0.2f : vec3(0.0f);
}
#endif
