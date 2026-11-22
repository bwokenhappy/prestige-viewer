// vkclean -- RAY-TRACED GLOBAL ILLUMINATION (#87, ReSTIR GI; RenderVKGI): the shared state of the
// GI trace (clean_deferred_light.comp pass 11 -DCLEAN_GI_TRACE), the GI denoiser (a second
// LLCleanDenoiserVK, clean_dn_*.comp unchanged) and the consumers (the split lighting's combine /
// BASE: deferReflIn replaces the probes' DIFFUSE irradiance with the denoised GI where it is valid).
//
// THE SIGNAL: per GI texel (the render extent x RenderVKGIScale, the denoiser's jittered sample
// convention) the receiver's IRRADIANCE / pi -- the cosine-weighted mean incoming radiance, the
// same quantity as the reflection probes' irradiance maps (clean_probe_filter.comp mode 1: E / pi),
// so the lit pass multiplies it by its own diffuse colour exactly as the probe term. Demodulated by
// construction (no albedo of the receiver in it).
//
// ONE BOUNCE (GI-1): a cosine-weighted ray from the G-buffer receiver; its closest hit is shaded by
// the RT reflections' hit shading (clean_rt_shade.glsl rtShadeHit: the sun / moon with one shadow
// ray, the local lights through the light grid's picks or the cluster, emissive / fullbright
// surfaces, the hit's ambient per GI_HIT_AMB_*), a miss takes the sky along the ray.
// RESERVOIRS (GI-2 temporal, GI-3 spatial; Ouyang et al. 2021 ReSTIR GI): the sample is the
// path's second vertex x_s with its normal n_s and outgoing radiance L; a reused sample is
// RECONNECTED from this receiver with the solid-angle Jacobian of the move
//     J = (|cos phi_now| / |cos phi_then|) x (|x_s - x_then|^2 / |x_s - x_now|^2)
// (phi = the angle at x_s between n_s and the direction to the receiver; 1 for a sky sample) and
// its visibility from this receiver is traced once (no light through walls from a neighbour's
// sample). Target p(y) = lum(L) x max(N.w, 0); the 1/M combine; M capped (prm2.x frames).
// RADIANCE CACHE (GI-4): a world-space hash grid of irradiance (SHaRC-style, our own): cells of
// prm3.x m x 2^level (the level grows with the distance to the camera), split by the dominant axis
// of the normal (the two faces of a thin wall never share a cell), filled by every GI receiver's
// estimate (atomic fixed-point accumulation), resolved once per frame into an exponential mean
// (prm3.z frames), evicted after prm3.w frames untouched, and INVALIDATED where the host's change
// spheres say geometry moved / appeared / vanished -- never by camera motion (the cells are world
// boxes). A GI hit takes its ambient from the cache (multi-bounce: last frame's irradiance there)
// before the probes. Bounded: a fixed table (cfg.z slots), live / cap / peak in the stats.
#ifndef CLEAN_GI_COMMON_GLSL
#define CLEAN_GI_COMMON_GLSL
#ifndef GIR_PREC
#define GIR_PREC   // (#1039: precise in the GI REUSE stage -- clean_deferred_light.comp)
#endif

#define GI_F_ON         1u     // GI runs this frame (the frame info's foliage_rt.x bit 128 too)
#define GI_F_FINAL_IN   2u     // the GI denoiser's final image is IN (an odd a-trous count), else OUT
#define GI_F_TEMPORAL   4u     // GI-2: reuse last frame's reservoir
#define GI_F_SPATIAL    8u     // GI-3: + its neighbours'
#define GI_F_CACHE      16u    // GI-4: the radiance cache is built / read
#define GI_F_RES        32u    // the reservoir buffer is bound at this extent (write this parity)
#define GI_F_RES_PREV   64u    // ... and last frame's parity holds this extent's reservoirs
#define GI_F_CACHE_FILL 128u   // the receivers write their estimate into the cache
#define GI_SPECOCC_SHIFT 8u    // bits 8-9: the specular occlusion (RenderVKGISpecOcclusion: 0 off,
                               // 1 the sky visibility, 2 max(it, GI / probe irradiance))
#define GI_EVERY_SHIFT 10u     // bits 10-11: trace 1 texel in 2^n per frame (RenderVKGITraceEvery 1 / 2 / 4;
                               // the others reuse their reservoirs only, or hold the history)
#define GI_F_MISS_PROBES 4096u // a ray that leaves without a hit takes the probes' far field along it
                               // (RenderVKGIMissProbes), not the raw sky
#define GI_F_CULL_BACK   8192u // GI rays cull single-sided back faces in hardware (RenderVKGICullBackFaces)
#define GI_F_CEL_OCC     16384u // cel shading's flatten reference (the sky ambient: the rim light's and the
                                // flattened ambient's level) x GI's occlusion (RenderVKGICelOcclusion)
#define GI_F_SPECOCC_RT_RES 32768u // RT-marked pixels: the GI specular occlusion scales the probe env their
                                   // RT splice LEFT (misses, fades) instead of skipping them
                                   // (RenderVKGISpecOcclusionRTResidual)
#define GI_F_BACK_STOP  131072u // (RenderVKGIBackFaceStop) a GI ray whose first surface is a SINGLE-SIDED BACK
                                // FACE stops there and returns no light (the surface's inside). Before, the
                                // walk re-traced past back faces (and GI_F_CULL_BACK culled them in hardware),
                                // so a ray that started inside geometry -- the receiver's depth-rebuilt point a
                                // hair behind / inside its own or an overlapping prim at a contact edge, a
                                // moving avatar's raster vs. TLAS pose -- or slipped into a seam walked out
                                // through the far side and brought back the lit world / the sky: sparkles
                                // along edges. Hardware back-face culling is off for GI rays with it.
#define GI_F_POINT_SHADOW 262144u // (RenderVKGIHitPointShadows) GI hits' point lights join the hit shadow-ray
                                  // budget (RenderVKGIHitShadowRays: the K brightest shares get a ray); before,
                                  // point lights lit every GI hit unshadowed -- crevices, undersides, the far
                                  // side of a wall: bright samples at contact edges.
#define GI_F_REPROJ_SEARCH 524288u // (RenderVKGIReprojSearch) a temporal reservoir candidate whose nearest texel is
                                  // not this surface (sub-texel motion across a thin strand / a silhouette at the
                                  // GI extent) takes the best-matching texel of the 3 x 3 around the reprojected
                                  // point instead (the GI denoiser's history the same: DN_F_REPROJ_SEARCH)
#define GI_F_UPS_NORMAL 1048576u  // (RenderVKGIUpsampleNormal) the half / quarter-extent GI upsample weights its taps by
                                  // their normal too (prm4.w: the GEOM parity) and, with no tap on this surface,
                                  // takes the best-matching texel of the 4 x 4 (deferGiFetch)
#define GI_F_HAZE_OCC  4194304u // (RenderVKGIHazeOcclusion) the main view's haze IN-SCATTER (sky light) at a lit pixel x
                               // min(1, 2 x its GI sky visibility): a sealed room's walls take no sky-lit haze
                               // (the owner's teal glow in the gold cube); outdoor facades (half the sky) keep it
#define GI_F_STABLE_SAMPLES 2147483648u // (RenderVKGIStableSamples, an A/B, default off) below full resolution GI texel m samples
                               // the FIXED render pixel floor((m + 0.5) r), not the TAA-jittered floor((m + 0.5 + J) r)
                               // (clean_deferred_light.comp giRenderPixel; the GI denoiser: DN_F_GI_STABLE; the
                               // combine's same-frame upsample deferGiFetch the same phase). With the jittered lattice a receiver a
                               // few texels wide -- a distant avatar's limbs and torso -- was sampled at a different
                               // point every frame (another normal, another depth, at its edges another surface):
                               // the combine's normal / depth-weighted taps and the denoiser's history tests chose
                               // differently each frame, the owner's swimming noise on bodies past ~5 m at half /
                               // quarter GI extent (scripts/check/test_gi_stable_samples.cpp reproduces it at quarter
                               // extent: the stable lattice cuts a small still / swaying limb's swim by 55-80 %; at half
                               // extent the model is mixed, a swaying torso at quarter extent slightly worse). The raster
                               // itself still jitters under the sample (sub-pixel), as at full resolution.
#define GI_F_STATS     2097152u // the trace / resolve counters (stat .. stat4) are wanted: the host reads them
                               // back only while the GPU timers run (RenderVKGpuTimers) -- else no stat atomics
#define GI_F_LAG       8388608u // (#978 phase 2, RenderVKGILatencyFrame) ONE FRAME OF GI LATENCY: this frame's trace runs
                               // AFTER the combine (on the async queue beside the rest of the frame), so the GI images
                               // the combine / BASE read hold LAST frame's result -- in last frame's pixel grid, from
                               // last frame's camera. deferGiFetch then reprojects (ssr.views[CLEAN_VIEW].taa_prev_vp: the receiver's
                               // point into last frame's view; last frame's view depth = clip w, the images' alpha
                               // convention) and takes only taps on this surface; a disocclusion takes the radiance
                               // cache's cell, else no estimate (the probes). The header's GI_F_FINAL_IN and prm4.w
                               // (the GEOM parity) then describe LAST frame's trace (the host's mGiLag*).
#define GI_F_LAG_STALE 16777216u // (GI_F_LAG) last frame's GI images are unusable this frame (the first frame of the
                               // mode, a resize, the GI images / extent re-created, a frame without a trace, an
                               // offscreen / dropped recording in between): the cache, else the probes, everywhere
#define GI_F_REUSE_MV  33554432u // (#1038, RenderVKGIReuseMotion) the reservoirs' temporal candidate (and the
                               // disocclusion taps around it) follow the pre-pass MOTION VECTORS (set 3 binding 28,
                               // only while the frame info says they were written: taa_params.y), not the camera-only
                               // reprojection: on a walking / dancing avatar the camera-only point is where the body
                               // was NOT last frame (the background, or another part of it), so its texels lost every
                               // temporal candidate -- M 1, one fresh ray -- for as long as it moved. The expected
                               // depth stays the camera-only one, its tolerance grown by the surface's own motion.
                               // The reconnection (Jacobian + visibility ray) makes the moved receiver's reuse exact.
                               // prm5.w (RenderVKGIDisoccCache): a texel left with its FRESH SAMPLE ALONE (no
                               // temporal / spatial candidate survived: a disocclusion) blends the radiance cache's
                               // cell at its point in as a prior of up to that many samples -- its estimate is the
                               // cell's (many receivers over many frames) instead of a 1-spp speckle; the cache
                               // itself still takes the raw estimate. 0 = off.
#define GI_F_VIS_TSKIP 268435456u // (#985, RenderVKGIReuseVisSkip; A/B) a TEMPORAL reservoir pick (src 1) takes no visibility
                               // ray when the ray's segment crosses no change sphere this frame (giSegmentChanged): last
                               // frame that sample was visible from this surface point (its reservoir kept it after its
                               // own ray, or it was the texel's fresh hit), and nothing in the scene's change list lies
                               // across it. Leaks where an occluder moves without a sphere (avatars, a mover after its
                               // first frame). Spatial picks and re-shades keep their rays.
#define GI_FWD_FILTER_SHIFT 26u // bits 26-27 (RenderVKGIForwardCacheFilter): how a FORWARD surface reads the cache
                               // (giCacheForward): 0 its own cell, 1 the 4 cells around it in its face plane,
                               // 2 the 8 around it (trilinear; also across the cell layers a tilted face crosses)
#define GI_F_LIGHT_CHG  65536u // CHANGED LIGHTS (RenderVKGILightChangeCut): a reservoir sample stores the
                               // radiance its hit point had WHEN IT WAS SHADED; a reused sample whose
                               // point a light reaches that changed since last frame (moved, or its colour /
                               // intensity / radius / falloff changed by more than RenderVKReSTIRFlickerEps:
                               // a colour-cycling club light, a fire, a moving lamp -- the host's chg mask,
                               // measured against each light's GI REFERENCE (its record when GI last flagged
                               // it; a move counts past max(RenderVKReSTIRMoveEps, RenderVKGILightMoveRel x
                               // its radius) -- #980: a worn light's mm-a-frame sway flagged every frame)
                               // carries a stale colour: when it is the PICK and those lights are SIGNIFICANT
                               // at its point (their unshadowed light there, old record or new, at least
                               // RenderVKGIReshadeThreshold of its stored radiance: prm5.x, chg_prev; #980),
                               // its point is RE-SHADED with this frame's lights (one closest-hit ray that is
                               // also its visibility ray), so the reuse stays and the colour is current; the
                               // share of the candidates' weight such SIGNIFICANT lights hold caps the GI
                               // denoiser's history (the moving-light rule's MOT w). Without it: crawling
                               // pink / green blotches under club lights (each blotch one stale sample spread
                               // by the spatial reuse).

// hit ambient (prm.w): what a GI hit's own indirect light is
#define GI_HIT_AMB_NONE   0u   // nothing: one bounce, a sealed room goes dark
#define GI_HIT_AMB_PROBES 1u   // the reflection probes there (the RT reflections' rule)
#define GI_HIT_AMB_CACHE  2u   // the radiance cache, the probes where it has no cell
#define GI_HIT_AMB_CACHE0 3u   // the radiance cache, nothing where it has no cell

// debug views (cfg.w; RenderVKGIDebug)
#define GI_DBG_FINAL   1u      // the denoised GI irradiance, full screen
#define GI_DBG_RAW     2u      // this frame's raw estimate (no history, no filter)
#define GI_DBG_LEN     3u      // the GI denoiser's history length
#define GI_DBG_VAR     4u      // its variance
#define GI_DBG_RES     5u      // the reservoir: hue = where the sample came from (red fresh, green
                               // temporal, blue spatial), brightness = M / the cap
#define GI_DBG_CACHE   6u      // the cache at the receiver: green = a resolved cell (its value),
                               // red = no cell, yellow = a cell without samples yet
#define GI_DBG_SKYVIS  7u      // the denoised sky visibility (the specular occlusion's input)
#define GI_DBG_OUTLIER 9u      // SPARKLE CAUSES: texels whose GI estimate is > 4x the mean of their subgroup's
                               // (an 8 x 4 texel block), coloured by where the sample came from -- red = a
                               // fresh ray that left without a hit (sky / probe far field), orange = a fresh
                               // GRAZING ray (receiver cosine < 0.2: the hairline-gap escapes), yellow = a fresh
                               // hit, green = reused from last frame, blue = from a neighbour, cyan = a
                               // re-shaded reused sample, WHITE = a fresh hit with no reuse candidate at all (a
                               // disocclusion: RenderVKGIDisoccTaps / RenderVKGIReprojSearch target these);
                               // every other texel dim grey (its GI), and DIM PURPLE = a
                               // fresh ray stopped at a back face this frame (GI_F_BACK_STOP: a leak caught --
                               // dark, never a sparkle itself)
#define GI_DBG_SPATIAL 10u     // the SPATIAL taps' most frequent rejection (clean_deferred_light.comp GI_TAP_*), as
                               // bright as its share of the taps: magenta = no surface / off the extent, green =
                               // off the receiver's plane (depth), red = the other wall's normal, grey = an unusable
                               // reservoir, yellow = its sample behind the receiver, blue = the Jacobian, white = a
                               // non-finite weight; dark grey = every tap kept. Only where the receiver ran the
                               // reuse pick itself (not the queued reuse's VIS TAIL)
#define GI_DBG_LAMP    11u     // THE SPECKS AND WHERE THEIR LIGHT CAME FROM (clean_gi_lamp_why.h giLampSpeckColour): an
                               // outlier texel (view 9's test) at full intensity -- its fresh hit's lamp-ray reason
                               // (magenta the light's own fixture rule, orange no-self-shadow, blue an emitter's own
                               // geometry, cyan the facing rule, yellow an alpha-test hole / translucent caster, white
                               // nothing on the ray), red a fresh non-lamp sample, green a reused one; the other
                               // texels black (their lamp reason faint). Drawn per GI texel over the final image
                               // (GI_DBG_BUF), each mark held GI_DBG_HOLD frames
#define GI_DBG_OUTCOME 13u     // (gi-glass) THE FRESH RAY'S OUTCOME per GI texel: blue = left to the sky, grey = an opaque
                               // hit, green = an alpha-mask hit, brown = terrain / water, orange = an avatar, dark
                               // purple = a back-face stop; YELLOW = a fullbright / emissive hit (RED: its emission
                               // dropped by the #91 split); + MAGENTA mixed in by the alpha-blend layers crossed
                               // (RenderVKGITransmit on)
#define GI_DBG_SPECK   12u     // the outlier texels alone: white on black (same overlay and hold)
#define GI_DBG_HOLD    8u      // frames a mark of views 11 / 12 stays on the overlay (max-hold, never averaged)
#define GI_F_SP_LINEAR 536870912u // (RenderVKGISpatialPlaneTaps 0; an A/B) the spatial / disocclusion taps' expected
                               // depth extrapolated linearly (giPlaneZ) and no retry of a geometry reject
#define GI_DBG_CHG     8u      // GI_F_LIGHT_CHG: red = the significant changed lights' share of the candidates'
                               // weight (the GI denoiser's history cap; #1087: with the hot lights'), green = the picked reused sample was
                               // re-shaded (changed lights significant at its point), DIM green = a changed light
                               // reaches the pick but too weakly there (RenderVKGIReshadeThreshold: kept as it
                               // was), blue = the reservoir's W was scaled by the firefly clamp

// (#1087) RECENTLY MOVED LIGHTS -- the "hot" list (prm6.x entries, at most
// GI_HOT_MAX): every light GI_F_LIGHT_CHG flagged as MOVED within the last RenderVKGILightChangeHold frames
// (radiance-only changes stay #980's: the pick's re-shade fixes their colour, and a colour-cycling light
// would stay hot forever), 4 vec4 each: [4k] its OLD record's pos_radius (the reference before its latest
// move flag; w < 0: it MOVED), [4k + 1]
// that record's color_falloff, [4k + 2] its CURRENT sphere (xyz centre, w radius), [4k + 3] x its index in
// this frame's light list (-1: not in it this frame). The chg mask alone holds a moving light for single
// frames only (its reference rule flags it every few frames: 0.5 % of its radius), so between flags the
// reservoirs regrew their full confidence on samples shaded under the light's old position and the GI
// denoiser's history cap lifted: stale and fresh light averaged into speckle around every moving lamp
// (worst at half GI extent: one stale pick covers 2 x 2 pixels). While a light is hot:
//   * a reused reservoir candidate whose point it is significant at (giLightHot: the #980 rule, old
//     record or new) has its confidence M clamped to prm6.y (RenderVKGILightMoveMCap), so this frame's
//     fresh samples outweigh the old ones within a few frames (static areas: the full M cap);
//   * those candidates count toward the changed share (MOT w: the GI denoiser's history cap) every frame
//     of the hold, not only on flag frames;
//   * radiance-cache cells inside its old or new sphere keep a mean of at most prm6.z frames
//     (RenderVKGILightChangeCacheFrames) instead of prm3.z: they follow the light instead of
//     averaging 32 frames of its path (fast decay, never a restart: #980's objection to restarts).
#define GI_HOT_MAX 32u
#define GI_SPHERES_MAX 64u
// (#1438) A radiance-cache cell inside a CHANGE SPHERE (the resolve): a cell measured this frame, or within
// the last GI_SPHERE_CACHE_FRAMES (an on-screen cell its receivers reach every few frames), keeps its value
// and a mean of at most this many frames -- it follows the changed emitter / geometry within a few frames,
// continuously (#1087's rule for moved lights). Only a cell nobody measured lately RESTARTS (907cecad7e:
// no stale emission lingers off screen). Before, every cell in a sphere restarted: the next forward read
// was a 1-frame mean or no cell at all (the probes), and with a sphere queued on most frames the glass near
// an emitter that changes (UV / tint animation, a moving glow prim) flashed.
#define GI_SPHERE_CACHE_FRAMES 4.0
#define GI_CHG_PREV_MAX 64u   // (#980) chg_prev's records
// RELATIVE FIREFLY GUARD (giReuseFinish; on with RenderVKGIFireflyClamp > 0): a texel's estimate is capped at
// GI_REL_CLAMP x the larger of its block's winsorised mean (each lane first capped at GI_REL_WINSOR x the plain
// mean) and its radiance-cache cell -- the absolute clamp alone never bit in a dark room (the steady specks).
#define GI_REL_CLAMP  4.0
#define GI_REL_WINSOR 4.0
struct GiHeader
{
    uvec4 cfg;     // x GI_F_*, y frame, z cache slots (power of two; 0 = none), w debug view
    vec4  prm;     // x intensity, y minimum ambient (the probe irradiance's fraction kept as a floor),
                   // z ray length (m), w hit ambient (GI_HIT_AMB_*)
    vec4  prm2;    // x M cap (frames), y spatial taps, z spatial radius (texels), w firefly clamp
                   // (luminance of one sample's irradiance estimate; 0 = off)
    vec4  prm3;    // x cache cell edge at level 0 (m), y distance per level step (m), z the cache
                   // mean's longest window (frames), w the eviction age (frames)
    uvec4 stat;    // x rays, y hits, z misses (sky), w sum of the hit distances (dm)
    uvec4 stat2;   // x temporal taps kept, y spatial taps kept, z reused samples rejected by their
                   // visibility ray, w sum of the reservoirs' M (x 4)
    uvec4 stat3;   // x cache lookups at hits, y of them found, z cells inserted, w inserts that
                   // found the table full
    uvec4 stat4;   // x live cells (the resolve), y evicted, z invalidated by change spheres, w picked
                   // reused samples re-shaded because a changed light reaches them (GI_F_LIGHT_CHG)
    uvec4 sph_n;   // x change spheres this frame, y bit0 overflow, upper bits optional hard-prefix count+1, z (#980) picked reused samples a changed
                   // light reaches but too weakly to re-shade (RenderVKGIReshadeThreshold; a stat, GI_F_STATS),
                   // w (#985) temporal picks kept without a visibility ray (GI_F_VIS_TSKIP; a stat, GI_F_STATS)
    vec4  prm4;    // x the smallest receiver cosine a reservoir candidate may have (RenderVKGIReuseCosMin;
                   // 0 = the old 1e-4 floor), y the re-shade gain limit (RenderVKGIReshadeGain; 0 =
                   // none), z the disocclusion taps (RenderVKGIDisoccTaps; 0 = none), w the GI
                   // denoiser's GEOM parity the CONSUMERS read (GI_F_UPS_NORMAL): this frame's trace's,
                   // or under GI_F_LAG last frame's (the trace takes its own from the push, rtr2.w);
                   // under GI_F_LAG + 2 = RenderVKGILagRevealSearch (clean_deferred_light.comp
                   // deferGiFetchLag: readers take int(w + 0.5) & 1 as the parity)
    uvec4 chg[2];  // the lights that moved or whose radiance changed (bit = light index; the host's mGiChg:
                   // against each light's GI reference), GI_F_LIGHT_CHG only
    vec4  prm5;    // (#980) x the re-shade significance threshold (RenderVKGIReshadeThreshold; 0 = re-shade
                   // wherever a changed light reaches), y the old records in chg_prev, z (#1041) the own-fixture
                   // box limit of the hits' budgeted point-light rays (RenderVKReSTIRFixtureSize, m; 0 = off),
                   // w (#1038) the disocclusion cache prior's largest weight in samples (RenderVKGIDisoccCache;
                   // 0 = off)
    vec4  sph[GI_SPHERES_MAX];   // xyz centre (world), w radius
    vec4  chg_prev[128];   // (#980; 2 x GI_CHG_PREV_MAX) the changed lights' OLD records (their GI reference), in
                   // ascending light index (the k-th set bit of chg): [2k] pos_radius (w < 0: it MOVED --
                   // radius |w|, its old projector frustum unknown), [2k + 1] color_falloff. A changed light
                   // past prm5.y has none: significant wherever it reaches. After the set bits' records (only
                   // when every set bit has one), up to prm5.y: GONE lights -- switched off / dimmed to nothing
                   // / derezzed this frame, no index, their old record alone (w < 0: range only).
    vec4  prm6;    // (#1087) x the hot list's entries (0 = off), y the M clamp of a candidate a hot light is
                   // significant at (RenderVKGILightMoveMCap; 0 = none), z the radiance cache's mean window
                   // inside a hot light's spheres (RenderVKGILightChangeCacheFrames; 0 = none), w (#1406) a white
                   // fullbright's radiance in this frame's units (the sky HDR scale: giSoftCap's unit)
    vec4  hot[128];   // (#1087; 4 x GI_HOT_MAX) the hot list (see GI_HOT_MAX)
    uvec4 mov;     // (RenderVKGIMoverSamples; clean_gi_movers.glsl) the mover pixels' GI the CONSUMERS read: xy the
                   // buffer's device address (0: none -- the reduced-extent GI everywhere), z its render extent
                   // (w | h << 16), w its history base (uvec2 units, bits 0-20) | the frame stamp the consumers read
                   // << 21 (this frame's, or under GI_F_LAG last frame's mover trace; its & 1 the parity)
    uvec4 xcfg;    // (gi-glass) x GIX_* flags, y the cache specular's roughness cutoff (float bits;
                   // RenderVKGICacheSpecRough), z the cache specular's gain (float bits; RenderVKGICacheSpecGain),
                   // w the reflection-source debug view (RenderVKGICacheSpecDebug: GIX_DBG_*)
    uvec4 stat5;   // (gi-glass, GI_F_STATS) GI RAY OUTCOMES of the fresh / re-shade rays: x left without a hit
                   // (sky), y stopped at an opaque-class instance, z at an alpha-mask-class instance, w at terrain /
                   // water
    uvec4 stat6;   // x at an avatar / worn attachment, y the hit is fullbright or emissive, z its emission dropped
                   // (the #91 GI split: ReSTIR DI lights it), w a single-sided back-face stop
    uvec4 stat7;   // x rays that crossed at least one alpha-blend layer (GIX_TRANSMIT), y the layers crossed
                   // (summed), z rays with no blend layer, w unused
};                 // 5424 B (LLCleanRendererVK::GiHeader)

// (gi-glass) xcfg.x flags
#define GIX_TRANSMIT   1u      // GI rays composite the alpha-blend layers in front of their hit / sky (RenderVKGITransmit)
#define GIX_TINT       2u      // ... the transmitted light tinted by the layer colour (RenderVKGITransmit 2; else grey 1 - a)
#define GIX_CACHE_DIR  4u      // the radiance cache's DIRECTION words exist and are filled (giCacheDirBase)
#define GIX_CACHE_SPEC 8u      // rough receivers take their specular from the cache (RenderVKGICacheSpec)
// xcfg.w: the reflection-source debug view (the combine; RenderVKGICacheSpecDebug)
#define GIX_DBG_SOURCE 1u      // red = RT reflection, green = the cache specular, yellow = the cross-fade, blue = the
                               // probe / SSR fallback (no cell), black = no specular source (unlit / fullbright)
#define GIX_DBG_SPEC   2u      // the cache specular alone (what a rough receiver reflects from it), black where none
// The fresh ray's OUTCOME word (RenderVKGIDebug 13; the split records' q3.w, the fused kernel's g_gi_outcome):
// bits 0-2 the class, bit 3 a fullbright / emissive hit, bit 4 its emission dropped (the #91 split), bits 5-7 the
// alpha-blend layers crossed (GIX_TRANSMIT)
#define GIO_SKY    0u
#define GIO_OPAQUE 1u
#define GIO_AMASK  2u
#define GIO_GROUND 3u          // terrain / water
#define GIO_AVATAR 4u
#define GIO_BACK   5u
#define GIO_FB     8u
#define GIO_EMDROP 16u
#define GIO_LAYER_SHIFT 5u

#ifndef GI_STATE_ACCESS
#define GI_STATE_ACCESS
#endif
// The state buffer: the header, then the cache -- RESOLVED cells (2 uvec4 each: [0] key (0 empty,
// GI_KEY_TOMB freed), valid marker, centre x, y bits; [1] centre z bits, half(r, g), half(b, 0),
// cell edge bits) at gi_data[2 slot ..], then the ACCUMULATORS (1 uvec4 each: r, g, b fixed point
// x GI_ACC_SCALE, count) at gi_data[2 cap + slot]. Final region at [3 cap + slot]:
// float32 millisecond EMA age, idle age, the cell's per-sample luminance variance (float32, the change
// test's spread: giCellVar), its change word (bits 0-3 the episode's direction / the pending departures,
// bits 8-15 the frames it stays CHANGED: giCellHold) -- 64 B/slot in all. After the slots, the
// change bins (GI_BIN_*: the regional change test, the unmeasured cells' ratio).
// Resolved c0.y is only a valid marker; sample counts remain in the accumulator.
#define GI_KEY_TOMB 0xFFFFFFFFu
// (gi-glass) THE CACHE'S DIRECTION WORDS (GIX_CACHE_DIR; host: llcleangivk.cpp giCacheBytes): one uvec4 per slot
// after the change bins (3 x GI_BIN_BANK = 3 x 4096 uvec4) -- xyz the frame's luminance-weighted ray-direction
// sums of the fills (int32 in the accumulator's fixed point, two's complement through uint atomics), w the
// resolved MEAN DIRECTION v (snorm 10:10:10, bit 30 valid): the cell's fresh rays' luminance-weighted mean
// direction, |v| <= 1 (cosine rays under uniform light: 2/3 N). 16 B / slot (4 MB at 2^18 slots).
#define GI_DIR_BASE(cap) (4u * (cap) + 3u * 4096u)
// The direction sums' extra fixed-point factor over the luminance's (x GI_ACC_SCALE): with STOCHASTIC rounding per
// sample (unbiased at any luminance -- a dark room's samples are a few units). int32 headroom: a sample is at most
// GI_ACC_MAXLUM x GI_ACC_SCALE x 4 = 2^17, so 16k samples per cell per frame at the luminance cap.
#define GI_DIR_Q 4.0
uint giPackDir(vec3 v)
{
    const ivec3 q = ivec3(round(clamp(v, vec3(-1.0), vec3(1.0)) * 511.0));
    return uint(q.x & 1023) | (uint(q.y & 1023) << 10) | (uint(q.z & 1023) << 20) | (1u << 30);
}
vec3 giUnpackDir(uint w)
{
    return vec3(float(int(w << 22) >> 22), float(int(w << 12) >> 22), float(int(w << 2) >> 22)) * (1.0 / 511.0);
}
// (the deferred kernels: set 3 binding 23; the forward lit FS: draw set 0 binding 53, CLEAN_GI_FORWARD)
#ifndef GI_STATE_SET
#define GI_STATE_SET 3
#define GI_STATE_BINDING 23
#endif
layout(std430, set = GI_STATE_SET, binding = GI_STATE_BINDING) GI_STATE_ACCESS buffer GiState
{
    GiHeader gi;
    uvec4    gi_data[];
};
#define GI_ACC_SCALE 64.0
#define GI_ACC_MAXLUM 512.0

// GI ran this frame (the frame info's foliage_rt.x bit 128): only then may the GI state be read.
#define giFrameOn() ((uint(ssr.foliage_rt.x + 0.5) & 128u) != 0u)   // (a macro: some includers see ssr later)
uint giPcg(uint v)
{
    const uint st = v * 747796405u + 2891336453u;
    const uint w = ((st >> ((st >> 28u) + 4u)) ^ st) * 277803737u;
    return (w >> 22u) ^ w;
}
float giLum(vec3 c) { GIR_PREC const float l = dot(c, vec3(0.2126, 0.7152, 0.0722)); return l; }
// normalize(v) -- in the REUSE stage as an explicit contraction-free sequence (GIR_PREC)
#ifdef GIR_PREC_ON
vec3 giNormalize(vec3 v) { precise const float d = dot(v, v); return v * inversesqrt(d); }
#else
vec3 giNormalize(vec3 v) { return normalize(v); }
#endif

// ---- the cache's cell of (P, N): its key (never 0), level and edge ----
// The distance level of P (edge: its cell edge).
int giCellLevel(vec3 P, vec3 cam, out float edge)
{
    const vec3 pv = P - cam;
    GIR_PREC const float d2 = dot(pv, pv);
    const float d = sqrt(d2);
    const float step = max(gi.prm3.y, 1.0);
    const int level = clamp(int(floor(log2(max(d / step, 1.0)))), 0, 7);
    edge = max(gi.prm3.x, 0.05) * float(1u << uint(level));
    return level;
}
// N's dominant axis (0 x, 1 y, 2 z) and its face bucket (axis x 2, + 1 when it points down that axis).
uint giCellAxis(vec3 N)
{
    const vec3 a = abs(N);
    return (a.x >= a.y && a.x >= a.z) ? 0u : (a.y >= a.z ? 1u : 2u);
}
uint giCellBucket(vec3 N, uint axis) { return axis * 2u + ((axis == 0u ? N.x : (axis == 1u ? N.y : N.z)) < 0.0 ? 1u : 0u); }
// The key of cell q at a level and face bucket.
uint giCellKeyQ(ivec3 q, int level, uint bucket)
{
    uint h = giPcg(uint(q.x) ^ giPcg(uint(q.y) ^ giPcg(uint(q.z) ^ giPcg(uint(level) * 8u + bucket))));
    return (h == 0u || h == GI_KEY_TOMB) ? 1u : h;
}
uint giCellKey(vec3 P, vec3 N, vec3 cam, out float edge)
{
    const int level = giCellLevel(P, cam, edge);
    return giCellKeyQ(ivec3(floor(P / edge)), level, giCellBucket(N, giCellAxis(N)));
}
// The cell centre of P at edge (the resolve's change-sphere test).
vec3 giCellCentre(vec3 P, float edge) { return (floor(P / edge) + 0.5) * edge; }
uint giCacheSlots() { return gi.cfg.z; }
// The slot holding key (read only); 0xFFFFFFFF: none.
uint giCacheFind(uint key)
{
    const uint cap = giCacheSlots();
    if (cap == 0u) return 0xFFFFFFFFu;
    uint s = giPcg(key) & (cap - 1u);
    for (uint i = 0u; i < 8u; ++i)
    {
        const uint k = gi_data[2u * s].x;
        if (k == key) return s;
        if (k == 0u) return 0xFFFFFFFFu;   // (a freed slot, GI_KEY_TOMB, keeps the chain going)
        s = (s + 1u) & (cap - 1u);
    }
    return 0xFFFFFFFFu;
}
// This recording's change regions, shared by cache readers, reuse and history. Emitter spheres include
// two legs of what the emitter's change can light (the host, #1406); geometry spheres keep their own
// radius. Two kinds (the sign of w):
//   HARD (w >= 0: geometry changed there, a store / origin reset; the list's overflow): old samples there
//   may be WRONG (a surface gone, an occluder moved) -- reservoirs dropped, the cache restarted where
//   nobody measured it lately, its readers before the resolve reject it.
//   SOFT (w < 0, radius -w: an emitter's light changed -- colour, map, a move, appear / release): old
//   samples are only STALE -- the history adapts within a few frames instead of restarting (the pulse
//   that restarting a 160 m sphere made every time a club's big panel changed): every reused reservoir
//   there counts as at most GI_SPHERE_MCAP samples, the cache cells take the short mean
//   (GI_SPHERE_CACHE_FRAMES) and keep their value, fresh rays trace every frame.
// 0: in none, 1: in a soft one only, 2: in a hard one. Missing bounds / overflow: hard (conservative).
#define GI_SPHERE_MCAP 2.0
// (strength: giSoftStrength's value at P, from the same loop -- 0 outside the soft spheres; a hard hit
//  returns before it is complete)
uint giSphereHitS(vec3 P, float padding, out float strength)
{
    strength = 0.0;
    if ((gi.sph_n.y & 1u) != 0u) return 2u;
    uint hit = 0u;
    for (uint i = 0u; i < min(gi.sph_n.x, GI_SPHERES_MAX); ++i)
    {
        const vec4 sp = gi.sph[i];
        const vec3 delta = P - sp.xyz;
        GIR_PREC const float d2 = dot(delta, delta);
        const float radius = abs(sp.w) + padding;
        if (d2 <= radius * radius)
        {
            if (sp.w >= 0.0) return 2u;
            hit = 1u;
            strength = max(strength, 0.25 * sp.w * sp.w / max(d2, 0.0625));
        }
    }
    return hit;
}
uint giSphereHit(vec3 P, float padding)
{
    float s;
    return giSphereHitS(P, padding, s);
}
// P lies in a HARD change sphere (its history is wrong, not only stale)
bool giSceneChanged(vec3 P, float padding)
{
    // Optional host-published hard prefix. The all-sphere readers still see every
    // soft sphere; only a hard-only cache query avoids the soft list and distance ALU.
    const uint prefix = gi.sph_n.y >> 1u;
    if (prefix == 0u) return giSphereHit(P, padding) == 2u;
    if ((gi.sph_n.y & 1u) != 0u) return true;
    const uint n = min(prefix - 1u, min(gi.sph_n.x, GI_SPHERES_MAX));
    for (uint i = 0u; i < n; ++i)
    {
        const vec4 sp = gi.sph[i];
        const vec3 delta = P - sp.xyz;
        GIR_PREC const float d2 = dot(delta, delta);
        const float radius = abs(sp.w) + padding;
        if (d2 <= radius * radius) return true;
    }
    return false;
}
// (#985) the segment a -> b passes within some change sphere this recording (or the list overflowed): the
// closest point of the segment to each centre against its radius.
bool giSegmentChanged(vec3 a, vec3 b)
{
    if ((gi.sph_n.y & 1u) != 0u) return true;
    const vec3 ab = b - a;
    const float l2 = max(dot(ab, ab), 1e-12);
    for (uint i = 0u; i < min(gi.sph_n.x, GI_SPHERES_MAX); ++i)
    {
        const vec4 sp = gi.sph[i];
        const vec3 delta = a + ab * clamp(dot(sp.xyz - a, ab) / l2, 0.0, 1.0) - sp.xyz;
        if (dot(delta, delta) <= sp.w * sp.w) return true;   // soft (w < 0) counts too: conservative
    }
    return false;
}
// (#985) the reused pick of source src needs no visibility ray o -> o + dv tm: GI_F_VIS_TSKIP, a temporal pick,
// the segment clear of every change sphere. Depends only on its arguments and the header (the re-shade TAIL
// re-derives nothing from it).
bool giVisSkip(uint src, vec3 o, vec3 dv, float tm)
{
    return (gi.cfg.x & GI_F_VIS_TSKIP) != 0u && src == 1u && !giSegmentChanged(o, o + dv * tm);
}
// (#1406) HOW MUCH a soft sphere's change matters at P, in units of the host's threshold (1/256 of a
// white fullbright's radiance): the host sized the sphere as the emitter's bounds + two legs of the reach
// where the change falls to the threshold, and the change falls off as the inverse square -- about
// (R / 2 d)^2 at d from the centre (R the radius): 1 at half the radius, 1/4 at its edge, large at the
// emitter. The largest over the soft spheres P lies in (merged spheres overstate it: conservative);
// giSphereHitS computes it in the sphere loop.
// The flat cap (a638ea06f6) applied GI_SPHERE_MCAP anywhere inside: a 150 m sphere around a club's big
// panel cut every room's reservoirs to 2 samples on a quarter of the frames -- dancing noise in corners
// far from the panel. GI_F_SPH_FLAT (RenderVKGISoftSphereFlat, an A/B) brings that rule back.
#define GI_F_SPH_FLAT 1073741824u
#define GI_SPHERE_THR (1.0 / 256.0)
// The confidence (samples) a stale value of brightness lum may keep inside a soft sphere where the change's
// strength is strength (giSphereHitS; mcap: the
// normal cap). Its error is the change there (strength x the threshold, in this frame's radiance units --
// a white fullbright is prm6.w, the sky HDR scale) against its own brightness: a change that is a large share of what
// the sample carries keeps GI_SPHERE_MCAP (it adapts within a few frames), a small share the full history
// (GI_SPHERE_STALE_K: the share at which the cap is mcap / K -- 10 % of its light at K = 40 is 4 of 16).
#define GI_SPHERE_STALE_K 40.0
float giSoftCap(float strength, float lum, float mcap)
{
    if ((gi.cfg.x & GI_F_SPH_FLAT) != 0u) return min(mcap, GI_SPHERE_MCAP);
    const float unit = GI_SPHERE_THR * max(gi.prm6.w, 1e-6);
    const float rel = strength * unit / max(lum, unit);
    return clamp(mcap / max(rel * GI_SPHERE_STALE_K, 1.0), min(mcap, GI_SPHERE_MCAP), mcap);
}
// A receiver at P traces a fresh ray this frame for a scene change there (on top of RenderVKGITraceEvery's
// own texels): a hard sphere, or where a soft one's change is at least the threshold (flat: anywhere inside)
bool giSphereTrace(vec3 P)
{
    float s;
    const uint h = giSphereHitS(P, 0.0, s);
    if (h != 1u) return h == 2u;
    return (gi.cfg.x & GI_F_SPH_FLAT) != 0u || s >= 1.0;
}
#ifndef GI_STATE_READONLY
// Find or insert the cell of key (centre / edge written by its inserter); 0xFFFFFFFF: the probe
// window is full (counted). inserted: this call took an empty / freed slot.
uint giCacheFindOrInsert(uint key, vec3 centre, float edge, out bool inserted)
{
    inserted = false;
    const uint cap = giCacheSlots();
    if (cap == 0u) return 0xFFFFFFFFu;
    uint s = giPcg(key) & (cap - 1u);
    for (uint i = 0u; i < 8u; ++i)
    {
        uint k = gi_data[2u * s].x;
        if (k == key) return s;
        if (k == 0u || k == GI_KEY_TOMB)
        {
            const uint prev = atomicCompSwap(gi_data[2u * s].x, k, key);
            if (prev == k)
            {
                inserted = true;
                gi_data[2u * s].y = 0u;
                gi_data[3u * cap + s] = uvec4(0u); // float32 EMA age and idle age, milliseconds
                gi_data[GI_DIR_BASE(cap) + s] = uvec4(0u);   // (gi-glass: never an old tenant's direction)
                gi_data[2u * s].z = floatBitsToUint(centre.x);
                gi_data[2u * s].w = floatBitsToUint(centre.y);
                gi_data[2u * s + 1u] = uvec4(floatBitsToUint(centre.z), 0u, 0u, floatBitsToUint(edge));
                return s;
            }
            if (prev == key) return s;
        }
        s = (s + 1u) & (cap - 1u);
    }
    return 0xFFFFFFFFu;
}
// A receiver's estimate (irradiance / pi) in the accumulator's fixed point.
uvec3 giCacheQ(vec3 E)
{
    const float l = giLum(E);
    if (l > GI_ACC_MAXLUM) E *= GI_ACC_MAXLUM / l;
    return uvec3(clamp(E, vec3(0.0), vec3(GI_ACC_MAXLUM)) * GI_ACC_SCALE + 0.5);
}
// A receiver's estimate (irradiance / pi) into its cell's accumulator.
void giCacheAdd(uint s, vec3 E)
{
    const uvec3 q = giCacheQ(E);
    const uint a = 2u * giCacheSlots() + s;
    atomicAdd(gi_data[a].x, q.x);
    atomicAdd(gi_data[a].y, q.y);
    atomicAdd(gi_data[a].z, q.z);
    atomicAdd(gi_data[a].w, 1u);
}
#endif
// ---- THE CHANGE TEST (the sealed-room fade) ----
// A cell's mean is an exponential window of up to prm3.z frames, and GI hits read it as the next bounce:
// in a closed room the light cycles cell -> hit -> receiver -> cell, so every bounce waited a full window
// (and, while the cache took the reservoirs' estimate, their ~1 / Msum per frame); the cells behind the
// camera, which no receiver measures, held their light until eviction (prm3.w frames) -- a room sealed
// from its light stayed lit for 15-20 s. The fix shortens history ONLY where the change is real:
//   * THE TEST is regional (GI_BIN_*): one cell's fresh mean is a few dozen 1-spp rays, ~17 % noise a
//     frame against a 20 % step -- no test on it can be both quick and quiet. Each measured cell adds its
//     relative departure r (this frame's FRESH mean m -- the receivers' own rays fill the cache: independent,
//     immediate -- against its stored mean mu) and that departure's relative sampling variance v (from the
//     cell's per-sample spread vs, giCellVar) to its region's bin (GI_BIN_EDGE m boxes); r is clipped at
//     GI_BIN_CLIP of its own sigma, so no outlier cell moves a bin alone. Next frame a bin whose summed
//     departure is GI_BIN_REL of the light and GI_BIN_K sigma of the summed noise (or GI_BIN_KS sigma the
//     same way GI_BIN_PERSIST frames running) FIRES: every cell in it is CHANGED for GI_CELL_HOLD frames,
//     re-armed while the region keeps moving the same way at GI_BIN_KS sigma (a two-threshold trigger:
//     noise can extend a real episode only rarely and never start one; giCellHold).
//     The caster test's idea (fix-stoch-shadow-noise: evidence against its sampling noise), at the scale
//     where the evidence exists -- the A-SVGF temporal-gradient idea without its extra rays (the fresh
//     estimates are the gradient samples), and DDGI-style adaptive hysteresis driven by a statistic.
//   * A CHANGED cell's mean is this frame's fresh mean (window GI_CELL_HWIN): the loop runs at one bounce
//     per frame, as fast as a frame-iterated cache can (the physical decay is instant); its receivers cap
//     their reused reservoir confidence at GI_CELL_MCAP (fresh rays weigh 1 / GI_CELL_MCAP a reuse) and
//     their GI denoiser history at DN_GI_CHANGED_HIST frames; under DLSS Ray Reconstruction (the GI goes
//     to RR raw, no GI denoiser) the responsivity guide is set there instead (GI_MOT_CHANGED), so RR's
//     own history -- the lag left after the cache's fix, broken only by camera motion -- drops too.
//   * An UNMEASURED cell (behind the camera, occluded) takes its bin's new / old ratio of the measured
//     cells' sums every frame it is CHANGED: the sealed room's unseen walls fade with its seen ones -- left
//     alone they held the room up at their old light until eviction. Settled light: never (no random walk).
// Settled light never fires; nothing outside a fired region changes. Fixture: scripts/check gi-sealed-fade.
#define GI_CELL_VCLIP  16.0   // one frame's squared residual adds at most this x the spread to it
#define GI_CELL_HOLD   64u    // frames a CHANGED cell stays CHANGED (frames: the loop runs a bounce a frame;
                              // 0.93^64 ~ 1 %: an albedo-0.9 room's episode converges within one)
#define GI_CELL_HWIN   1.0    // a CHANGED cell's window (frames): this frame's fresh mean
#define GI_CELL_MCAP   0.25   // a CHANGED cell's receivers: reused reservoir confidence
#define GI_MOT_CHANGED 256u   // the GI MOT image's w bit 8: the receiver's cell is CHANGED (clean_dn_temporal.comp,
                              // clean_rr_guides.comp read it; bits 0-7 the changed lights' share)
// The per-sample variance after this frame: the residual against the mean BEFORE the update (a
// prediction residual: conservative), scaled to one sample (x cnt: a cell's coverage changes with the
// camera), clipped, at the mean's own rate a. A CHANGED cell leaves it as it was (the steps are no noise).
float giCellVar(float vs, float m, float mu, float cnt, float a, bool changed)
{
    if (changed) return vs;
    const float r = cnt * (m - mu) * (m - mu);
    if (!(vs > 0.0)) return r;
    return mix(vs, min(r, GI_CELL_VCLIP * vs), a);
}
// THE CHANGE BINS: GI_BIN_N keyed records per bank, three banks (written this frame, read -- last frame's
// --, cleared for the next, by the header frame cfg.y; a record's tag is the frame that wrote it + 1).
// A record: [0] key, tag, measured cells, their old means' sum; [1] their relative departure sum (signed,
// GI_BIN_RQ), relative sampling variance sum (GI_BIN_VQ), new means' sum. The means in units of the
// change unit (giSoftCap's: 1/256 of a white fullbright) x GI_BIN_LQ, each at most GI_BIN_LMAX units (16 x a
// white fullbright: 2^18 quanta, so a sum overflows only past 16384 cells a bin -- a clutter-free 4 m box holds
// ~1536 level-0 cells; brighter cells saturate both sums: ratio ~1); the ratio of the sums, not the mean of ratios (that one is biased up by
// the noise: chained every frame, it ran away).
// Fixed: 3 x 2 GI_BIN_N uvec4 after the slots (host: kGiBinBytes, llcleangivk.cpp).
#define GI_BIN_N     2048u
#define GI_BIN_EDGE  4.0
#define GI_BIN_MIN   8u
#define GI_BIN_K     6.0
#define GI_BIN_KS    3.0
#define GI_BIN_REL   0.05
#define GI_BIN_CLIP  3.0
#define GI_BIN_RQ    4096.0
// (GI_BIN_VMAX x GI_BIN_VQ = 245760 quanta a cell: the variance sum overflows only past 16384 cells a bin, as the
//  means' sums)
#define GI_BIN_VQ    16384.0
#define GI_BIN_VMAX  15.0
#define GI_BIN_RMAX  4.0
#define GI_BIN_LQ    64.0
#define GI_BIN_LMAX  4096.0
#define GI_BIN_BANK  (2u * GI_BIN_N)
uint giBinBank(uint frame) { return frame % 3u; }
uint giBinKey(vec3 c)
{
    const ivec3 q = ivec3(floor(c / GI_BIN_EDGE));
    const uint h = giPcg(uint(q.x) ^ giPcg(uint(q.y) ^ giPcg(uint(q.z) ^ 0x5BD1E995u)));
    return h == 0u ? 1u : h;
}
// A measured cell's terms for its bin: the relative departure r of its fresh mean m (clipped at GI_BIN_CLIP
// sigma) and the relative variance v of that departure -- the frame mean's (vs / cnt) and the stored
// mean's own (an exponential mean of window n holds 1 / (2 n - 1) of it), over mu^2. false: not testable
// (fewer than 2 samples, no spread yet, no light).
// v is NOT capped at 1 (it was, up to 2026-11-14): a cell whose few fresh samples a frame rarely include its bright
// path (a small emitter, a lit patch seen through a gap: fewer than ~1 such hit per cell and frame -- the regime of
// reduced GI extent and of upscaling, where a cell gets 9 x fewer rays at DLSS Ultra Performance than at native 4K)
// has a frame mean whose relative spread is many times 1. Capped at 1, every frame WITHOUT the rare hit read as a
// ~1 sigma drop in every such cell at once; summed over the bin's cells it passed GI_BIN_K: the region went CHANGED
// with no change at all (1-frame cache means, reservoir confidence GI_CELL_MCAP, GI denoiser history
// DN_GI_CHANGED_HIST -- near 1-spp GI, the owner's noise at reduced resolution, worst around emissives). Only the
// sums' fixed point bounds it now (GI_BIN_VMAX: relative spread ~3.9, beyond which a cell's departure carries next to
// no evidence anyway). scripts/check/checks/gi_sealed_fade.py (c) replays it.
bool giBinTerms(float m, float mu, float vs, float cnt, float n, float unit, out float r, out float v)
{
    r = 0.0; v = 0.0;
    if (n < 1.0 || cnt < 2.0 || !(vs > 0.0) || !(mu > unit)) return false;
    v = min(vs / (cnt * mu * mu) * (1.0 + 1.0 / (2.0 * n - 1.0)), GI_BIN_VMAX);
    const float lim = GI_BIN_CLIP * sqrt(v);
    r = clamp((m - mu) / mu, -lim, lim);
    return true;
}
// A bin's record (count, the summed departure sr and variance sv, scaled back): bits 0-1 the direction of
// a MATERIAL departure (a mean of GI_BIN_REL) of at least GI_BIN_KS sigma (1 up, 2 down; 0 none), bit 2 at
// least GI_BIN_K sigma.
uint giBinVerdict(uint count, float sr, float sv)
{
    if (count < GI_BIN_MIN) return 0u;
    const float a = abs(sr), sig = sqrt(max(sv, 1e-12));
    if (!(a > GI_BIN_KS * sig) || !(a > GI_BIN_REL * float(count))) return 0u;
    const uint dir = sr > 0.0 ? 1u : 2u;
    return a > GI_BIN_K * sig ? dir | 4u : dir;
}
// A cell's CHANGED state after its bin's verdict v. hold: frames left (already counted down this frame);
// st: bits 0-1 a direction, bits 2-3 a count -- held, the episode's direction; not held, the direction of
// the consecutive GI_BIN_KS departures seen so far. An episode STARTS on a GI_BIN_K departure, or on
// GI_BIN_PERSIST consecutive GI_BIN_KS ones the same way (noise: (1.3e-3)^3 a frame -- a 10 % step under
// 1-spp noise stands at ~5 sigma a frame, every frame); it is RE-ARMED while its region keeps moving the
// same way at GI_BIN_KS (a two-threshold trigger: a settled region ends it). Returns the new st.
#define GI_BIN_PERSIST 3u
uint giCellHold(uint v, inout uint hold, uint st)
{
    const uint vd = v & 3u, dir = st & 3u;
    if ((v & 4u) != 0u) { hold = GI_CELL_HOLD; return vd; }
    if (hold > 0u)
    {
        if (vd != 0u && vd == dir) hold = GI_CELL_HOLD;
        return dir;
    }
    if (vd == 0u) return 0u;
    const uint runs = vd == dir ? ((st >> 2u) & 3u) + 1u : 1u;
    if (runs >= GI_BIN_PERSIST) { hold = GI_CELL_HOLD; return vd; }
    return vd | (runs << 2u);
}
// A mean's quantum in a bin's sums (lum: its luminance; unit: the change unit).
uint giBinQ(float lum, float unit) { return uint(clamp(lum / unit, 0.0, GI_BIN_LMAX) * GI_BIN_LQ + 0.5); }
// The ratio an unmeasured cell takes each frame: its bin's measured cells' new / old (sums; 1: too few).
float giBinRatio(uint count, uint qOld, uint qNew)
{
    if (count < GI_BIN_MIN || qOld == 0u) return 1.0;
    return clamp(float(qNew) / float(qOld), 1.0 / GI_BIN_RMAX, GI_BIN_RMAX);   // (both ways: it chains 64 frames)
}
// The cell's resolved irradiance; false: no cell or no samples yet.
// Light li changed since last frame (GI_F_LIGHT_CHG; the host's mask).
bool giLightChanged(uint li) { return li < 256u && (gi.chg[li >> 7u][(li >> 5u) & 3u] & (1u << (li & 31u))) != 0u; }
// sphTest: inSph = the cell lies in one of this frame's HARD change spheres (the caller decides what that
// means; giCacheRead rejects it). false: no sphere loop, inSph false.
bool giCacheReadCell(uint s, bool sphTest, out vec3 E, out bool inSph)
{
    E = vec3(0.0);
    inSph = false;
    if (s == 0xFFFFFFFFu) return false;
    const uvec4 c0 = gi_data[2u * s];
    const uvec4 c1 = gi_data[2u * s + 1u];
    if ((c0.y & 0xFFFFu) == 0u) return false;
    if (sphTest)
    {
        const vec3 centre = vec3(uintBitsToFloat(c0.zw), uintBitsToFloat(c1.x));
        inSph = giSceneChanged(centre, 0.87 * uintBitsToFloat(c1.w));
    }
    const vec2 rg = unpackHalf2x16(c1.y);
    E = vec3(rg, unpackHalf2x16(c1.z).x);
    if (any(isnan(E)) || any(isinf(E))) return false;
    // Cache priors/forward reads obey the CURRENT clamp too (settings may have
    // changed since this cell was resolved). The prior never feeds the accumulator.
    const float clampL = gi.prm2.w, lum = giLum(E);
    if (clampL > 0.0 && lum > clampL) E *= clampL / lum;
    return true;
}
// The cell's resolved irradiance for the readers that run BEFORE this frame's resolve (the GI hits'
// ambient, the reuse / disocclusion priors): a cell inside a HARD change sphere holds
// irradiance from before the change -- rejected (907cecad7e). A soft (emitter) sphere's cell is only
// stale by a frame or so: kept (rejecting it dropped those readers to the probes -- a flash every time a
// big emitter changed; the resolve shortens its mean). The FORWARD reader (giCacheForward) runs
// after the resolve and takes the cell as the resolve left it (#1438; under GI_F_LAG its copy is one
// resolve behind, the lag every GI consumer of that mode has).
bool giCacheRead(uint s, out vec3 E)
{
    bool inSph;
    const bool ok = giCacheReadCell(s, true, E, inSph);
    if (inSph) E = vec3(0.0);
    return ok && !inSph;
}
// (GI_F_LAG) The lagged combine's reader: it shows last frame's GI, and the cells as last frame's resolve left
// them are part of that frame -- a change in a HARD sphere reaches them one frame late like everything else in
// the mode (giCacheRead's rejection sent the combine's pixels there to the probes for a frame: a flash). A
// global recovery clears the cache before the combine, so nothing stale is found then.
bool giCacheReadLag(uint s, out vec3 E)
{
    bool inSph;
    return giCacheReadCell(s, false, E, inSph);
}
// A FORWARD surface's cache irradiance at P (giForwardRefl): it has no GI pixel of its own and reads
// the cells directly. Its own cell alone drew the cell grid on glass -- squares on a window facing an
// axis, polygons where a tilted or curved pane cuts the cubes -- so (GI_FWD_FILTER_SHIFT) 2 weights
// the 8 cells around P by their centres (trilinear), 1 the 4 of its own cell layer (half the finds;
// the steps where a tilted face crosses a layer stay), 0 its own cell. Cells with no estimate drop
// out; w: the weight the found ones carry (0: none -- the probes stand).
// (#1438) NO change-sphere rejection here: this reader runs after the frame's resolve, which already
// handled the spheres' cells (GI_SPHERE_CACHE_FRAMES). Rejecting them as well dropped glass to the probes'
// irradiance and UNOCCLUDED probe specular on every frame a sphere touched its cells -- the emitter
// spheres are queued on most frames -- so windows flashed brighter while GI was on. inSph (only
// computed for the GI_DBG_CACHE view): a found cell lies in a change sphere this frame.
vec3 giCacheForward(vec3 P, vec3 N, vec3 cam, out float w, out bool inSph)
{
    w = 0.0;
    inSph = false;
    float edge;
    const int level = giCellLevel(P, cam, edge);
    const uint axis = giCellAxis(N);
    const uint bucket = giCellBucket(N, axis);
    const uint mode = (gi.cfg.x >> GI_FWD_FILTER_SHIFT) & 3u;
    const bool sphTest = gi.cfg.w == GI_DBG_CACHE;   // (a header word: uniform)
    vec3 E;
    bool sp;
    if (mode == 0u)
    {
        if (!giCacheReadCell(giCacheFind(giCellKeyQ(ivec3(floor(P / edge)), level, bucket)), sphTest, E, inSph))
            return vec3(0.0);
        w = 1.0;
        return E;
    }
    const vec3 g = P / edge - 0.5;
    ivec3 b = ivec3(floor(g));
    vec3 f = g - vec3(b);
    if (mode == 1u)
    {
        b[axis] = int(floor(P[axis] / edge));   // the face's own layer: its far corners weigh 0
        f[axis] = 0.0;
    }
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 8; ++i)
    {
        const ivec3 o = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        const vec3 t = mix(1.0 - f, f, vec3(o));
        const float wi = t.x * t.y * t.z;
        if (wi < 1e-3) continue;   // (no find for a corner that would not show)
        if (giCacheReadCell(giCacheFind(giCellKeyQ(b + o, level, bucket)), sphTest, E, sp))
        {
            sum += wi * E;
            w += wi;
            inSph = inSph || sp;
        }
    }
    return w > 0.0 ? sum / w : vec3(0.0);
}

// ---- reservoirs (GI-2 / GI-3): 2 uvec4 per texel and parity, set 3 binding 22 ----
//   [0] x_s (3 x float bits), W (float bits)
//   [1] x: n_s (snorm oct 2 x 16), y: half(L.r, L.g), z: half(L.b, M), w: half(the distance from
//       the receiver that took it to x_s (0 = a sky sample), |cos phi| there)
struct GiSample { vec3 xs; vec3 ns; vec3 L; float dist; float cosv; };
vec2 giOctWrap(vec2 v) { return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0); }
uint giPackN(vec3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-6);
    const vec2 e = n.z >= 0.0 ? n.xy : giOctWrap(n.xy);
    return packSnorm2x16(e);
}
vec3 giUnpackN(uint v)
{
    const vec2 e = unpackSnorm2x16(v);
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    const float t = max(-n.z, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return giNormalize(n);
}
#endif
