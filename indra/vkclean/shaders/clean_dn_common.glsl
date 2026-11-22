// vkclean — DIFFUSE / SPECULAR DENOISER passes (#85 phase 2; LLCleanDenoiserVK): the pipeline
// layout both passes share -- set 0 = the draw set 0 (only the frame info is read: the motion /
// reset / jitter state, clean_frame_info.glsl), set 1 = the denoiser's own images
// (clean_pdn_common.glsl IMAGES), one push block.
//   clean_dn_temporal.comp  reprojection + history validation + clamp / anti-lag + accumulation
//   clean_dn_atrous.comp    one variance-guided a-trous iteration per dispatch
// (#1608) LAYERED STEREO: the passes run over the N views of a stereo frame in one dispatch, z = N (the kernels
// #define CLEAN_VIEW CLEAN_VIEW_Z before this include; their stereo pipelines carry kViewCount = N, spec constant 20 --
// LLCleanDenoiserVK::init's views). Every image holds its layers once per view, INTERLEAVED: layer l of view v at
// l x N + v (DN_L; the split deferred lighting's projector planes and RT reflection history do the same), so the N
// views of one layer are contiguous (the split RAW snapshot copies them in one region). MOT is a layer per view (DN_MOT),
// the motion vectors (binding 5) are the pre-pass's 2D_ARRAY at the view's layer. A layer count read from an image is
// divided by N (DN_LAYERS). One view (the desktop, the noise probe): l, layer 0, the counts of before.
#include "clean_frame_info.glsl"
#include "clean_pdn_common.glsl"
#include "clean_history_time.glsl"
#define DN_L(l) (int(l) * kViewCount + int(CLEAN_VIEW))
#define DN_MOT(q) ivec3((q), int(CLEAN_VIEW))
#define DN_LAYERS(img) (imageSize(img).z / kViewCount)

layout(set = 1, binding = 0, rgba16f) uniform image2DArray uDnIn;     // producer's raw; a-trous scratch
layout(set = 1, binding = 1, rgba32ui) uniform uimage2DArray uDnMot;  // camera-only reprojection + expected depth
layout(set = 1, binding = 2, rgba32ui) uniform uimage2DArray uDnGeom; // guides, per parity
layout(set = 1, binding = 3, rgba32ui) uniform uimage2DArray uDnHist; // accumulated colour + variance, per parity
layout(set = 1, binding = 4, rgba16f) uniform image2DArray uDnOut;    // temporal output / a-trous ping-pong
layout(set = 1, binding = 5) uniform sampler2DArray uDnMotion;        // pre-pass motion vectors (render extent, a layer per view)
// (THE NARROW AGE: DN_AGE_NARROW, the GI instance's variant -- LLCleanDenoiserVK::init(.., narrowAge)) the GI never
// has the sun / moon channel, the split channel or the radiance mover record, so its age word is x alone (the
// diffuse / specular ages): AGE is r32ui there (4 B per layer, not 16). dnSunOn / dnSplitOn / dnMoverRecordOn are
// constant false in that variant, so nothing reads or writes y / z / w; the host never gives it a sun / split.
#ifdef DN_AGE_NARROW
layout(set = 1, binding = 6, r32ui) uniform uimage2DArray uDnAge;     // the diffuse / specular ages only, per parity
#else
layout(set = 1, binding = 6, rgba32ui) uniform uimage2DArray uDnAge;  // the history ages + mover record, per parity
#endif
layout(set = 1, binding = 7, rgba32ui) uniform uimage2DArray uDnSunHist;   // the sun / moon channel's history, per parity
layout(set = 1, binding = 8, rgba16ui) uniform uimage2DArray uDnSplitAge;  // the split slots' ages, per parity
// THE LEAN HISTORY (per-pixel memory; LLCleanDenoiserVK::HistMode): HIST (binding 3) holds ONLY the accumulated
// diffuse / specular colours, SUNHIST (binding 7) only the sun / moon channel's -- the images the host may leave out
// while DLSS Ray Reconstruction takes that signal RAW (no reader of them then: dnRawView / dnSunRaw). AGE (binding 6)
// holds what every frame reads, raw or not: the ages + mover record (layer parity); SPLITAGE (binding 8) the split
// slots' ages (layer parity; the 1 x 1 placeholder without the split channel -- every access is behind dnSplitOn).
//   dnColourHist: HIST is at the denoiser extent (lean: the 1 x 1 placeholder);
//   dnSunHistImg: SUNHIST is (without the sun channel or lean: the 1 x 1 placeholder).
// Without one, every READ of it is off (dnHistOk / dnSunHistOk) and every STORE skipped: the first frame after RR
// stops runs without that history (the host re-allocates it before the next) -- what such a frame started from
// anyway (a raw frame stores a length-1 history).
bool dnColourHist() { return all(equal(imageSize(uDnHist).xy, imageSize(uDnGeom).xy)); }
bool dnSunHistImg() { return all(equal(imageSize(uDnSunHist).xy, imageSize(uDnGeom).xy)); }

// 96 B (LLCleanDenoiserVK::Push).
layout(push_constant) uniform DnPush {
    vec4  dims;   // denoiser w, h; render w, h
    vec4  prm0;   // x diffuse max history (frames), y specular max history, z clamp width (sigmas),
                  // w luminance edge-stop (sigmas, a-trous)
    vec4  prm1;   // x depth edge-stop, y diffuse normal power, z W change that cuts the history
                  // (relative; 0 = off), w the shortest specular history (a mirror's, frames)
    uvec4 ctl;    // x flags (DN_F_*), y temporal flags OR a-trous iteration k; spatial z N/w frame.
                  // Temporal sample-grid mode reuses zw for previous jitter fp32 bits.
    // SUN / MOON CHANNEL (DN_F_SUN; clean_pdn_common.glsl SUN CHANNEL):
    vec4  sun0;   // x longest history (frames), y moving-caster hard history, z moving-caster
                  // longest history (0 = no cap), w the light's step since last frame (disc radii)
    vec4  sun1;   // x a-trous penumbra radius cap (texels), y moving-caster box width (sigmas),
                  // z moving-caster countdown (frames), w the disc rays per texel (0: sun_rt.y)
} dp;
#define DN_F_HIST   1u    // the history is valid (the denoiser ran last frame at this size)
#define DN_F_PARITY 2u    // this frame's GEOM / HIST layer (the producer wrote GEOM layer parity)
                          // (bit 2: DN_F_YOUNG_BOOST, bit 31: DN_F_MOVE_ZTOL -- below, #1038;
                          //  bit 3: DN_F_RADIANCE -- below)
// (RenderVKReSTIRRadiance) RADIANCE SIGNAL from a RESERVOIR-REUSING sampler (the pass-15 instance in radiance
// mode; LLCleanDenoiserVK::Params::radiance -- the GI instance and the ratio path never set it): one pick per
// texel of the SHADOWED light, its reservoir handed to a spatial neighbourhood (ReSTIR 5c, ~12 texels) and kept
// for up to the M cap in frames (5b). The 3x3 of this frame's raw samples is then NOT nine independent
// samples: on a flat lit surface the whole 3x3 often holds the same pick, its sigma ~0, and the neighbourhood
// clamp dragged the history onto that one pick and cut its length (dnClamp: <= 8 frames, down to 1) -- a new
// low-pass of a new pick every frame: the low-frequency blotches that CRAWL. With this bit the clamp box's
// sigma is floored at the history's own per-sample spread (the accumulated luminance variance, which no
// spatial correlation collapses; per channel in proportion to the colour, so a coloured light's chroma noise
// is inside the box too). A real change still leaves that box (a light switched off, a shadow arriving) and
// cuts the history as before; noise from the same pick across the neighbourhood no longer does.
#define DN_F_RADIANCE 8u
bool dnRadiance() { return (dp.ctl.x & DN_F_RADIANCE) != 0u; }
// Temporal pass only. Host recordSpatial overwrites ctl.y with each iteration.
bool dnShadowPlane() { return (dp.ctl.y & 512u) != 0u; }
// RenderVKShadowCasterHistory: 0 off, 1 old broad cap, 2 visibility delta, 3 visibility delta + THE MOVER RECORD
// (clean_dn_temporal.comp). Policy 3 runs every policy-2 rule (dnCasterPolicy() reads 2) and adds the record.
uint dnCasterPolicyRaw() { return (dp.ctl.y >> 7u) & 3u; }
uint dnCasterPolicy() { return min(dnCasterPolicyRaw(), 2u); }
#ifdef DN_AGE_NARROW
bool dnMoverRecordOn() { return false; }   // (THE NARROW AGE: no record word; the GI is never a radiance signal)
#else
bool dnMoverRecordOn() { return dnRadiance() && dnCasterPolicyRaw() == 3u; }
#endif
bool dnRadianceMoveReset() { return dnRadiance() && (dp.ctl.y & 1u) != 0u; }
bool dnLocalReceiverGuides() { return (dp.ctl.y & 2u) != 0u; } // temporal only; never GI
bool dnSkinnedVarianceRecovery() { return dnRadiance() && (dp.ctl.y & 4u) != 0u; } // temporal only
// Local shadows, and the GI with RenderVKGIHistoryJitter. Temporal ctl.zw carry previous raster
// jitter as two fp32 bit patterns; recordSpatial restores iteration count/frame.
// Bit 5 survives in both temporal flags and spatial iteration words.
bool dnSpecularLobeGuard() { return dnRadiance() && (dp.ctl.y & 32u) != 0u; }
bool dnSampleGridHistory() { return (dp.ctl.y & 16u) != 0u; }
// (RenderVKShadowMoverMask 2, local radiance only; temporal AND a-trous ctl.y bit 12) THE EXACT-MOVER RULE: the temporal
// pass caps the history at sun0.z (RenderVKRTShadowDynamicMaxHistory) where the producer's mover countdown runs (MOT w
// pdnMotMoverCd: a ray met a mover now or within it -- never on exact-set membership alone, nothing cut on entry / exit);
// the a-trous gives an exact centre (MOT w PDN_MOT_EXACT_SET) only exact taps.
#define DN_T_MOVER_EXACT 4096u
bool dnMoverExact() { return dnRadiance() && (dp.ctl.y & DN_T_MOVER_EXACT) != 0u; }
// (radiance, temporal ctl.y only; clean_dn_temporal.comp THE CONVERGENCE RULE) bits 14-17 THE MOVER COUNTDOWN's length
// (max(RenderVKRTShadowMoverPersist, RenderVKRTShadowDynamicMaxHistory); 0 = off), bits 18-21 the history cap while it
// runs (RenderVKRTShadowDynamicMaxHistory) -- every RenderVKShadowMoverMask mode, not only 2
#define DN_T_MOVER_CD_SHIFT 14u
#define DN_T_MOVER_CAP_SHIFT 18u
#ifdef DN_TEMPORAL_PASS
uint dnMoverCdLen() { return dnRadiance() ? (dp.ctl.y >> DN_T_MOVER_CD_SHIFT) & 15u : 0u; }
float dnMoverCdCap() { return float((dp.ctl.y >> DN_T_MOVER_CAP_SHIFT) & 15u); }
#endif
// (radiance, temporal ctl.y only; RenderVKShadowFixMoverVariance A/B -- clean_dn_temporal.comp THE COUNTDOWN'S NOISE) bits
// 22-23: 0 off (the countdown withholds its deviation from the variance), 1 a steady texel's deviation floors OUT (the
// a-trous's stop) this frame, 2 it feeds the history's variance (HIST)
#define DN_T_MOVER_VAR_SHIFT 22u
#ifdef DN_TEMPORAL_PASS
uint dnMoverVarFix() { return dnRadiance() ? (dp.ctl.y >> DN_T_MOVER_VAR_SHIFT) & 3u : 0u; }
#endif
// (radiance, temporal ctl.y only; RenderVKProjStochasticAdaptiveHistory -- clean_dn_temporal.comp THE ADAPTIVE MOVER
// HISTORY) bits 24-26: the adaptive extra passes A the producer ran on its flagged tiles this frame (0 = the rule off): a
// texel that took them (MOT w PDN_MOT_EXTRA_RAYS) while its mover countdown runs keeps cap / (1 + A) frames
#define DN_T_ADAPT_SHIFT 24u
#ifdef DN_TEMPORAL_PASS
uint dnAdaptPasses() { return dnRadiance() ? (dp.ctl.y >> DN_T_ADAPT_SHIFT) & 7u : 0u; }
#endif
// The countdown in the AGE word's mover record (y bits 28-31; THE MOVER RECORD uses 0-27)
#define DN_REC_CD_SHIFT 28u
uint dnRecCd(uint rec) { return rec >> DN_REC_CD_SHIFT; }
// (radiance) an EXACT texel this frame: every candidate traced -- its input is the answer (THE CONVERGENCE RULE)
bool dnExactMot(uint w) { return dnRadiance() && (w & PDN_MOT_EVIDENCE_EXACT) != 0u; }
// (GI only, the sealed-room fade) the producer's MOT w bit 8 marks a texel whose radiance-cache cell is CHANGED
// (clean_gi_common.glsl GI_MOT_CHANGED): its diffuse history is capped at DN_GI_CHANGED_HIST frames
bool dnGiChangedCut() { return (dp.ctl.y & 1024u) != 0u; }
#define DN_GI_CHANGED_HIST 2.0
// (GI only, temporal only: RenderVKGIStableSamples, clean_gi_common.glsl GI_F_STABLE_SAMPLES) the producer's texels
// sampled the FIXED lattice floor((m + 0.5) r): no sample phase (dnJitter 0: the render pixel whose motion vector the
// texel follows)
#define DN_F_GI_STABLE 2048u
#ifdef DN_TEMPORAL_PASS
bool dnGiStable() { return (dp.ctl.y & DN_F_GI_STABLE) != 0u; }
#else
bool dnGiStable() { return false; }   // (ctl.y is the a-trous iteration there; it never maps texels to pixels)
#endif
vec2 dnSampleHistoryHc(vec2 hc)
{
    const vec2 r = dp.dims.zw / dp.dims.xy;
    const vec2 previousJitter = uintBitsToFloat(dp.ctl.zw);
    const vec2 gridJitter = dp.dims.x < dp.dims.z ? previousJitter : vec2(0.0);
    return pdnSampleGridHc((hc + 0.5) * r + previousJitter, r, gridJitter);
}
// ... and (clean_dn_atrous.comp YOUNG-HISTORY FEEDBACK) a texel whose history is shorter than this many frames takes
// the first a-trous iteration's output as its history colour
#define DN_RAD_FEED_LEN 8.0
// The radiance clamp's sigma floor: the per-sample luminance spread sqrt(var) carried to each channel in
// proportion to the larger of the two estimates' colour (grey: the same in every channel).
vec3 dnRadSpread(float var, vec3 a, vec3 b)
{
    const float sd = sqrt(max(var, 0.0));
    const vec3 c = max(a, b);
    const float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    return l > 1e-6 ? sd * c / l : vec3(sd);
}
#define DN_DBG_SHIFT 4u   // bits 4-7: the debug view (PDN_DBG_*; 8 / 9 the #869 split views)
#define DN_F_SUN       256u    // the producer wrote the sun / moon channel (IN layer 2)
#define DN_F_SUN_HIST  512u    // ... and last frame's sun history is valid (no sun <-> moon cut)
#define DN_F_SUN_HARD  1024u   // hard sun (no disc: one ray per pixel)
#define DN_F_SUN_EXACT 2048u   // exact history reprojection (RenderVKRTMaskExactReproj)
// NO-INFORMATION SAMPLES (the projector WALL-BLEED fix, RenderVKProjStochBleedFix; the producer's
// PDN_OPT_BLEED, clean_deferred_light.comp): a texel without a ray-traced projector candidate (W 0)
// or whose picks carried no light this frame (IN layer 0 a < 0 diffuse, layer 1 a < 0 specular)
// says nothing about visibility -- its ratio 1 is a default, not "lit". With this bit such a
// frame is a WEIGHT-0 SAMPLE of the ratio estimator, never a reason to drop the texel:
//   * temporal: the history holds at its length (no accumulation); with no usable history the
//     informative same-surface 3x3 mean seeds it at length 1 (a real estimate); only a texel
//     with neither stays UNKNOWN (length 0, its value the default);
//   * the 3x3 clamp box is built from informative raw samples only, widened by the bounded
//     [0, 1] ratio's Bernoulli spread when few are informative (no collapsed box resetting the
//     history every frame);
//   * history / a-trous / upsample taps are NOT rejected: a texel's value is the history or the
//     seeded estimate. Only UNKNOWN texels are down-weighted (DN_UNKNOWN_W), and an unknown
//     a-trous centre takes its neighbours' geometry-stopped mean (no luminance stop anchored on
//     the default). Leakage across walls is stopped by the geometric stops (depth plane +
//     normal) every tap already has, and by the producer's facing gate (PDN_OPT_FACING).
// Without this bit the 1s are real samples and spread through the filters to receivers behind walls.
#define DN_F_INFO 4096u
// (#869 / #85) ReSTIR-CONVERGED DIFFUSE: bits 13-15 = the a-trous iterations a converged texel's
// diffuse takes (GEOM meta bit 15, set by the temporal pass: a reused reservoir with M >= DN_RS_M
// and a diffuse history of >= DN_RS_M frames); 0 = every texel takes all of them.
#define DN_CONV_SHIFT 13u
#define DN_RS_M 8.0 // actual reservoir sample confidence, never milliseconds
#define DN_CONV_AGE 8.0 // minimum history duration in 60 Hz reference units
uint dnConvIters() { return (dp.ctl.x >> DN_CONV_SHIFT) & 7u; }
// (#869) THE MOVING-LIGHT RULE (RenderVKReSTIRMoveCut; clean_regir.glsl RS_MOVE_*): bits 16-19 = the
// history (frames) of a texel whose light MOVED this frame -- the producer's MOT w = the moved
// lights' share of its target x 255; 0 = the rule is off (every other producer writes w = 0).
#define DN_MOVE_SHIFT 16u
// #870 RAW SIGNAL ROUTING (RenderVKDlssRawSignals, while DLSS Ray Reconstruction reconstructs):
// the consumer wants this frame's RAW samples -- no history, no a-trous filter, exactly the
// PDN_DBG_RAW pass-through but without its debug false colour or the consumer's debug view.
// DN_F_RAW: the diffuse / specular channels (projectors; GI) -- and (#869) the split lights' own
// visibility with them (one ray each this frame; a slot without information passes as 1, its light
// has no term there): with RenderVKProjStochasticSplitK 0 exactly the old raw routing;
// DN_F_RAW_SUN: the sun / moon channel.
// The last a-trous iteration still writes the view depth into alpha (the consumer's upsample).
#define DN_F_RAW     (1u << 20)
#define DN_F_RAW_SUN (1u << 21)
uint dnMoveHist() { return (dp.ctl.x >> DN_MOVE_SHIFT) & 15u; }
// (#869) MOVING SURFACES (RenderVKReSTIRSurfaceMoveVeto; bits 22-25 = the threshold in quarter
// denoiser texels, 0 = off): a texel whose surface moved on its own since last frame -- the motion
// vector's reprojection lands more than this far from the camera-only one (an animated avatar /
// animesh, a moving prim) -- never takes the ReSTIR-converged shortcut. Its reservoir's M is the
// light SELECTION's confidence (the pick is re-weighed and re-traced every frame), not a sign that
// the visibility converged: a static lamp's shadow slides over a breathing / walking body in
// surface space, which the moving-LIGHT rule (a light that moved) never sees.
#define DN_SURF_MOVE_SHIFT 22u
float dnSurfMoveTexels() { return float((dp.ctl.x >> DN_SURF_MOVE_SHIFT) & 15u) * 0.25; }
// The moving-light rule caps the DIFFUSE history only (the GI denoiser: its specular channel is the
// sky visibility, which no light change touches -- RenderVKGILightChangeHistory).
#define DN_F_MOVE_DIFF_ONLY (1u << 26)
bool dnMoveDiffOnly() { return (dp.ctl.x & DN_F_MOVE_DIFF_ONLY) != 0u; }
// (#87) DISOCCLUSION HISTORY FIX (the GI denoiser, RenderVKGIDisoccFix; bits 27-29 = N frames, 0 = off):
// a texel whose history is shorter than N frames (a disocclusion: camera zoom / pan, silhouettes of
// hair strands and limbs) holds a 1..N-sample estimate. SVGF / ReBLUR-style spatial boost, fading as
// the history grows: the temporal pass floors its variance at a few-sample estimate's (~mean^2 / n:
// the a-trous luminance stop no longer anchors on its own outlier), and the a-trous weights its taps
// by their history length relative to the centre's (up to DN_HFIX_MAXW: converged neighbours fill
// it) with the luminance stop widened. Texels at N frames or more: exactly as before.
#define DN_HFIX_SHIFT 27u
#define DN_HFIX_MAXW 8.0
float dnHistFix() { return float((dp.ctl.x >> DN_HFIX_SHIFT) & 7u); }
// youth in [0, 1]: 1 - len / N below N frames, 0 at N or more (and with the fix off)
float dnYouth(float len) { const float n = dnHistFix(); return n > 0.0 ? clamp(1.0 - len / n, 0.0, 1.0) : 0.0; }
// ... measured against the texel's own base history when that is shorter than N (a history settled at a base of 4 with
// N = 4 is done filling, not young; a base under N otherwise kept the youth rules on forever). Base >= N, or not the
// local radiance (the GI / ratio instances keep their rules exactly): dnYouth.
float dnYouthTo(float len, float base)
{
    const float n = dnRadiance() ? min(dnHistFix(), max(base, 1.0)) : dnHistFix();
    return dnHistFix() > 0.0 ? clamp(1.0 - len / n, 0.0, 1.0) : 0.0;
}
// ... and the other half: a SETTLED centre's tap younger than N frames counts len / N (at least
// 1 / DN_HFIX_MAXW) -- the young outlier's floored variance widens its settled neighbours' stop too
// (the 3x3 variance prefilter), which must not pull its sparkle into them (a halo). 1 with the fix
// off, at N frames or more, and for an unknown tap (dnConf has it).
float dnMaturity(float len)
{
    const float n = dnHistFix();
    return (n > 0.0 && len > 0.0) ? max(min(len / n, 1.0), 1.0 / DN_HFIX_MAXW) : 1.0;
}
// (#87) REPROJECTION SEARCH (the GI denoiser, RenderVKGIReprojSearch): when none of the bilinear 2x2
// history taps is the same surface (sub-texel motion across a thin strand / a silhouette at the
// reduced extent: every tap lands beside it), the best-matching texel of the 3x3 around the
// reprojected point (the depth plane, the normal) gives the history instead of a restart.
#define DN_F_REPROJ_SEARCH (1u << 30)
bool dnReprojSearch() { return (dp.ctl.x & DN_F_REPROJ_SEARCH) != 0u; }
// (#1038) YOUNG-TEXEL BOOST (RenderVKDenoiseYoungBoost; acts only with the history fix, DN_HFIX_SHIFT
// > 0): a disocclusion that uncovers a WHOLE surface at once -- an avatar walking out from behind a
// wall, its body revealed in one frame -- has no settled neighbour for the history fix to fill from:
// every tap is as young as the centre, and the a-trous degenerated to plain SVGF on 1-sample texels,
// its normal stop (power 64) cutting nearly every tap on a curved, normal-mapped body. With this bit:
//   * temporal: a texel with NO usable history stores the same-surface 3x3 mean of this frame's raw
//     samples (the centre included; informative ones only under DN_F_INFO) as its first estimate
//     (length 1) instead of its own single sample -- the history starts ~9x less noisy and the
//     clamp box of the next frames is not anchored on an outlier;
//   * a-trous: a young centre's normal stop relaxes toward power 8 with its youth (indirect light is
//     smooth over a body's curvature at this resolution; the depth plane still stops silhouettes) and,
//     from the second iteration on, its step doubles (a wider footprint without another dispatch).
// Texels at or past the history-fix length: exactly as before (no steady-state cost or change).
#define DN_F_YOUNG_BOOST 4u
bool dnYoungBoost() { return (dp.ctl.x & DN_F_YOUNG_BOOST) != 0u && dnHistFix() > 0.0; }
// (#1038) MOVING-SURFACE DEPTH TOLERANCE (RenderVKDenoiseMovingDepthTol): the producer's expected
// previous depth (MOT z) is the CAMERA-ONLY one -- this frame's point seen from last frame's camera.
// A surface that moved on its own (a walking / running avatar, a moving prim) was elsewhere, at
// another depth: toward / away from the camera by about as much as it moved sideways. The history
// taps' depth test then rejected a fast mover's history every frame (a constant restart). With this
// bit the tolerance grows by the surface's own sideways motion in metres (the motion vectors'
// reprojection against the camera-only one, through last frame's projection), at most 25 % of the
// depth; the normal test still applies. Static surfaces (no own motion): unchanged.
#define DN_F_MOVE_ZTOL (1u << 31)
bool dnMoveZTol() { return (dp.ctl.x & DN_F_MOVE_ZTOL) != 0u; }
// The extra depth tolerance (m) for a surface whose motion-vector reprojection hc lands dTex denoiser
// texels from its camera-only one, at expected depth pw (0: no own motion / no projection).
float dnMoveDepthTol(vec2 dTex, float pw)
{
    const vec2 pk = vec2(abs(ssr.views[CLEAN_VIEW].prev_proj[0][0]), abs(ssr.views[CLEAN_VIEW].prev_proj[1][1]));
    if (!(pk.x > 0.05 && pk.y > 0.05) || !(pw > 0.0)) return 0.0;
    const float lat = length(dTex / dp.dims.xy * 2.0 / pk) * pw;
    return (lat >= 0.0) ? min(lat, 0.25 * pw) : 0.0;
}
bool dnInfoOn() { return (dp.ctl.x & DN_F_INFO) != 0u; }
// Informative diffuse / specular at texel q of THIS frame (W: its GEOM w's reference, raw: IN).
bool dnInfoD(float W, vec4 raw) { return W > 0.0 && raw.a > 0.0; }
bool dnInfoS(float W, vec4 raw) { return W > 0.0 && raw.a >= 0.0; }
// Tap weight of an UNKNOWN texel (DN_F_INFO, history length 0: never sampled, no estimate) --
// soft, never a hard zero (a band of them must not starve the filters).
#define DN_UNKNOWN_W 0.05
float dnConf(float len) { return (!dnInfoOn() || len > 0.0) ? 1.0 : DN_UNKNOWN_W; }
// The sun / moon disc rays per texel this frame (sun1.w; 0 = the frame info's RenderVKRTSunSamples):
// the one-ray option (RenderVKShadowSunOneRay) traces 1.
float dnSunRays() { return dp.sun1.w > 0.5 ? dp.sun1.w : max(ssr.sun_rt.y, 1.0); }
uint dnParity()  { return (dp.ctl.x & DN_F_PARITY) != 0u ? 1u : 0u; }
uint dnDebug()   { return (dp.ctl.x >> DN_DBG_SHIFT) & 15u; }
// a pass-through view (RAW, or the ReSTIR view's false colour): no history, no filter
bool dnRawView() { const uint d = dnDebug(); return d == PDN_DBG_RAW || d == PDN_DBG_RESTIR || (dp.ctl.x & DN_F_RAW) != 0u; }
// the sun / moon channel's pass-through (its debug RAW view, or #870 raw routing)
bool dnSunRaw()  { return dnDebug() == PDN_DBG_RAW || (dp.ctl.x & DN_F_RAW_SUN) != 0u; }
bool dnHistOk()  { return (dp.ctl.x & DN_F_HIST) != 0u && ssr.taa_params.z < 0.5 && dnColourHist(); }
#ifdef DN_AGE_NARROW
bool dnSunOn()   { return false; }   // (THE NARROW AGE: no sun age / countdown words; the GI has no sun channel)
#else
bool dnSunOn()   { return (dp.ctl.x & DN_F_SUN) != 0u; }
#endif
bool dnSunHistOk() { return dnHistOk() && (dp.ctl.x & DN_F_SUN_HIST) != 0u && dnSunHistImg(); }
float dnLum3(vec3 v) { return dot(v, vec3(1.0 / 3.0)); }   // (the sun channel: the RT mask's luminance)

// (#869) THE SPLIT CHANNEL (clean_pdn_common.glsl SPLIT CHANNEL): the images carry it (GEOM's
// layer count -- no push bit; the producer writes empty slots when it does not split), and its
// IN / OUT layer (the last one: after the sun channel's, which is allocated with it).
bool dnExactSpecular() { return dnRadiance() && (dp.ctl.y & 64u) != 0u && DN_LAYERS(uDnGeom) >= PDN_SPLIT_GEOM_LAYERS; }
#ifdef DN_AGE_NARROW
bool dnSplitOn()     { return false; }   // (THE NARROW AGE: the GI never has the split channel)
#else
bool dnSplitOn()     { return DN_LAYERS(uDnGeom) >= PDN_SPLIT_GEOM_LAYERS; }
#endif
uint dnSpecularOwner(ivec2 q, uint par)
{
    if (!dnExactSpecular()) return 0u;
#ifdef DN_TEMPORAL_PASS
    const int L = par == dnParity() ? PDN_SPLIT_GEOM_FINAL : PDN_SPLIT_GEOM_PAR + int(par);
#else
    const int L = PDN_SPLIT_GEOM_PAR + int(par);
#endif
    return imageLoad(uDnGeom, ivec3(q, DN_L(L))).x & 0xFFFFu;
}
int  dnSplitLayer()  { return DN_LAYERS(uDnIn) - 1; }
// The render pixel this denoiser texel samples (the RT mask's convention, clean_rt_mask_common
// .glsl rtMaskSamplePx: below the render extent the sample point follows the TAA jitter).
vec2 dnJitter() { return (ssr.taa_params.w > 0.5 && dp.dims.x < dp.dims.z && !dnGiStable()) ? ssr.taa_jitter.xy : vec2(0.0); }
ivec2 dnRenderPx(ivec2 m)
{
    const vec2 r = dp.dims.zw / dp.dims.xy;
    return clamp(ivec2((vec2(m) + 0.5 + dnJitter()) * r), ivec2(0), ivec2(dp.dims.zw) - 1);
}

// The guide of texel q in layer l: view depth (<= 0: none), normal, roughness, depth gradient.
// AGE (binding 6) layers: parity ages (diffuse, specular, sun, sun mover); SPLITAGE (binding 8,
// only with split images; the placeholder otherwise) layer parity: the split slots' ages.
// No coarsely quantized length feeds back; the sun history (SUNHIST) stays separate.
// The age word: x the diffuse (bits 0-15) and specular (16-31) ages in 1/32 ms (<= 2047 ms; the 63-frame
// history cap is 1050 ms at 60 Hz: a 1/32 ms step is 0.002 frames), y the radiance MOVER RECORD
// (clean_dn_temporal.comp; 0 = none), z / w the sun's age and mover countdown (float32 ms). THE NARROW AGE
// (DN_AGE_NARROW, the GI): x alone -- y / z / w read as 0 (none of their readers is compiled in).
int dnAgeLayer(uint par) { return int(par); }
int dnSplitAgeLayer(uint par) { return int(par); }   // (SPLITAGE's layer, not AGE's)
uint dnPackAges(float msD, float msS)
{
    return uint(clamp(msD, 0.0, 2047.0) * 32.0 + 0.5) | (uint(clamp(msS, 0.0, 2047.0) * 32.0 + 0.5) << 16u);
}
vec4 dnDecodeAges(uvec4 w)
{
#ifdef DN_AGE_NARROW
    return vec4(float(w.x & 0xFFFFu) / 32.0, float(w.x >> 16u) / 32.0, 0.0, 0.0);
#else
    return vec4(float(w.x & 0xFFFFu) / 32.0, float(w.x >> 16u) / 32.0, uintBitsToFloat(w.z), uintBitsToFloat(w.w));
#endif
}
// THE SPLIT AGES (SPLITAGE, rgba16ui; slot k = component k): each slot's history LENGTH (units of the 60 Hz reference
// interval, clean_history_time.glsl) in 1/2048 steps -- <= 31.9995; the split cap is maxL = clamp(prm0.x, 1, 31)
// (clean_dn_temporal.comp dnSplitTemporal), so nothing clamps. An integer length (every cap: maxL, the moving-light
// cap, 4) is exact, so the a-trous `len < 4.0` youth test and the caps compare as with float32; any other length is
// within 1/4096 of a reference interval (0.004 ms). A POSITIVE length never rounds to 0: `len > 0` is the
// known / UNKNOWN test (dnConf, the a-trous luminance stop), and a bilinear mean diluted by UNKNOWN taps can be tiny.
#define DN_SPLIT_AGE_SCALE 2048.0
uvec4 dnPackSplitAges(vec4 len)
{
    const vec4 c = clamp(len, vec4(0.0), vec4(65535.0 / DN_SPLIT_AGE_SCALE));
    return max(uvec4(c * DN_SPLIT_AGE_SCALE + 0.5), uvec4(greaterThan(c, vec4(0.0))));
}
vec4 dnSplitAges(ivec2 q, uint par) { return vec4(imageLoad(uDnSplitAge, ivec3(q, DN_L(dnSplitAgeLayer(par))))) / DN_SPLIT_AGE_SCALE; }
vec4 dnAges(ivec2 q, uint par) { return dnDecodeAges(imageLoad(uDnAge, ivec3(q, DN_L(dnAgeLayer(par))))); }
struct DnGeom { float z; vec3 n; float rough; vec2 grad; uint meta; float lenD; float lenS; uint rec; };
bool dnPlaneOrtho(bool previous)
{
    const mat4 M = previous ? ssr.views[CLEAN_VIEW].taa_prev_vp : ssr.views[CLEAN_VIEW].taa_cur_vp;
    return M[0][3] == 0.0 && M[1][3] == 0.0 && M[2][3] == 0.0;
}
float dnSurfaceDepthAt(DnGeom g, ivec2 center, ivec2 q)
{
    if (!dnShadowPlane()) return g.z + dot(g.grad, vec2(q - center));
    const float stepPx = max(floor(dp.dims.z / dp.dims.x + 0.5), 1.0);
    const vec2 offset = vec2(dnRenderPx(q) - dnRenderPx(center)) / stepPx;
    return pdnPlaneDepth(g.z, g.grad, offset, dnPlaneOrtho(false));
}
#ifdef DN_TEMPORAL_PASS
float dnPreviousDepthAt(DnGeom g, ivec2 q, vec2 hc)
{
    if (!dnShadowPlane()) return g.z;
    const vec2 ratio = dp.dims.zw / dp.dims.xy;
    const vec2 phase = dp.dims.x < dp.dims.z ? uintBitsToFloat(dp.ctl.zw) : vec2(0.0);
    const float stepPx = max(floor(ratio.x + 0.5), 1.0);
    const vec2 offset = (pdnPlaneRasterBetween(hc, ratio, phase) - pdnPlaneRaster(vec2(q), ratio, phase)) / stepPx;
    return pdnPlaneDepth(g.z, g.grad, offset, dnPlaneOrtho(true));
}
#endif
DnGeom dnGeom(ivec2 q, uint l)
{
    const uvec4 g = imageLoad(uDnGeom, ivec3(q, DN_L(int(l))));
    DnGeom r;
    r.z = uintBitsToFloat(g.x);
    r.n = pdnUnpackN(g.y);
    r.rough = pdnUnpackR(g.y);
    r.grad = unpackHalf2x16(g.z);
    r.meta = g.w;
#ifdef DN_TEMPORAL_PASS
    // Current guides are read by neighbouring temporal invocations while those
    // invocations write their own ages. Temporal validation needs only OLD ages
    // (and the old mover record, written with them).
    const uvec4 aw = l != dnParity() ? imageLoad(uDnAge, ivec3(q, DN_L(dnAgeLayer(l)))) : uvec4(0u);
#else
    const uvec4 aw = imageLoad(uDnAge, ivec3(q, DN_L(dnAgeLayer(l))));
#endif
    const vec2 age = dnDecodeAges(aw).xy / HT_REF_MS;
    r.lenD = age.x; r.lenS = age.y;
#ifdef DN_AGE_NARROW
    r.rec = 0u;   // (THE NARROW AGE: no record word)
#else
    r.rec = aw.y;
#endif
    if (!(r.z > 0.0) || isinf(r.z)) r.z = -1.0;
    return r;
}
