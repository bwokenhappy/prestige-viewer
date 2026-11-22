// vkclean -- THE FORWARD RT REFLECTIONS' RESOLVE (clean_deferred_light.comp pass 14, RenderVKRTReflectionsForward): how
// the front-most alpha-blend / water layer's traced reflection is accumulated over frames. Shared by the shader and
// the CPU fixture scripts/check/test_rt_fwd_temporal.cpp (scripts/check/checks/rt_fwd_temporal.py), so the rule the
// check runs is the rule the GPU runs.
//
// THE NOISE IT REMOVES: pass 14 traces ONE mirror ray per 2 x 2 block, through the block corner of the JITTERED
// camera (deferInvVP). The opaque resolve (pass 13) has a lattice gather, a lobe filter and a history
// (rtrTemporal). The forward reflection had none: each frame's block rays went to the forward FS raw. On a layer with
// a normal map (a "water" prim, rippled glass), the subpixel jitter moves each block's ray to another normal, so its
// reflection jumps between different parts of the room from frame to frame. That is the grain, and the reflection
// looked "held back, not filling in the picture". (The TAA / DLSS RR cannot hold it: RR treats blend as TRANSP and
// only upscales it, and a reflection does not move with its surface.)
//
// THE RESOLVE (per half-resolution texel, fixed cost: the 3 x 3 neighbourhood from the workgroup's shared tile + one
// history fetch + one variance fetch):
//   * cur: this frame's block ray; mu / sigma: the mean / deviation of the 3 x 3 neighbours on the SAME layer (view
//     depth within the rtFwdIn tolerance), this texel included;
//   * the history: the previous frame's resolved texel where this layer point (or, on a smooth flat layer, its
//     reflection's virtual image) was, accepted on its stored view depth (pass 14's rftReproject), with its TEMPORAL
//     VARIANCE and length -- the opaque reflections' rule, clean_rt_refl_temporal.h rrtResolve: per channel, in log
//     space, updated every frame at a fixed rate (a reset included); a per-channel anti-lag at RRT_TEST deviations;
//     a running mean capped at RFT_FRAMES. The 3 x 3 alone cannot describe a rippled reflection with a small bright
//     window in it (in a frame where none of the 9 rays met the window its box excluded the true mean: the fixture,
//     18 % too dark); the texel's own variance remembers that the window is part of its reflection;
//   * COHERENT NOISE (the window flicker, 2026-11-09): what the reflection's hits read from the world-space radiance
//     cache (their ambient; a 0.25 m cell is tens of half-resolution texels) changes from frame to frame for a whole
//     region at once -- every one of the 9 rays agrees (sigma 0), so only time can tell the noise. The old rule kept
//     a luminance moment blended at the history's weight and centred on the RESOLVED colour: one anti-lag reset set it
//     to this frame's square, the next frame's spread was 0, its box [cur, cur] and its tolerance 2 % of the light --
//     so it reset again: a blob 5 % noisy came through at 60 % of its frame-to-frame change, 20 % noisy at 95 % (the
//     fixture's COHERENT case). The log variance (never restarted by a reset) knows the noise, and a light switched
//     OFF -- a drop by a factor -- still resets (CHANGE OFF; an absolute spread ghosted it: pass 13's finding);
//   * no history (a disocclusion, the first frame, a camera cut): the spatial step alone, this frame's 3 x 3 mean
//     with its own ray (RFT_SPATIAL_SELF), until the history builds; the variance seeded (rrtResolve's rule).
// The same resolve serves pass 14's second channel, the front layer's radiance-cache irradiance (rftResolve's
// history weight wHist also blends its cover; clean_deferred_light.comp pass 14, THE LAYER'S GI).
#ifndef CLEAN_RT_FWD_TEMPORAL_H
#define CLEAN_RT_FWD_TEMPORAL_H
#ifdef __cplusplus
#define RFT_FN inline
#define RFT_OUT(T) T&
#else
#define RFT_FN
#define RFT_OUT(T) out T
#endif
#include "clean_rt_refl_temporal.h"
// uRtFwd's rows, in half-resolution heights h (pass 14 writes, the forward FSs read; the host allocates RFT_REGIONS h,
// LLCleanRendererVK mDsRtFwd): [0, h) the resolved reflection (rgb, a the layer's view depth: > 0 a traced
// reflection, < 0 -- minus the depth -- a blend layer without one), [h, 3h) its history by parity, [3h, 5h) its
// temporal variance (rgb) and length (a) by parity, [5h, 6h) the resolved layer GI (rgb the cache irradiance x its
// cover, a the cover; a < 0: no GI read), [6h, 8h) its history by parity, [8h, 10h) its variance / length by parity,
// [10h, 11h) THE LAYER'S LIGHT (RenderVKBlendFrontLight: r the sun / moon visibility, g the local lights' shadowed /
// unshadowed ratio, b the occlusion ray's visibility, a the layer's alpha; a <= 0: none), [11h, 13h) its history by
// parity, [13h, 15h) its variance / length by parity.
#define RFT_REGIONS      15
#define RFT_FRAMES       float(8.0)    // the accumulation's cap (frames)
#define RFT_SPATIAL_SELF float(0.5)    // this frame's own ray's share against the 3 x 3 mean
#define RFT_FAR_FADE     float(0.8)    // (#1570) a water hit fades to the miss env (probe / sky) from this share of
                                       // the reflection length to its end: hits and misses meet without a step
#define RFT_SBOX         float(3.0)    // the 3 x 3's spread in the clamp box, x RRT_BOX (one ray of 9: at x 1 a frame
                                       // whose 3 x 3 missed a bright window clipped the history -- STEADY 6 % too dark;
                                       // x 4 kept the glint's history -- 3.3 % too bright)
// The same-layer test of a neighbour / a history texel (rtFwdIn's depth tolerance): view depths a and b (m).
RFT_FN bool rftSameLayer(float a, float b) { return a > float(0.0) && b > float(0.0) && abs(a - b) <= float(0.02) * b + float(0.05); }
// cur: this frame's ray; mu / sigma: the same-layer 3 x 3 mean / deviation (n >= 1 texels, this one included); hist /
// hv / hlen / histOk: the reprojected history, its temporal variance and length. Returns this frame's resolved value,
// (v, len) the next frame's variance and length -- written with it as the next frame's history -- and (wHist) the
// weight this frame took against the history (1: none of it), for a quantity resolved alongside (pass 14's cover).
RFT_FN vec3 rftResolve(vec3 cur, vec3 mu, vec3 sigma, float n, vec3 hist, vec3 hv, float hlen, bool histOk,
                       RFT_OUT(vec3) v, RFT_OUT(float) len, RFT_OUT(float) wHist)
{
    // this frame's estimate: the own ray with the 3 x 3 mean (RFT_SPATIAL_SELF) -- pass 13's cur is its gather, mostly
    // its own ray; a lone ray here against a 9-ray mu reset the texel on every neighbour's glint (the fixture's STEADY
    // glint: 46 % of the raw change and 5 % too bright with the own ray alone)
    const vec3 c = n > float(1.5) ? mix(mu, cur, RFT_SPATIAL_SELF) : cur;
    const vec3 r = rrtResolve(c, mu, sigma * RFT_SBOX, n, hist, hv, hlen, histOk, RFT_FRAMES, v, len);
    wHist = float(1.0) / max(len, float(1.0));
    return r;
}
#endif
