// vkclean -- MOVER PROBE (RenderVKMoverProbePixel, Persist 0): what the mover shadow routing computed at ONE
// pixel -- the screen centre -- read back and logged once a second (tag VKMoverProbe). Instrumentation only:
// nothing here changes a result. The RT shadow mask (clean_rt_shadow_mask.comp) and its spatial pass
// (clean_rt_mask_spatial.comp) write into the frame slot's host-visible guard buffer (RT set binding 15; words
// from RTP_BASE, past the RT candidate diagnostic's RTD_END); the lighting's side (the upsample taps and the
// final ratios) goes to the lit guard words (clean_deferred_light.comp, LG_MPR_*). Host: LLCleanRTVK::guardPrepare
// arms a slot (header words), guardHarvest -> mprHarvest logs it; LLCleanRendererVK::readLitGuardStats the
// lighting's words.
// Header (host, every armed slot; the rest of the region zeroed):
//   [0] 1 = armed   [1] the probed RENDER pixel x | y << 16   [2] the probed MASK texel x | y << 16   [3] RT frame
// MASK region (RTP_MASK, the texel's record; word 0 = 0: the texel was not traced this frame):
//   fixed words RTP_M_* below, then RTP_EV_WORDS-word EVENTS from RTP_M_EV (rtpEvent: one per light visit and one
//   per disc ray -- see the host's decoder, llcleanrtaudit.cpp mprHarvest, for the field meanings).
// EVENT TYPES (word 0 & 15): 1 a LIGHT (rtpLight: phases 0 scan, 1 mover rays, 2 static ray, 3 the sun, 4 gated out,
//   5 PASS 3 SKIPPED its static ray -- bits 1 the walled rule, 2 the contribution cut), 2 a disc RAY (rtpRay), 3 a light's
//   RAY GEOMETRY and 4 its two DIAGNOSTIC centre rays (mvProbeRays, pass 2, every eligible light: what the mask's mover
//   ray and pass 15's all-caster ray meet from the SAME origin, traced even where the core traced nothing).
// SPATIAL region (RTP_SP): per a-trous iteration k of the REST slot at the texel, RTP_SP_K words.
#ifndef CLEAN_RT_MOVER_PROBE_GLSL
#define CLEAN_RT_MOVER_PROBE_GLSL
#define RTP_BASE      8772u   // = RTD_END (clean_rt_common.glsl) = LLCleanRTVK::kRtpBase
#define RTP_HDR       8u
#define RTP_MASK      (RTP_BASE + RTP_HDR)
#define RTP_MASK_N    2560u   // (2026-11-13: x5 -- the per-light geometry / diagnostic-ray events, ~25 events a light)
#define RTP_SP        (RTP_MASK + RTP_MASK_N)
#define RTP_SP_N      256u
// (RenderVKShadowMoverMask 2) PASS 15's MOVER-EXACT RECORD of the probed texel (clean_deferred_light.comp: SELECT's
// pdnMoverExactPlan, SHADE's exact sum; the host's VKMoverProbe "mover exact" lines):
//   [0] 1 written | 2 exact | 4 by a light segment | 8 by the countdown | 16 wanted but the list was full | 32 the tile's
//       candidates overflowed (every mover tested) | 64 the bounds list overflowed | last frame's countdown << 8 (4 bits) |
//       this frame's countdown << 12 (4 bits, SHADE) | the lights reaching it << 16 (9 bits)
//   [1] the denoiser texel x | y << 16  [2] the light whose segment met a mover first (index + 1; 0 none)  [3] the tile's
//   candidate movers (| 1 << 31 overflow)  [4] the reach (m, float)  [5] its first job  [6] / [7] the exact sum's diffuse /
//   specular radiance (luminance, before demodulation)  [8] the diffuse written (demodulated, luminance)  [9] lights listed
//   [RTP_X_L + RTP_X_LW q] light q < RTP_X_LIGHTS of its walk: [0] index + 1 | 1 << 16 its segment meets a mover | the
//   first such mover (the frame's list: avatars first) << 17 (0x7FFF none)  [1] its diffuse term t (luminance)  [2] its
//   specular term  [3] its visibility V  [4] the nearest blocker's distance (65000: none)  [5] 1 lit here | 2 the ray met a
//   MOVING caster | 4 coloured | (index + 1) << 8
#define RTP_X         (RTP_SP + RTP_SP_N)
#define RTP_X_N       512u
#define RTP_X_L       16u
#define RTP_X_LW      6u
#define RTP_X_LIGHTS  64u
#define RTP_END       (RTP_X + RTP_X_N)      // = LLCleanRTVK::kRtpEnd
// mask fixed words
#define RTP_M_FLAGS   0u    // 1 traced | 2 mover route | 4 fold | 8 rest on | 16 a mover ray blocked | 32 the local-light
                            // mover block ran | 64 sun mover-only | 128 no mover light listed (g_mv_nloc 0) | 256 carried
                            // | 512 sky | 1024 the texel's light cluster overflowed (its mover lights: every light, mvNextLight)
                            // | 2048 the RT PROJECTOR loop took the texel (!rtProjStochastic: the mover block never ran)
#define RTP_M_PX      1u    // mask texel x | y << 16
#define RTP_M_RP      2u    // render pixel x | y << 16
#define RTP_M_P       3u    // [3] receiver P xyz (float bits)
#define RTP_M_NG      6u    // [3] Ng
#define RTP_M_NL      9u    // eligible lights in range of the texel (the mover walk's positions, mvNextLight)
#define RTP_M_NHIT    10u   // eligible lights with a mover on their segment
#define RTP_M_NLOC    11u   // g_mv_nloc | g_mv_loc_all << 31
#define RTP_M_TOP     12u   // [4] top lights (index + 1)
#define RTP_M_FOLD    16u   // (radiance packing) THE EXACT PIXEL FOLD's plan (clean_rt_shadow_mask.comp mvFoldPlan):
                            // p | q << 4 | N << 8 | D << 9 | 0x800 slots written | C lights seen << 12 | Q seen << 20
                            // (C: blocked + statically reached; Q: other lights in a static shadow; 0: no slot)
#define RTP_M_FOLDL   17u   // (radiance packing) THE FOLD'S LIGHT LISTS (mvFoldWrite / mvFoldWriteHit, the probe only):
                            // [0..3] the C lights kept (index + 1, lowest first: explicit-slot candidates -- a mover on
                            // their segment AND Vs above MV_VS_OUT), [4..7] the Q lights kept (a static shadow: Vs slot
                            // candidates), [8] g_fc_n | g_fq_n << 4 | g_fh_n << 8 | blkOver << 12 | cBits << 16 |
                            // cBlk << 20 | the write path << 28 (1 mvFoldWrite, 2 mvFoldWriteHit), [9] the H lights
                            // (unblocked hit lights: Vm 1 slots) index + 1, 8 bits each
#define RTP_M_REST    27u   // [0] restN lum [1] restD lum [2] restW lum (floats) [3] k (slots written) [4..7] ids
                            // [8..11] the slots' values (luminance, floats) [12] mover + weight rays traced
#define RTP_M_EVN     47u   // events written (may exceed the room: the rest dropped)
#define RTP_M_EV      48u
#define RTP_EV_WORDS  9u    // = LLCleanRTVK::kRtpEvWords (a light event: [8] its identity, mvLightInfo)
#define RTP_EV_CAP    ((RTP_MASK_N - RTP_M_EV) / RTP_EV_WORDS)
// spatial words, per a-trous iteration k < 4 of the REST slot (RTP_SP_K each): [0] 1 | k << 4 | n << 8 | on << 16 |
// young << 17 | to-scratch << 18 | slot << 20   [1] input lum   [2] output lum   [3] radius r   [4] the weights' sum
// [5] history length   [6 .. 30] the 5 x 5 taps' weights (float; -1 = skipped: sky / no REST slot there)
// [31 .. 55] the taps' input luminances
#define RTP_SP_K      56u
#define RTP_SP_ITERS  4u
#endif
