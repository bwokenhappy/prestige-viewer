/**
 * @file llcleangivk.cpp
 * @brief RAY-TRACED GLOBAL ILLUMINATION (#87; ReSTIR GI + a world-space radiance cache;
 *        RenderVKGI, default off). The shader side and the data contract are in
 *        indra/vkclean/shaders/clean_gi_common.glsl; this file is the host side.
 *
 * FRAME (split deferred lighting with ray tracing up; one decision per recording):
 *   1. giDecide (the light pass, before the frame info): the settings, the GI denoiser's images
 *      at the render extent x RenderVKGIScale, the state buffer (header + radiance cache), the
 *      reservoirs (GI-2+) -- mGiNow, and the frame info's foliage_rt.x bit 128: the consumers read
 *      the GI images only on such a frame;
 *   2. giWriteHeader (the light pass, with the other uploads): flags, parameters, the change
 *      spheres (GI-4), the counters zeroed;
 *   3. recordGI (after the RT reflections and the projector sampler, before BASE / COMBINE, on the
 *      lighting's queue): the GI trace (pass 11 -DCLEAN_GI_TRACE: one ray per GI texel, its hit shaded
 *      as the RT reflections shade theirs, reservoir reuse; #1031 RenderVKGISplitKernels: as the
 *      TRACE / SHADE / REUSE / RESHADE / TAIL stages over mGiHit; #1036 RenderVKGIShadeSplit: SHADE as
 *      MISS / HIT-MAT / HIT-LIGHT over TRACE's hit / miss lists), the cache's resolve (GI-4), the GI
 *      denoiser (the #85 temporal + a-trous, a second LLCleanDenoiserVK: its own images, never the
 *      shadow channels'), the stats copy;
 *   4. the combine / BASE (clean_deferred_light.comp deferReflIn): the denoised GI REPLACES the
 *      probes' diffuse irradiance where it has an estimate (x RenderVKGIIntensity, floored by
 *      RenderVKGIMinAmbient x the probes' value); the probes stay the specular and the far field.
 * VRAM: the GI denoiser (144 B per GI texel: ~127 MB at 1440p half extent), the reservoirs
 * (64 B per GI texel), the split stages' records (100 B per GI texel + 48 B: ~88 MB at 1440p half extent;
 * the #1036 shade split's lists and material records live in the same space),
 * the state (5.4 KB header + 80 B per cache slot: 64 B the cell, 16 B its direction words (gi-glass) -- 20 MB at
 * 2^18, + the 192 KB change bins). All fixed by the
 * extent / the settings, allocated soft (#697: a refusal turns GI off and retries in 30 s), freed
 * when GI is switched off or held unused for kRtReflHoldSecs.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 * $/LicenseInfo$
 */
#include "linden_common.h"

#include "llcleanrenderervk.h"
#include "llcleandenoisevk.h"
#include "llcleanrtvk.h"
#include "../llcleaninstancestore.h"
#include "llxrsession.h"

#include "llviewercontrol.h"
#if defined(LL_HAS_CLEAN_PIPELINE)
#  include "llviewerobjectlist.h"   // (#1406) the GI delta log's local id
#endif
#include "../llvkhitch.h"
#include "vk_common.h"
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

namespace
{
double giClock()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
uint32_t giDim(uint32_t r, float s) { return std::max(1u, (uint32_t)std::ceil((float)r * s)); }
VkShaderModule giModule(VkDevice dev, const uint32_t* code, size_t bytes, const char* name)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
    vkNoteShaderModule(m, code, bytes, name);
    return m;
}
const VkSpecializationInfo* giVrSpec(uint32_t views);
// The lighting kernels' debug specialization OFF (as llcleanprojstochvk.cpp pdnLitSpec). (#1608) views > 1: + kViewCount
// = views (giVrSpec's info: the GI kernels per view in a stereo session); views 1: the one-entry info of before.
const VkSpecializationInfo* giLitSpec(uint32_t views = 1)
{
    if (views > 1u) return giVrSpec(views);
    static const VkBool32 kOff = VK_FALSE;
    static const VkSpecializationMapEntry kEntry{1u, 0u, sizeof(VkBool32)};
    static const VkSpecializationInfo kSiOff{1u, &kEntry, sizeof(VkBool32), &kOff};
    return &kSiOff;
}
// clean_gi_common.glsl GI_F_*
constexpr uint32_t GI_F_ON = 1u, GI_F_FINAL_IN = 2u, GI_F_TEMPORAL = 4u, GI_F_SPATIAL = 8u, GI_F_CACHE = 16u,
                   GI_F_RES = 32u, GI_F_RES_PREV = 64u, GI_F_CACHE_FILL = 128u, GI_F_LIGHT_CHG = 65536u,
                   GI_F_BACK_STOP = 131072u, GI_F_POINT_SHADOW = 262144u, GI_F_REPROJ_SEARCH = 524288u,
                   GI_F_UPS_NORMAL = 1048576u, GI_F_STATS = 2097152u, GI_F_LAG = 8388608u, GI_F_LAG_STALE = 16777216u,
                   GI_F_REUSE_MV = 33554432u,   // (#1038)
                   GI_F_VIS_TSKIP = 268435456u, // (#985)
                   GI_F_STABLE_SAMPLES = 2147483648u;   // (RenderVKGIStableSamples)
// One change sphere into the next header's list: up to the header's 64; past them a merge within the
// bound or, for a soft change, the least relevant one to the viewer at focus let go
// (LLCleanChangeSpheres::add -- a coarser invalidation instead of wiping the whole cache, and never a
// hard sphere grown over a soft region). s_giPairBudget: the closest-pair searches (O(64^2) each) left
// until the next header. True: a soft change was let go.
uint32_t s_giPairBudget = 16u;
bool giAddSphere(std::array<LLCleanChangeSpheres::Sphere, 64u>& v, uint32_t& n,
                 const LLCleanChangeSpheres::Sphere& s, const float* focus)
{
    using LLCleanChangeSpheres::Outcome;
    Outcome how = Outcome::Appended;
    LLCleanChangeSpheres::add(v.data(), n, 64u, s, s_giPairBudget, focus, how);
    return how == Outcome::Dropped || how == Outcome::Replaced;
}
// The state buffer's cache part after the header: 64 B per slot, then (the sealed-room fade) the change
// bins -- 3 banks of 2 x GI_BIN_N uvec4 (clean_gi_common.glsl GI_BIN_N 2048, GI_BIN_BANK): 192 KB, fixed.
constexpr VkDeviceSize kGiBinBytes = 3u * (2u * 2048u) * 16u;
// (gi-glass) + after the bins, the DIRECTION words (clean_gi_common.glsl GI_DIR_BASE): 16 B per slot -- 4 MB at the
// default 2^18 slots; written only while RenderVKGICacheSpec runs, always allocated with the cache (no realloc on toggle).
VkDeviceSize giCacheBytes(uint32_t slots) { return slots ? (VkDeviceSize)slots * (64u + 16u) + kGiBinBytes : 0u; }
// The settings (all live).
struct GiCfg
{
    U32 mode = 0;          // RenderVKGI: 0 off, 1 one bounce, 2 + temporal reuse, 3 + spatial reuse
    bool vrOff = false;    // #1076: RenderVKGI is on but a VR session runs (mode forced 0; setting untouched)
    F32 scale = 0.5f;      // RenderVKGIScale
    F32 intensity = 1.f;   // RenderVKGIIntensity
    F32 minAmb = 0.f;      // RenderVKGIMinAmbient
    F32 rayLen = 64.f;     // RenderVKGIRayLength
    U32 hitAmb = 2;        // RenderVKGIHitAmbient
    F32 firefly = 4.f;     // RenderVKGIFireflyClamp
    U32 atrous = 3;        // RenderVKGIAtrous
    F32 history = 32.f;    // RenderVKGIHistory
    F32 clamp = 2.f;       // RenderVKGIClamp
    bool hitGrid = true;   // RenderVKGIHitLightGrid
    U32 hitShadow = 1;     // RenderVKGIHitShadowRays
    U32 debug = 0;         // RenderVKGIDebug
    U32 mcap = 16;         // RenderVKGIMCap
    U32 taps = 3;          // RenderVKGISpatialTaps
    U32 radius = 16;       // RenderVKGISpatialRadius
    bool cache = true;     // RenderVKGICache
    U32 slotsLog2 = 18;    // RenderVKGICacheSlotsLog2
    F32 cell = 0.25f;      // RenderVKGICacheCellSize
    F32 levelDist = 8.f;   // RenderVKGICacheLevelDistance
    F32 window = 32.f;     // RenderVKGICacheFrames
    F32 evict = 600.f;     // RenderVKGICacheEvictFrames
    U32 specOcc = 1;       // RenderVKGISpecOcclusion
    U32 fwdFilter = 2;     // RenderVKGIForwardCacheFilter
    U32 everyLog = 0;      // RenderVKGITraceEvery 1 / 2 / 4 -> 0 / 1 / 2
    bool missProbes = true;   // RenderVKGIMissProbes
    bool cullBack = true;  // RenderVKGICullBackFaces
    bool celOcc = true;    // RenderVKGICelOcclusion
    bool hazeOcc = true;   // RenderVKGIHazeOcclusion
    bool specOccRtRes = true;   // RenderVKGISpecOcclusionRTResidual
    bool lightChg = true;  // RenderVKGILightChangeCut
    F32 chgHist = 8.f;     // RenderVKGILightChangeHistory
    F32 cosMin = 0.05f;    // RenderVKGIReuseCosMin
    F32 reshadeGain = 2.f; // RenderVKGIReshadeGain
    F32 reshadeThr = 0.1f; // RenderVKGIReshadeThreshold
    bool backStop = true;  // RenderVKGIBackFaceStop
    bool pointShadow = true;   // RenderVKGIHitPointShadows
    U32 disoccFix = 4;     // RenderVKGIDisoccFix
    bool reprojSearch = true;  // RenderVKGIReprojSearch
    U32 disoccTaps = 4;    // RenderVKGIDisoccTaps
    bool upsNormal = true; // RenderVKGIUpsampleNormal
    bool split = true;     // RenderVKGISplitKernels (#1031)
    bool shadeSplit = true;    // RenderVKGIShadeSplit (#1036)
    bool reuseMotion = true;   // RenderVKGIReuseMotion (#1038)
    U32 disoccCache = 4;   // RenderVKGIDisoccCache (#1038)
    bool youngBoost = true;    // RenderVKDenoiseYoungBoost (#1038; the GI denoiser's half)
    bool moveZTol = true;  // RenderVKDenoiseMovingDepthTol (#1038; the GI denoiser's half)
    U32 moveMCap = 2;      // RenderVKGILightMoveMCap (#1087)
    F32 moveCache = 4.f;   // RenderVKGILightChangeCacheFrames (#1087)
    bool visTSkip = false; // RenderVKGIReuseVisSkip (#985)
    bool stable = false;   // RenderVKGIStableSamples (below full extent: the fixed sample lattice, GI_F_STABLE_SAMPLES; A/B)
    bool movers = false;   // RenderVKGIMoverSamples (full-resolution GI on mover pixels: clean_gi_movers.glsl; A/B)
    bool sampleGrid = true;    // RenderVKGIHistoryJitter (the GI denoiser reads its history on last frame's jittered lattice)
    bool layerFill = true; // RenderVKGILayerFill (the front alpha-blend layer fills its radiance-cache cells: recordGILayers)
    F32 moverBudget = 0.25f;   // RenderVKGIMoverBudget (the mover list's cap: this x the GI texel count)
};
GiCfg giCfg()
{
    GiCfg c;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32>  s_mode(gSavedSettings, "RenderVKGI", 0U);
    static LLCachedControl<F32>  s_scale(gSavedSettings, "RenderVKGIScale", 0.5f);
    static LLCachedControl<F32>  s_int(gSavedSettings, "RenderVKGIIntensity", 1.f);
    static LLCachedControl<F32>  s_min(gSavedSettings, "RenderVKGIMinAmbient", 0.f);
    static LLCachedControl<F32>  s_len(gSavedSettings, "RenderVKGIRayLength", 64.f);
    static LLCachedControl<U32>  s_amb(gSavedSettings, "RenderVKGIHitAmbient", 2U);
    static LLCachedControl<F32>  s_ff(gSavedSettings, "RenderVKGIFireflyClamp", 4.f);
    static LLCachedControl<U32>  s_at(gSavedSettings, "RenderVKGIAtrous", 3U);
    static LLCachedControl<F32>  s_hist(gSavedSettings, "RenderVKGIHistory", 32.f);
    static LLCachedControl<F32>  s_clamp(gSavedSettings, "RenderVKGIClamp", 2.f);
    static LLCachedControl<bool> s_grid(gSavedSettings, "RenderVKGIHitLightGrid", true);
    static LLCachedControl<U32>  s_hsr(gSavedSettings, "RenderVKGIHitShadowRays", 1U);
    static LLCachedControl<U32>  s_dbg(gSavedSettings, "RenderVKGIDebug", 0U);
    static LLCachedControl<U32>  s_mcap(gSavedSettings, "RenderVKGIMCap", 16U);
    static LLCachedControl<U32>  s_taps(gSavedSettings, "RenderVKGISpatialTaps", 3U);
    static LLCachedControl<U32>  s_rad(gSavedSettings, "RenderVKGISpatialRadius", 16U);
    static LLCachedControl<bool> s_cache(gSavedSettings, "RenderVKGICache", true);
    static LLCachedControl<U32>  s_slots(gSavedSettings, "RenderVKGICacheSlotsLog2", 18U);
    static LLCachedControl<F32>  s_cell(gSavedSettings, "RenderVKGICacheCellSize", 0.25f);
    static LLCachedControl<F32>  s_ld(gSavedSettings, "RenderVKGICacheLevelDistance", 8.f);
    static LLCachedControl<F32>  s_win(gSavedSettings, "RenderVKGICacheFrames", 32.f);
    static LLCachedControl<F32>  s_ev(gSavedSettings, "RenderVKGICacheEvictFrames", 600.f);
    static LLCachedControl<U32>  s_so(gSavedSettings, "RenderVKGISpecOcclusion", 1U);
    static LLCachedControl<U32>  s_fwdf(gSavedSettings, "RenderVKGIForwardCacheFilter", 2U);
    static LLCachedControl<U32>  s_every(gSavedSettings, "RenderVKGITraceEvery", 1U);
    static LLCachedControl<bool> s_missp(gSavedSettings, "RenderVKGIMissProbes", true);
    static LLCachedControl<bool> s_cull(gSavedSettings, "RenderVKGICullBackFaces", true);
    static LLCachedControl<bool> s_celocc(gSavedSettings, "RenderVKGICelOcclusion", true);
    static LLCachedControl<bool> s_hazeocc(gSavedSettings, "RenderVKGIHazeOcclusion", true);
    static LLCachedControl<bool> s_sortres(gSavedSettings, "RenderVKGISpecOcclusionRTResidual", true);
    static LLCachedControl<U32>  s_lchg(gSavedSettings, "RenderVKGILightChangeCut", 1U);
    static LLCachedControl<F32>  s_lchgh(gSavedSettings, "RenderVKGILightChangeHistory", 8.f);
    static LLCachedControl<F32>  s_cosmin(gSavedSettings, "RenderVKGIReuseCosMin", 0.05f);
    static LLCachedControl<F32>  s_reshadeGain(gSavedSettings, "RenderVKGIReshadeGain", 2.f);
    static LLCachedControl<F32>  s_reshadeThr(gSavedSettings, "RenderVKGIReshadeThreshold", 0.1f);
    static LLCachedControl<U32>  s_bstop(gSavedSettings, "RenderVKGIBackFaceStop", 1U);
    static LLCachedControl<U32>  s_pshad(gSavedSettings, "RenderVKGIHitPointShadows", 1U);
    static LLCachedControl<U32>  s_dfix(gSavedSettings, "RenderVKGIDisoccFix", 4U);
    static LLCachedControl<U32>  s_rsearch(gSavedSettings, "RenderVKGIReprojSearch", 1U);
    static LLCachedControl<U32>  s_dtaps(gSavedSettings, "RenderVKGIDisoccTaps", 4U);
    static LLCachedControl<U32>  s_upsn(gSavedSettings, "RenderVKGIUpsampleNormal", 1U);
    static LLCachedControl<bool> s_split(gSavedSettings, "RenderVKGISplitKernels", true);
    static LLCachedControl<bool> s_shsplit(gSavedSettings, "RenderVKGIShadeSplit", true);
    static LLCachedControl<bool> s_rmv(gSavedSettings, "RenderVKGIReuseMotion", true);
    static LLCachedControl<U32>  s_dcache(gSavedSettings, "RenderVKGIDisoccCache", 4U);
    static LLCachedControl<bool> s_yboost(gSavedSettings, "RenderVKDenoiseYoungBoost", true);
    static LLCachedControl<bool> s_mztol(gSavedSettings, "RenderVKDenoiseMovingDepthTol", true);
    static LLCachedControl<U32>  s_mmcap(gSavedSettings, "RenderVKGILightMoveMCap", 2U);
    static LLCachedControl<F32>  s_mcache(gSavedSettings, "RenderVKGILightChangeCacheFrames", 4.f);
    static LLCachedControl<bool> s_vtskip(gSavedSettings, "RenderVKGIReuseVisSkip", false);
    static LLCachedControl<bool> s_stable(gSavedSettings, "RenderVKGIStableSamples", false);
    static LLCachedControl<bool> s_movers(gSavedSettings, "RenderVKGIMoverSamples", false);
    static LLCachedControl<bool> s_sgrid(gSavedSettings, "RenderVKGIHistoryJitter", true);
    static LLCachedControl<bool> s_layerFill(gSavedSettings, "RenderVKGILayerFill", true);
    static LLCachedControl<F32>  s_mbudget(gSavedSettings, "RenderVKGIMoverBudget", 0.25f);
    c.mode = std::min<U32>(s_mode, 3u);
    c.scale = s_scale >= 0.75f ? 1.f : (s_scale >= 0.375f ? 0.5f : 0.25f);
    c.intensity = std::clamp((F32)s_int, 0.f, 16.f);
    c.minAmb = std::clamp((F32)s_min, 0.f, 1.f);
    c.rayLen = std::clamp((F32)s_len, 1.f, 1024.f);
    c.hitAmb = std::min<U32>(s_amb, 3u);
    c.firefly = std::max((F32)s_ff, 0.f);
    c.atrous = s_at;
    c.history = std::clamp((F32)s_hist, 1.f, 63.f);
    c.clamp = std::clamp((F32)s_clamp, 0.5f, 8.f);
    c.hitGrid = s_grid;
    // (the sealed-room leak, clean_hit_nee.h) at least one ray: the hit's shadowable lights reach it only through
    // the sampled rays, so 0 would drop them -- never light them unshadowed
    c.hitShadow = std::clamp<U32>(s_hsr, 1u, 4u);
    c.debug = std::min<U32>(s_dbg, 13u);   // (10: the spatial taps' rejections, #1406; 11 / 12: the speck overlay; 13: ray outcome)
    c.mcap = std::clamp<U32>(s_mcap, 1u, 64u);
    c.taps = std::min<U32>(s_taps, 6u);
    c.radius = std::clamp<U32>(s_rad, 1u, 64u);
    c.cache = s_cache;
    c.slotsLog2 = std::clamp<U32>(s_slots, 12u, 22u);
    c.cell = std::clamp((F32)s_cell, 0.05f, 8.f);
    c.levelDist = std::clamp((F32)s_ld, 1.f, 256.f);
    c.window = std::clamp((F32)s_win, 1.f, 1024.f);
    c.evict = std::clamp((F32)s_ev, 16.f, 100000.f);
    c.specOcc = std::min<U32>(s_so, 2u);
    c.fwdFilter = std::min<U32>(s_fwdf, 2u);
    c.everyLog = s_every >= 4u ? 2u : (s_every >= 2u ? 1u : 0u);
    c.missProbes = s_missp;
    c.cullBack = s_cull;
    c.celOcc = s_celocc;
    c.hazeOcc = s_hazeocc;
    c.specOccRtRes = s_sortres;
    c.lightChg = s_lchg != 0u;
    c.chgHist = std::clamp((F32)s_lchgh, 0.f, 15.f);
    c.cosMin = std::clamp((F32)s_cosmin, 0.f, 0.5f);
    c.reshadeGain = std::clamp((F32)s_reshadeGain, 0.f, 16.f);
    c.reshadeThr = std::clamp((F32)s_reshadeThr, 0.f, 4.f);
    c.backStop = s_bstop != 0u;
    c.pointShadow = s_pshad != 0u;
    c.disoccFix = std::min<U32>(s_dfix, 7u);
    c.reprojSearch = s_rsearch != 0u;
    c.disoccTaps = std::min<U32>(s_dtaps, 8u);
    c.upsNormal = s_upsn != 0u;
    c.split = s_split;
    c.shadeSplit = s_shsplit;
    c.reuseMotion = s_rmv;
    c.disoccCache = std::min<U32>(s_dcache, 16u);
    c.youngBoost = s_yboost;
    c.moveZTol = s_mztol;
    c.moveMCap = std::min<U32>(s_mmcap, 64u);
    c.moveCache = std::clamp((F32)s_mcache, 0.f, 1024.f);
    c.visTSkip = s_vtskip;
    c.stable = s_stable;
    c.movers = s_movers;
    c.sampleGrid = s_sgrid;
    c.layerFill = s_layerFill;
    c.moverBudget = std::clamp((F32)s_mbudget, 0.01f, 4.f);
#endif
    // (#1608, owner direction 2026-11-18: every feature works in VR -- the #1076 override that forced GI off while a VR
    // session ran is gone; a layered stereo frame runs RenderVKGI per eye, SlipstreamVRGI picks the cheaper world-cache
    // tier instead. vrOff stays false.)
    return c;
}
// (#1139) VR GI, the world-cache tier (giDecideVr): the desktop's GI settings with the VR preset laid over
// them -- never written, like the session's mode override above. One bounce into the shared radiance cache
// (no reservoirs, no GI denoiser: nothing per eye on screen), the hits' ambient from the cache (multi-
// bounce), a quarter of the per-eye render extent (the upscaler's internal resolution) per axis, and
// SlipstreamVRGITraceEvery of those texels per frame (2: a checkerboard; the eyes take complementary ones).
// The fused trace only -- a choice of the tier (the split's records / lists are per view since #1608, the desktop's GI).
GiCfg giVrCfg()
{
    GiCfg c = giCfg();
    U32 every = 2u;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_every(gSavedSettings, "SlipstreamVRGITraceEvery", 2U);
    every = s_every;
#endif
    c.mode = 1u;
    c.vrOff = false;
    c.scale = 0.25f;
    c.cache = true;
    c.hitAmb = 2u;   // GI_HIT_AMB_CACHE: the cache, the probes where it has no cell
    c.everyLog = every >= 4u ? 2u : (every >= 2u ? 1u : 0u);
    c.split = false;
    c.shadeSplit = false;
    // (no reservoirs, no GI denoiser: the stable lattice and the mover samples are the desktop's -- VR as before)
    c.stable = false;
    c.movers = false;
    c.layerFill = false;
    return c;
}
// The VR GI kernels' specialization: debug views off (constant 1, as giLitSpec) + kViewCount (constant 20)
// = the session's view count (llcleanrenderervk.cpp litDebugSpec(false, views)'s shape).
const VkSpecializationInfo* giVrSpec(uint32_t views)
{
    struct Data { VkBool32 dbg; int32_t views; };
    static_assert(sizeof(Data) == 8, "giVrSpec offsets");
    static const VkSpecializationMapEntry kEntries[2] = {{1u, 0u, sizeof(VkBool32)},
                                                         {GraphicsPipelineDesc::kViewCountConstId, 4u, sizeof(int32_t)}};
    static const Data kData[3] = {{VK_FALSE, 2}, {VK_FALSE, 3}, {VK_FALSE, 4}};
    static const VkSpecializationInfo kInfo[3] = {{2u, kEntries, sizeof(Data), &kData[0]},
                                                  {2u, kEntries, sizeof(Data), &kData[1]},
                                                  {2u, kEntries, sizeof(Data), &kData[2]}};
    return &kInfo[std::clamp<uint32_t>(views, 2u, 4u) - 2u];
}
} // namespace

// pdnInit (initDeferSplit): the GI denoiser always -- set 3 bindings 18-21 name its images or its
// placeholders in every split frame -- and the GI kernels when the RT layout exists.
void LLCleanRendererVK::giInit(VkDevice dev)
{
    if (!mGiDn)
    {
        mGiDn = new LLCleanDenoiserVK();
        // (THE NARROW AGE: the GI has no sun / split channel or mover record -- its AGE is r32ui, 24 B per texel less)
        // (#1608: its images a layer set per view, its passes z = N -- clean_dn_common.glsl DN_L)
        if (!mGiDn->init(mCtx, mDslDraw0, mDepthSampler, "GI denoiser", true, mViewCount))
        {
            LL_WARNS("Vulkan") << "GI denoiser: init failed; ray-traced GI unavailable (RenderVKGI acts as 0)" << LL_ENDL;
            mGiDn->destroy();
            delete mGiDn;
            mGiDn = nullptr;
        }
    }
    writeGiMotion();   // (the GI denoiser's binding 5 + #1038 set 3 binding 28: valid from init on)
    // (#1029) The GI kernels below join the deferred ray-query group (rtDeferredCompute): compiled
    // after the world pipelines, installed together; every user checks its handle per frame.
    // (#1608: the per-view GI kernels carry kViewCount = N in a stereo session -- giLitSpec(mViewCount); the cache's
    //  resolve (world space) and the mover samples (desktop only) keep one view)
    if (mRtReflLayout && !mGiTracePipe)
    {
        VkShaderModule m = giModule(dev, kWorldDeferGiTraceSpv, kWorldDeferGiTraceSpv_bytes, "DeferGiTrace");
        rtDeferredCompute(dev, m, mRtReflLayout, 0u, giLitSpec(mViewCount), &mGiTracePipe, "mGiTracePipe (DeferGiTrace)");
        destroyShaderModuleDeferred(dev, m, nullptr);
    }
    if (mRtReflLayout && !mGiMoverPipe)
    {
        // (RenderVKGIMoverSamples) full-resolution GI on mover pixels: the LIST / TRACE kernel (clean_gi_movers.glsl)
        VkShaderModule m = giModule(dev, kWorldDeferGiMoverSpv, kWorldDeferGiMoverSpv_bytes, "DeferGiMover");
        rtDeferredCompute(dev, m, mRtReflLayout, 0u, giLitSpec(), &mGiMoverPipe, "mGiMoverPipe (DeferGiMover)");
        destroyShaderModuleDeferred(dev, m, nullptr);
    }
    if (mRtReflLayout && !mGiLayerPipe)
    {
        // (RenderVKGILayerFill) the front alpha-blend layer as a GI receiver: its radiance-cache cells
        VkShaderModule m = giModule(dev, kWorldDeferGiLayerSpv, kWorldDeferGiLayerSpv_bytes, "DeferGiLayer");
        rtDeferredCompute(dev, m, mRtReflLayout, 0u, giLitSpec(), &mGiLayerPipe, "mGiLayerPipe (DeferGiLayer)");
        destroyShaderModuleDeferred(dev, m, nullptr);
    }
    if (mRtReflLayout && !mGiResolvePipe)
    {
        // (GI-4) the radiance cache's resolve
        VkShaderModule m = giModule(dev, kWorldDeferGiResolveSpv, kWorldDeferGiResolveSpv_bytes, "DeferGiResolve");
        rtDeferredCompute(dev, m, mRtReflLayout, 0u, giLitSpec(), &mGiResolvePipe, "mGiResolvePipe (DeferGiResolve)");
        destroyShaderModuleDeferred(dev, m, nullptr);
    }
    if (mRtReflLayout)
    {
        // (#1031) the split stages (distinct module names: RenderVKPipelineStats logs each one's registers)
        struct Stage { const uint32_t* code; size_t bytes; const char* mod; const char* name; };
        const Stage st[GIS_COUNT] = {
            {kWorldDeferGiSplitTraceSpv, kWorldDeferGiSplitTraceSpv_bytes, "DeferGiSplitTrace", "mGiSplitPipe (DeferGiSplitTrace)"},
            {kWorldDeferGiSplitShadeSpv, kWorldDeferGiSplitShadeSpv_bytes, "DeferGiSplitShade", "mGiSplitPipe (DeferGiSplitShade)"},
            {kWorldDeferGiSplitReuseSpv, kWorldDeferGiSplitReuseSpv_bytes, "DeferGiSplitReuse", "mGiSplitPipe (DeferGiSplitReuse)"},
            {kWorldDeferGiSplitReshadeSpv, kWorldDeferGiSplitReshadeSpv_bytes, "DeferGiSplitReshade",
             "mGiSplitPipe (DeferGiSplitReshade)"},
            {kWorldDeferGiSplitChgTagSpv, kWorldDeferGiSplitChgTagSpv_bytes, "DeferGiSplitChgTag",
             "mGiSplitPipe (DeferGiSplitChgTag)"}};
        for (uint32_t i = 0; i < GIS_COUNT; ++i)
        {
            if (mGiSplitPipe[i]) continue;
            VkShaderModule m = giModule(dev, st[i].code, st[i].bytes, st[i].mod);
            rtDeferredCompute(dev, m, mRtReflLayout, 0u, giLitSpec(mViewCount), &mGiSplitPipe[i], st[i].name);
            destroyShaderModuleDeferred(dev, m, nullptr);
        }
        // (#1036) the shade split's stages
        const Stage sh[GSS_COUNT] = {
            {kWorldDeferGiSplitMissSpv, kWorldDeferGiSplitMissSpv_bytes, "DeferGiSplitMiss", "mGiShadePipe (DeferGiSplitMiss)"},
            {kWorldDeferGiSplitHitMatSpv, kWorldDeferGiSplitHitMatSpv_bytes, "DeferGiSplitHitMat",
             "mGiShadePipe (DeferGiSplitHitMat)"},
            {kWorldDeferGiSplitHitLightSpv, kWorldDeferGiSplitHitLightSpv_bytes, "DeferGiSplitHitLight",
             "mGiShadePipe (DeferGiSplitHitLight)"}};
        for (uint32_t i = 0; i < GSS_COUNT; ++i)
        {
            if (mGiShadePipe[i]) continue;
            VkShaderModule m = giModule(dev, sh[i].code, sh[i].bytes, sh[i].mod);
            rtDeferredCompute(dev, m, mRtReflLayout, 0u, giLitSpec(mViewCount), &mGiShadePipe[i], sh[i].name);
            destroyShaderModuleDeferred(dev, m, nullptr);
        }
    }
    giInitVr(dev);   // (#1139: a stereo session only)
    writeGiBindings();
}

// (#1031) the split stages can run this recording: every pipeline (async builds landed) + the records
bool LLCleanRendererVK::giSplitReady(uint32_t w, uint32_t h) const
{
    for (uint32_t i = 0; i < GIS_COUNT; ++i)
        if (!mGiSplitPipe[i]) return false;
    return mGiHit.buffer && mGiHitW == w && mGiHitH == h;
}

// (#1036) the shade split's three pipelines exist (async builds landed); else SHADE
bool LLCleanRendererVK::giShadeSplitReady() const
{
    for (uint32_t i = 0; i < GSS_COUNT; ++i)
        if (!mGiShadePipe[i]) return false;
    return true;
}

void LLCleanRendererVK::giDestroy()
{
    if (!mCtx) return;
    VkDevice dev = mCtx->device();
    if (mGiTracePipe) { vkDestroyPipeline(dev, mGiTracePipe, nullptr); mGiTracePipe = VK_NULL_HANDLE; }
    if (mGiResolvePipe) { vkDestroyPipeline(dev, mGiResolvePipe, nullptr); mGiResolvePipe = VK_NULL_HANDLE; }
    if (mGiMoverPipe) { vkDestroyPipeline(dev, mGiMoverPipe, nullptr); mGiMoverPipe = VK_NULL_HANDLE; }
    if (mGiLayerPipe) { vkDestroyPipeline(dev, mGiLayerPipe, nullptr); mGiLayerPipe = VK_NULL_HANDLE; }
    if (mGiMov.buffer) { mCtx->destroyBuffer(mGiMov); mGiMov = GpuBuffer{}; }
    if (mGiMovRb.buffer) { mCtx->destroyBuffer(mGiMovRb); mGiMovRb = GpuBuffer{}; }
    mGiMovW = mGiMovH = mGiMovCap = 0;
    mGiMovNow = mGiMovLastHave = false;
    for (bool& u : mGiMovRbUsed) u = false;
    for (VkPipeline& sp : mGiSplitPipe)
        if (sp) { vkDestroyPipeline(dev, sp, nullptr); sp = VK_NULL_HANDLE; }
    for (VkPipeline& sp : mGiShadePipe)
        if (sp) { vkDestroyPipeline(dev, sp, nullptr); sp = VK_NULL_HANDLE; }
    if (mGiTraceVrPipe) { vkDestroyPipeline(dev, mGiTraceVrPipe, nullptr); mGiTraceVrPipe = VK_NULL_HANDLE; }   // (#1139)
    if (mGiVrUberPipe) { vkDestroyPipeline(dev, mGiVrUberPipe, nullptr); mGiVrUberPipe = VK_NULL_HANDLE; }
    mGiVrNow = false;
    // (THE RT LIGHTING SCRATCH: mGiHit / mPdnSel / mPdnVis are views of it -- pdnDestroy, the only caller, cleared
    //  its own before this; the buffer goes here)
    mGiHit = mPdnSel = mPdnVis = GpuBuffer{};
    if (mRtScratch.buffer) { mCtx->destroyBuffer(mRtScratch); mRtScratch = GpuBuffer{}; }
    mRtScrGiBytes = mRtScrGiKeep = mPdnSelBytes = mPdnVisBytes = mPdnSelOff = mPdnVisOff = 0u;
    mGiHitW = mGiHitH = 0;
    if (mGiDbgBuf.buffer) { mCtx->destroyBuffer(mGiDbgBuf); mGiDbgBuf = GpuBuffer{}; }   // (RenderVKGIDebug 11 / 12)
    mGiDbgW = mGiDbgH = 0;
    mGiSplitNow = false;
    mGiShadeSplitNow = false;
    if (mGiState.buffer) { mCtx->destroyBuffer(mGiState); mGiState = GpuBuffer{}; }
    if (mGiRes.buffer) { mCtx->destroyBuffer(mGiRes); mGiRes = GpuBuffer{}; }
    if (mGiStatRb.buffer) { mCtx->destroyBuffer(mGiStatRb); mGiStatRb = GpuBuffer{}; }
    giLagDestroyRes();   // (#978 phase 2)
    mGiLagPool.destroy();
    for (VkDescriptorSet& s : mGiLagSet) s = VK_NULL_HANDLE;
    for (bool& u : mGiStatUsed) u = false;
    mGiStateSlots = ~0u;
    mGiResW = mGiResH = 0;
    mGiResPrevOk = false;
    if (mGiDn)
    {
        mGiDn->destroy();
        delete mGiDn;
        mGiDn = nullptr;
    }
    mGiNow = mGiWanted = false;
    mGiReq = GiReq{};
}

void LLCleanRendererVK::writeGiMotion()
{
    if (mGiDn)
    {
        // (#1608: the 2D_ARRAY view -- binding 5 is a sampler2DArray, a layer per view)
        if (mMotion.view && mCtx->addArrayView(mMotion, VK_IMAGE_ASPECT_COLOR_BIT))
            mGiDn->setMotion(mMotion.arrayView, mDepthSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        else
            mGiDn->setMotion(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL);
    }
    // (#1038) set 3 binding 28: the same motion vectors for the reservoir reprojection of moving surfaces
    // (the GI trace's GI_F_REUSE_MV, pass 15's RS_F_REUSE_MV; read only while the frame info says they
    // were written: taa_params.y), else a
    // denoiser's 1 x 1 placeholder (GENERAL) -- the binding stays valid for the pipelines declaring it.
    // Same callers / guarantee as writePdnMotion (no frame in flight uses the set), + giInit.
    if (!mDsetDeferSplit || !mCtx) return;
    VkDescriptorImageInfo ii{};
    const LLCleanDenoiserVK* dn = mPdn ? mPdn : mGiDn;
    // (#1608: sampler2DArray -- the motion vectors' 2D_ARRAY view, a layer per view; the placeholder's is one too)
    if (mMotion.view && mCtx->addArrayView(mMotion, VK_IMAGE_ASPECT_COLOR_BIT))
        ii = {mDepthSampler, mMotion.arrayView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    else if (dn && dn->motionPlaceholderView() && dn->nearestSampler())
        ii = {dn->nearestSampler(), dn->motionPlaceholderView(), VK_IMAGE_LAYOUT_GENERAL};
    else return;
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = mDsetDeferSplit; w.dstBinding = kDsBindingGiMotion; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
    vkUpdateDescriptorSets(mCtx->device(), 1, &w, 0, nullptr);
}

// Set 3 bindings 18-23 + 25 (callers guarantee no frame in flight uses the set).
void LLCleanRendererVK::writeGiBindings()
{
    if (!mDsetDeferSplit || !mCtx) return;
    VkDescriptorImageInfo ii[4]{};
    VkDescriptorBufferInfo bi[3]{};
    VkWriteDescriptorSet w[7]{};
    uint32_t n = 0;
    // (the GI denoiser's images; without it -- its init failed -- the projector denoiser's views, which
    //  have the same formats: the combine / BASE declare 18-21 statically and read them only on a GI
    //  frame, which never comes then)
    const LLCleanDenoiserVK* dn = mGiDn ? mGiDn : mPdn;
    if (dn)
    {
        const VkImageView v[4] = {dn->inView(), dn->geomView(), dn->motView(), dn->outView()};
        for (uint32_t i = 0; i < 4; ++i)
        {
            ii[i] = {VK_NULL_HANDLE, v[i], VK_IMAGE_LAYOUT_GENERAL};
            w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = mDsetDeferSplit;
            w[n].dstBinding = kDsBindingGiIn + i; w[n].descriptorCount = 1;
            w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[n].pImageInfo = &ii[i];
            ++n;
        }
    }
    const VkBuffer res = mGiRes.buffer ? mGiRes.buffer : mRsResDummy.buffer;
    const VkBuffer st = mGiState.buffer ? mGiState.buffer : mRsResDummy.buffer;
    if (res && st)
    {
        bi[0] = {res, 0, VK_WHOLE_SIZE};
        bi[1] = {st, 0, VK_WHOLE_SIZE};
        for (uint32_t i = 0; i < 2; ++i)
        {
            w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = mDsetDeferSplit;
            w[n].dstBinding = (i == 0 ? kDsBindingGiRes : kDsBindingGiState); w[n].descriptorCount = 1;
            w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[n].pBufferInfo = &bi[i];
            ++n;
        }
    }
    // (#1031) 25: the split stages' records (only the split pipelines declare it)
    if (const VkBuffer hb = mGiHit.buffer ? mGiHit.buffer : mRsResDummy.buffer)
    {
        // (its range of the RT lighting scratch: offset 0, the kept prefix + the records)
        bi[2] = {hb, 0, mGiHit.buffer ? mRtScrGiBytes : VK_WHOLE_SIZE};
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = mDsetDeferSplit;
        w[n].dstBinding = kDsBindingGiHit; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[n].pBufferInfo = &bi[2];
        ++n;
    }
    if (n) vkUpdateDescriptorSets(mCtx->device(), n, w, 0, nullptr);
    // draw set 0 binding 53: the same state buffer for the forward lit FS (CLEAN_GI_FORWARD) -- or (#978
    // phase 2, one-frame-latency GI) its copy, mGiStateFwd, while one exists for this cache size: the trace
    // then writes mGiState beside pass 2 (giFwdCopy refreshes the copy every GI frame before pass 2).
    mGiFwdBound = false;
    if (mDsetDraw0 && st)
    {
        const bool fwd = mGiStateFwd.buffer && mGiState.buffer && mGiStateFwdSlots == mGiStateSlots;
        VkDescriptorBufferInfo fb{fwd ? mGiStateFwd.buffer : st, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet d{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        d.dstSet = mDsetDraw0; d.dstBinding = 53; d.descriptorCount = 1;
        d.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; d.pBufferInfo = &fb;
        vkUpdateDescriptorSets(mCtx->device(), 1, &d, 0, nullptr);
        mGiFwdBound = fwd;
    }
}

// The light pass, before the frame info and the light-grid decision: does this recording run GI,
// with which resources. Allocation changes wait for the device (set 3 may be bound by frames in
// flight) and rewrite set 3's GI bindings.
void LLCleanRendererVK::giDecide(uint32_t rw, uint32_t rh)
{
    mGiNow = false;
    mGiSplitNow = false;    // (#1031: decided below, on a GI frame)
    mGiShadeSplitNow = false;   // (#1036: ditto)
    mGiLagNow = false;      // (#978 phase 2: decided below, on a GI frame)
    mGiFwdCopied = false;
    mGiLagValue = 0;        // (joined by the last recording, at the latest by its tail: asyncSubmitFrame)
    mGiVrNow = false;       // (#1139: decided by giDecideVr, stereo frames only)
    // (#1608) a stereo frame runs the desktop's GI per eye (RenderVKGI: the split's TRACE / SHADE / REUSE, the
    // reservoirs, the GI denoiser and the lists over every view's texels) -- or, with SlipstreamVRGI, the cheaper
    // world-cache tier (giDecideVr: the uber kernel reads the cache)
    if (stereoActive())
    {
        bool vrTier = false;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<bool> s_vrGi(gSavedSettings, "SlipstreamVRGI", false);
        vrTier = s_vrGi;
#endif
        if (vrTier) { giDecideVr(rw, rh); return; }
        if (mGiVrLogged)
        {
            mGiVrLogged = false;
            mGiVrLogKey = ~0u;
            LL_INFOS("VR") << "VR GI tier off (SlipstreamVRGI 0): the eyes run RenderVKGI per view, as on the desktop"
                           << LL_ENDL;
        }
    }
    GiCfg cfg = giCfg();
    LLCleanInstanceStore::instance().setGiRadianceCollect(cfg.mode != 0u, cfg.rayLen, mShadowEye);
    cfg.scale = rtSignalScale(cfg.scale);   // (RenderVKDlssFullResSignalsMode)
    U32 split = 1;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32>  s_split(gSavedSettings, "RenderVKDeferredSplit", 2U);
    split = s_split;
#endif
    mGiModeSetting = cfg.mode;
    {
        static bool s_vr_off_logged = false;   // (#1076: once per VR on / off edge)
        if (cfg.vrOff != s_vr_off_logged)
        {
            s_vr_off_logged = cfg.vrOff;
            LL_INFOS("VR") << (cfg.vrOff ? "ray-traced GI is off while the VR session runs (RenderVKGI kept)"
                                         : "ray-traced GI follows RenderVKGI again (the VR session stopped)")
                           << LL_ENDL;
        }
    }
    // (Decided in the light pass, BEFORE this frame's TLAS build: tlasValid() is false there by
    // construction -- it wants this frame's slot -- so the decision takes last frame's build;
    // recordGI re-checks tlasValid() and skips the trace without one -- the consumers then read the
    // GI images' last content, or nothing (alpha 0 -> the probes).)
    uint32_t why = 0u;
    if (split == 0u) why |= 1u;
    if (mOffscreen) why |= 2u;
    if (!mDeferSplitOk || !mDsetDeferSplit) why |= 4u;
    if (!mGiDn || !mGiDn->pipelinesReady() || !mGiTracePipe || !mRtReflLayout) why |= 8u;
    if (!mRT) why |= 16u;
    else if (!(mRT->tlasValid() || mRT->tlasBuiltLast())) why |= 32u;
    else if (mRT->consumerSet() == VK_NULL_HANDLE) why |= 64u;
    if (!mRsResDummy.buffer) why |= 128u;
    if (mTexPressure) why |= 256u;
    const bool path = cfg.mode != 0u && why == 0u;
    mGiWanted = path;
    const double now = giClock();
    // the settings line (on change -- of the settings or of the failing conditions)
    {
        const uint32_t key = cfg.mode | (path ? 16u : 0u) | (cfg.cache ? 32u : 0u) | (cfg.hitAmb << 8) | (why << 12);
        if (cfg.mode != 0u && (!mGiLogged || key != mGiLogKey))
        {
            mGiLogged = true;
            mGiLogKey = key;
            static const char* const kMode[4] = {"off", "one bounce (GI-1)", "+ temporal reuse (GI-2)",
                                                 "+ temporal and spatial reuse (GI-3)"};
            static const char* const kAmb[4] = {"nothing (one bounce)", "the probes", "the radiance cache, else the probes",
                                                "the radiance cache, else nothing"};
            LL_INFOS("VKShadow") << "ray-traced GI (RenderVKGI " << cfg.mode << "): " << kMode[cfg.mode]
                                 << "; hit ambient = " << kAmb[cfg.hitAmb] << "; radiance cache "
                                 << (cfg.cache ? "on" : "off") << "; ";
            if (path) LL_CONT << "ACTIVE";
            else
            {
                LL_CONT << "waiting on:";
                if (why & 1u)   LL_CONT << " the deferred lighting is not split (RenderVKDeferredSplit);";
                if (why & 2u)   LL_CONT << " an offscreen (snapshot) recording;";
                if (why & 4u)   LL_CONT << " the split lighting's kernels / set not ready;";
                if (why & 8u)   LL_CONT << " the GI kernels / GI denoiser not ready;";
                if (why & 16u)  LL_CONT << " no ray tracing;";
                if (why & 32u)  LL_CONT << " no TLAS built last frame;";
                if (why & 64u)  LL_CONT << " no RT consumer set;";
                if (why & 128u) LL_CONT << " set 3's placeholder buffer missing;";
                if (why & 256u) LL_CONT << " texture memory pressure;";
            }
            LL_CONT << LL_ENDL;
        }
        else if (cfg.mode == 0u && mGiLogged)
        {
            mGiLogged = false;
            LL_INFOS("VKShadow") << "ray-traced GI off ("
                                 << (cfg.vrOff ? "a VR session runs: GI is off in VR, the setting is kept" : "RenderVKGI 0")
                                 << "): the probes' diffuse irradiance" << LL_ENDL;
        }
    }
    if (!path)
    {
        // off (or under texture pressure): free everything at once; a transient stand-down (no TLAS
        // this frame) keeps the resources for the hold time
        const bool hold = cfg.mode != 0u && !mTexPressure && mGiDn && mGiDn->allocated()
                       && now - mGiLastActive < kRtReflHoldSecs;
        if (!hold && mCtx && (mGiState.buffer || mGiRes.buffer || (mGiDn && mGiDn->allocated())))
        {
            // (freed FOR PRESSURE: the re-admission gate, giPressureAdmit; freed again soon after a
            //  re-admission -- the room was not real -- the dwell doubles, up to 160 s)
            if (mTexPressure && cfg.mode != 0u)
            {
                if (now - mGiPressAdmitAt < 60.0) mGiPressDwell = std::min(mGiPressDwell * 2.0, 160.0);
                else if (now - mGiPressAdmitAt > 300.0) mGiPressDwell = 10.0;   // (five quiet minutes: the base again)
                mGiPressFreedAt = now;
                mGiPressNeed = 0;   // (the gate logs its first check)
            }
            if (resizeAtFrameStart())
            {
                // (freed before the next recording: giPrestep)
                mGiReq = GiReq{};
                mGiReq.pending = true;
                mGiReq.free = true;
            }
            else
                giFreeAll();
        }
        if (cfg.mode == 0u) { mGiPressFreedAt = 0.0; mGiPressNeed = 0; }   // (off: no pressure gate pending)
        mGiResPrevOk = false;
        mGiLagLast.have = false;   // (#978 phase 2: no trace this recording -- offscreen, no TLAS, ...)
        return;
    }
    if (now < mGiRetryAt) return;
    const uint32_t w = giDim(rw, cfg.scale), h = giDim(rh, cfg.scale);
    // the state buffer's radiance cache (GI-4: its slots; 0 without the cache); (GI-2+) the reservoirs
    const uint32_t slots = cfg.cache ? (1u << cfg.slotsLog2) : 0u;
    const bool wantRes = cfg.mode >= 2u;
    // (THE LEAN HISTORY, llcleandenoisevk.h) DLSS RR has taken the GI raw for the dwell: the GI denoiser without its
    // colour history (nothing reads it then -- clean_dn_temporal.comp histOk is off on a raw frame)
    const bool giLeanNow = mGiDn->allocated() && (mGiDn->histMode() & LLCleanDenoiserVK::HIST_NO_COLOUR) != 0u;
    const bool giLean = leanHistDue(rrRawSignal(kRrRawGi, w, h), mGiRawSince, mGiRawOff, giLeanNow, now, leanHistHold());
    mGiHistWant = giLean ? (uint32_t)LLCleanDenoiserVK::HIST_NO_COLOUR : (uint32_t)LLCleanDenoiserVK::HIST_FULL;
    const bool needImg = !mGiDn->allocated() || mGiDn->width() != w || mGiDn->height() != h || !mGiState.buffer
                      || mGiStateSlots != slots;
    // (a history-only change keeps this frame on the current images: lean images on a frame that is not raw run
    //  without history -- clean_dn_common.glsl dnColourHist -- never without GI)
    const bool needHist = !needImg && mGiDn->histMode() != mGiHistWant && now >= mGiHistRetryAt;
    const bool needRes = wantRes ? (!mGiRes.buffer || mGiResW != w || mGiResH != h) : (mGiRes.buffer != VK_NULL_HANDLE);
    // (#1031) the split stages' records: wanted while RenderVKGISplitKernels is on and every stage's
    // pipeline exists (a refusal: the fused kernel, retried after mGiHitRetryAt)
    bool splitPipes = true;
    for (uint32_t i = 0; i < GIS_COUNT; ++i) splitPipes = splitPipes && mGiSplitPipe[i] != VK_NULL_HANDLE;
    const bool wantHit = cfg.split && splitPipes && (mGiHit.buffer || now >= mGiHitRetryAt);
    const bool needHit = wantHit ? (!mGiHit.buffer || mGiHitW != w || mGiHitH != h) : (mGiHit.buffer != VK_NULL_HANDLE);
    if (needImg && !giPressureAdmit(w, h, rw, rh, slots, wantRes, wantHit, now))
    {
        mGiWanted = false;   // (no cache to invalidate: giNoteSpheres collects nothing meanwhile)
        mGiResPrevOk = false;
        mGiLagLast.have = false;
        return;   // (freed under pressure: this frame without GI until its set fits)
    }
    if (needImg || needRes || needHit || needHist)
    {
        // (2026-10-22, the Beta 5 DEVICE_LOST rule) never re-created under this recording: draw set 0
        // is bound already and set 3 / the GI denoiser's set would be rewritten behind a device wait
        // that does not cover this frame's recorded commands. giPrestep applies it; this frame runs
        // without GI while the images / state do not fit (reservoirs alone: without reuse).
        if (resizeAtFrameStart())
        {
            mGiReq = GiReq{};
            mGiReq.pending = true;
            mGiReq.w = w; mGiReq.h = h; mGiReq.slots = slots; mGiReq.wantRes = wantRes; mGiReq.scale = cfg.scale;
            mGiReq.wantHit = wantHit;
            mGiReq.hist = mGiHistWant;
            if (needImg) { mGiResPrevOk = false; return; }
        }
        // (RenderVKResizeAtFrameStart 0: in place -- the history mode only with new images, never alone mid-recording)
        else if (!giApply(w, h, slots, wantRes, cfg.scale, wantHit, needImg ? mGiHistWant : mGiDn->histMode()))
            return;
    }
    // (#978 phase 2) ONE-FRAME-LATENCY GI (RenderVKGILatencyFrame; llcleangivk.cpp GI LATENCY FRAME): its
    // resources at this render extent (requested like the images above: applied before the next recording);
    // this frame runs the mode only when they are all in place (the forward copy bound to b53 above all --
    // the trace must never write the buffer pass 2 reads). Off: freed (b53 back on mGiState).
    {
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<bool> s_lag(gSavedSettings, "RenderVKGILatencyFrame", true);
        mGiLagWant = s_lag;
#endif
        const bool ready = mGiLagWant && giLagReady(rw, rh);
        if (mGiLagWant && !ready && now >= mGiLagRetryAt)
        {
            if (resizeAtFrameStart())
            {
                mGiReq.pending = true;
                mGiReq.lag = true; mGiReq.lagFree = false;
                mGiReq.lagW = rw; mGiReq.lagH = rh;
            }
            else giLagEnsure(rw, rh);
        }
        else if (!mGiLagWant && (mGiStateFwd.buffer || mGiLagDepth.image))
        {
            if (resizeAtFrameStart()) { mGiReq.pending = true; mGiReq.lagFree = true; mGiReq.lag = false; }
            else giLagFree();
        }
        mGiLagNow = mGiLagWant && giLagReady(rw, rh);
    }
    mGiNow = true;
    mGiRw = rw; mGiRh = rh;   // (the mover buffer's extent: giWriteHeader)
    mGiSplitNow = wantHit && giSplitReady(w, h);   // (#1031: else the fused kernel this recording)
    // (#1036) SHADE as MISS / HIT-MAT / HIT-LIGHT over TRACE's lists (same records: no allocation of
    // its own); while its pipelines compile, SHADE
    mGiShadeSplitNow = mGiSplitNow && cfg.shadeSplit && giShadeSplitReady();
    mGiLastActive = now;
    mGiDn->beginFrame();
    ++mGiFrame;
    mGiDebug = cfg.debug;
    mGiAtrous = LLCleanDenoiserVK::clampIters(cfg.atrous);
    // (#870) RAW under DLSS RR (recordGI's dp.raw, the same test): every a-trous iteration only COPIES
    // the temporal pass's output (dnRawView skips the filter) and the last one writes the consumer's
    // depth / fill sign -- one iteration does exactly that (the final image then IN: GI_F_FINAL_IN below).
    if (rrRawSignal(kRrRawGi, mGiDn->width(), mGiDn->height())) mGiAtrous = 1u;
    const bool res = mGiRes.buffer && mGiResW == w && mGiResH == h;
    mGiFlags = GI_F_ON | (LLCleanDenoiserVK::finalIsIn(mGiAtrous) ? GI_F_FINAL_IN : 0u)
             | (res ? GI_F_RES : 0u) | (res && mGiResPrevOk ? GI_F_RES_PREV : 0u)
             | (res && cfg.mode >= 2u ? GI_F_TEMPORAL : 0u) | (res && cfg.mode >= 3u && cfg.taps > 0u ? GI_F_SPATIAL : 0u)
             | (slots != 0u ? GI_F_CACHE | GI_F_CACHE_FILL : 0u);
    mGiResPrevOk = res;   // (this frame's trace writes them: next frame's previous)
    // (#978 phase 2) the combine reads LAST frame's trace: usable only when it ran the frame before this
    // one (consecutive GI frames: no recording without a trace in between) into these very images, at this
    // render extent (the reprojection maps render pixels); its final image (IN / OUT) and GEOM parity go
    // into this header for the consumers (the trace itself takes its own from the push / the denoiser).
    if (mGiLagNow)
    {
        mGiLagStale = !(mGiLagLast.have && mGiLagLast.frame + 1u == mGiFrame && mGiLagLast.w == mGiDn->width()
                        && mGiLagLast.h == mGiDn->height() && mGiLagLast.rw == rw && mGiLagLast.rh == rh);
        mGiLagHdrFinalIn = mGiLagLast.finalIn;
        mGiLagHdrPar = mGiLagLast.par;
    }
}

// The GI resources freed (the device waits): the denoiser's images, the state buffer, the
// reservoirs; set 3 bindings 18-23 back on the fallbacks. giPrestep (or in place with
// RenderVKResizeAtFrameStart 0).
void LLCleanRendererVK::giFreeAll()
{
    if (!mCtx) return;
    vkDeviceWaitIdle(mCtx->device());
    if (mGiDn && mGiDn->allocated()) mGiDn->release();
    if (mGiState.buffer) { mCtx->destroyBuffer(mGiState); mGiState = GpuBuffer{}; }
    if (mGiRes.buffer) { mCtx->destroyBuffer(mGiRes); mGiRes = GpuBuffer{}; }
    // (#1031; THE RT LIGHTING SCRATCH: the GI's records leave it -- the split shadow core's share stays)
    if (mRtScrGiBytes) rtScratchLayout(0u, 0u, mPdnSelBytes, mPdnVisBytes);
    mGiHitW = mGiHitH = 0;
    if (mGiDbgBuf.buffer) { mCtx->destroyBuffer(mGiDbgBuf); mGiDbgBuf = GpuBuffer{}; }   // (RenderVKGIDebug 11 / 12)
    mGiDbgW = mGiDbgH = 0;
    if (mGiMov.buffer) { mCtx->destroyBuffer(mGiMov); mGiMov = GpuBuffer{}; }   // (RenderVKGIMoverSamples)
    mGiMovW = mGiMovH = mGiMovCap = 0;
    mGiMovNow = mGiMovLastHave = false;
    mGiSplitNow = false;
    mGiShadeSplitNow = false;
    giLagDestroyRes();   // (#978 phase 2: the forward copy, the depth copy)
    mGiStateSlots = ~0u;
    mGiResW = mGiResH = 0;
    mGiResPrevOk = false;
    writeGiBindings();
}

// PRESSURE RE-ADMISSION (the header's mGiPressFreedAt): true when GI may (re-)allocate its set now. Only a set
// freed under texture pressure is gated: it comes back once the dwell has passed AND everything it brings
// back -- the denoiser's images, the state + cache, the reservoirs, the split records, the one-frame-latency
// copies and the vis queue (freed with its only consumer; counted at its cap) -- lands under the grow line
// (VRAM LINES, llvrambudgetvk.h: what grows into free VRAM stops there, one hysteresis step under the soft
// line, so it cannot re-enter pressure by itself). The ladder is untouched: GI is not one of its rungs.
bool LLCleanRendererVK::giPressureAdmit(uint32_t w, uint32_t h, uint32_t rw, uint32_t rh, uint32_t slots, bool wantRes,
                                        bool wantHit, double now)
{
    if (mGiPressFreedAt <= 0.0 || !mGiDn) return true;
    // (#1608: every view's texels -- the per-texel buffers hold the extent's rows x N; the denoiser counts its views)
    const uint64_t px = (uint64_t)w * h * mViewCount;
    uint64_t need = mGiDn->bytesAt(w, h, false, false, mGiHistWant) + sizeof(GiHeader) + giCacheBytes(slots);
    if (wantRes) need += px * 64u;
    if (wantHit)
    {
        // (THE RT LIGHTING SCRATCH: what the records add to the buffer they share with the split shadow core)
        const VkDeviceSize cur = mRtScratch.buffer ? mRtScratch.size : 0u;
        const VkDeviceSize tot = rtScratchPlace(giHitBytes(w, h * mViewCount), giHitKeepBytes(w, h * mViewCount),
                                                mPdnSelBytes, mPdnVisBytes);
        need += tot > cur ? tot - cur : 0u;
    }
    if (mGiLagWant) need += (uint64_t)rw * rh * mViewCount * 4u + sizeof(GiHeader) + giCacheBytes(slots);
    if (!mVq.buffer) need += mVqCapBytes;
    const uint64_t usage = (uint64_t)std::max(mTexVramUsage, 0.0);
    const uint64_t line = mBudget.lines().grow;
    const bool dwelt = now - mGiPressFreedAt >= mGiPressDwell;
    const bool fits = line == 0u || usage + need <= line;   // (no budget measured yet: nothing to gate on)
    if (dwelt && fits)
    {
        LL_INFOS("VKMem") << "ray-traced GI: re-admitted after a VRAM pressure free (" << (int)(now - mGiPressFreedAt)
                          << " s): needs " << (need >> 20) << " MB, usage " << (usage >> 20) << " MB, grow line "
                          << (line >> 20) << " MB" << LL_ENDL;
        mGiPressFreedAt = 0.0;
        mGiPressAdmitAt = now;
        mGiPressNeed = 0;
        return true;
    }
    if (mGiPressNeed == 0u)
        LL_INFOS("VKMem") << "ray-traced GI: freed under VRAM pressure; back when its " << (need >> 20)
                          << " MB fits under the grow line (usage " << (usage >> 20) << " MB, line " << (line >> 20)
                          << " MB) and " << (int)mGiPressDwell << " s have passed" << LL_ENDL;
    mGiPressNeed = std::max<uint64_t>(need, 1u);
    return false;
}

// The GI resources at this extent / cache size (each change waits for the device; set 3's GI
// bindings rewritten). False: the denoiser's images or the state buffer were refused (#697: GI
// off, retry in 30 s); refused reservoirs only drop the reuse.
bool LLCleanRendererVK::giApply(uint32_t w, uint32_t h, uint32_t slots, bool wantRes, float scale, bool wantHit,
                                uint32_t hist)
{
    if (!mCtx || !mGiDn) return false;
    const double now = giClock();
    bool changed = false;
    // the GI denoiser's images
    if (!mGiDn->allocated() || mGiDn->width() != w || mGiDn->height() != h)
    {
        if (!mGiDn->ensure(w, h, false, false, hist))
        {
            mGiRetryAt = now + 30.0;
            LL_WARNS("Vulkan") << "ray-traced GI: the GI denoiser's " << w << "x" << h << " images were refused (VRAM);"
                                  " GI off, retry in 30 s" << LL_ENDL;
            writeGiBindings();
            return false;
        }
        changed = true;
    }
    else if (mGiDn->histMode() != hist)
    {
        // (THE LEAN HISTORY: the colour history alone -- the final image, GEOM and MOT stay, so set 3 and the
        //  one-frame-latency GI's last frame are untouched: not a change here. A refused growth runs without it.)
        if (!mGiDn->ensure(w, h, false, false, hist)) mGiHistRetryAt = now + 30.0;
    }
    // the state buffer: the header + the radiance cache (GI-4: its slots; 0 without the cache)
    if (!mGiState.buffer || mGiStateSlots != slots)
    {
        vkDeviceWaitIdle(mCtx->device());
        if (mGiState.buffer) { mCtx->destroyBuffer(mGiState); mGiState = GpuBuffer{}; }
        const VkDeviceSize bytes = sizeof(GiHeader) + giCacheBytes(slots) + 16u;
        {
            VkMemScope memcat(VKMEM_DENOISE);
            VkDeviceContext::SoftAllocScope soft(mCtx);
            mGiState = mCtx->createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                                                 | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        }
        mGiStateSlots = mGiState.buffer ? slots : ~0u;
        changed = true;
        if (!mGiState.buffer)
        {
            mGiRetryAt = now + 30.0;
            LL_WARNS("Vulkan") << "ray-traced GI: the state buffer (" << ((bytes + (1u << 19)) >> 20)
                               << " MB) was refused (VRAM); GI off, retry in 30 s" << LL_ENDL;
            writeGiBindings();
            return false;
        }
        mCtx->setObjectName(VK_OBJECT_TYPE_BUFFER, (uint64_t)mGiState.buffer, "giState");
        // The new cache starts empty, but old reservoirs/denoiser images can survive
        // this allocation. Preserve pending scene invalidation until their next trace.
        mGiStateFresh = true;   // (the cache starts empty: giWriteHeader clears it)
    }
    // (GI-2+) the reservoirs at the GI extent
    if (!wantRes && mGiRes.buffer)
    {
        vkDeviceWaitIdle(mCtx->device());
        mCtx->destroyBuffer(mGiRes);
        mGiRes = GpuBuffer{};
        mGiResW = mGiResH = 0;
        changed = true;
    }
    if (wantRes && (!mGiRes.buffer || mGiResW != w || mGiResH != h))
    {
        vkDeviceWaitIdle(mCtx->device());
        if (mGiRes.buffer) { mCtx->destroyBuffer(mGiRes); mGiRes = GpuBuffer{}; }
        const VkDeviceSize bytes = (VkDeviceSize)w * h * mViewCount * 2u * 32u;   // (#1608: every view's texels)
        {
            VkMemScope memcat(VKMEM_DENOISE);
            VkDeviceContext::SoftAllocScope soft(mCtx);
            mGiRes = mCtx->createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        }
        mGiResW = mGiRes.buffer ? w : 0u;
        mGiResH = mGiRes.buffer ? h : 0u;
        mGiResPrevOk = false;
        changed = true;
        if (!mGiRes.buffer)
            LL_WARNS("Vulkan") << "ray-traced GI: the reservoirs (" << ((bytes + (1u << 19)) >> 20)
                               << " MB) were refused (VRAM); no reuse (RenderVKGI acts as 1)" << LL_ENDL;
        else mCtx->setObjectName(VK_OBJECT_TYPE_BUFFER, (uint64_t)mGiRes.buffer, "giReservoirs");
    }
    // (#1031) the split stages' records at the GI extent (the re-shade list + 96 B per GI texel)
    // (THE RT LIGHTING SCRATCH, llcleanprojstochvk.cpp rtScratchLayout: the records share one buffer with the split
    //  shadow core's descriptors / visibilities -- the layout waits for the device and rewrites b25-27 itself)
    if (!wantHit && mGiHit.buffer)
    {
        rtScratchLayout(0u, 0u, mPdnSelBytes, mPdnVisBytes);
        mGiHitW = mGiHitH = 0;
        changed = true;
    }
    if (wantHit && (!mGiHit.buffer || mGiHitW != w || mGiHitH != h))
    {
        // (#1608: every view's texels -- the records, lists and tags of an extent N x as tall: clean_deferred_light.comp
        //  giTexels)
        const VkDeviceSize bytes = giHitBytes(w, h * mViewCount);
        const bool got = rtScratchLayout(bytes, giHitKeepBytes(w, h * mViewCount), mPdnSelBytes, mPdnVisBytes) && mGiHit.buffer;
        mGiHitW = got ? w : 0u;
        mGiHitH = got ? h : 0u;
        changed = true;
        if (!got)
        {
            mGiHitRetryAt = now + 30.0;
            LL_WARNS("Vulkan") << "ray-traced GI: the split stages' records (" << ((bytes + (1u << 19)) >> 20)
                               << " MB) were refused (VRAM); the fused GI kernel instead, retry in 30 s" << LL_ENDL;
        }
    }
    if (changed)
    {
        vkDeviceWaitIdle(mCtx->device());
        mGiLagLast.have = false;   // (#978 phase 2: new images / state -- no last frame's GI to reproject)
        writeGiBindings();
        LL_INFOS("Vulkan") << "ray-traced GI: " << w << "x" << h
                           << (mViewCount > 1u ? " x " + std::to_string(mViewCount) + " views" : std::string())
                           << " (RenderVKGIScale " << scale << "), denoiser "
                           << ((mGiDn->bytes() + (1u << 19)) >> 20) << " MB, reservoirs "
                           << ((mGiRes.buffer ? (uint64_t)w * h * mViewCount * 64u : 0u) >> 20) << " MB, split-stage records "
                           << ((mGiHit.buffer ? (uint64_t)giHitBytes(w, h * mViewCount) : 0u) >> 20) << " MB, state + cache "
                           << ((sizeof(GiHeader) + giCacheBytes(slots)) >> 20) << " MB (" << slots << " cache slots)"
                           << LL_ENDL;
    }
    return true;
}

// syncFromStoresPrestep (before the frame's command buffer begins): what the last recording's
// giDecide asked for.
void LLCleanRendererVK::giPrestep()
{
    if (!mGiReq.pending) return;
    const GiReq r = mGiReq;
    mGiReq = GiReq{};
    if (!mCtx || !mGiDn) return;
    if (r.free) { giFreeAll(); return; }
    if (r.w) giApply(r.w, r.h, r.slots, r.wantRes, r.scale, r.wantHit, r.hist);
    // (#978 phase 2: after the state buffer -- the forward copy takes its cache size)
    if (r.lag) giLagEnsure(r.lagW, r.lagH);
    else if (r.lagFree) giLagFree();
}

// The light pass, after the WAR barrier on last frame's reads, with the other uploads (the barrier
// after them makes it visible to every compute stage).
void LLCleanRendererVK::giWriteHeader(VkCommandBuffer cmd)
{
    mGiHdrChg = false;
    mGiCacheSpecRough = 0.f;   // (gi-glass: set below while the cache specular runs)
    if (!mGiNow || !mGiState.buffer) return;
    const GiCfg cfg = mGiVrNow ? giVrCfg() : giCfg();   // (#1139: a VR GI frame's preset)
    GiHeader h{};
    h.cfg[0] = mGiFlags | (cfg.specOcc << 8) | (cfg.everyLog << 10)   // (clean_gi_common.glsl GI_SPECOCC_SHIFT /
             | (cfg.missProbes ? 4096u : 0u) | (cfg.cullBack ? 8192u : 0u)       //  GI_EVERY_SHIFT / GI_F_MISS_PROBES / _CULL_BACK
             | (cfg.celOcc ? 16384u : 0u) | (cfg.specOccRtRes ? 32768u : 0u)     //  / GI_F_CEL_OCC / GI_F_SPECOCC_RT_RES
             | (cfg.hazeOcc ? 4194304u : 0u)                                     //  / GI_F_HAZE_OCC
             | (cfg.fwdFilter << 26);                                            //  / GI_FWD_FILTER_SHIFT)
    h.cfg[1] = mGiFrame;
    h.cfg[2] = mGiStateSlots == ~0u ? 0u : mGiStateSlots;
    h.cfg[3] = mGiDebug;
    h.prm[0] = cfg.intensity;
    h.prm[1] = cfg.minAmb;
    h.prm[2] = cfg.rayLen;
    h.prm[3] = (float)cfg.hitAmb;
    h.prm2[0] = (float)cfg.mcap;
    h.prm2[1] = (float)cfg.taps;
    h.prm2[2] = (float)cfg.radius;
    h.prm2[3] = cfg.firefly;
    h.prm3[0] = cfg.cell;
    h.prm3[1] = cfg.levelDist;
    h.prm3[2] = cfg.window;
    h.prm3[3] = cfg.evict;
    h.prm4[0] = cfg.cosMin;
    h.prm4[1] = cfg.reshadeGain;
    // (#87) DISOCCLUSION (camera zoom / pan: silhouettes lose their history): the reservoirs' extra
    // small-radius taps where no temporal candidate survived, the reprojection search, the edge-safe
    // upsample (its GEOM parity: the one this frame's trace writes -- beginFrame flipped it above)
    h.prm4[2] = (float)cfg.disoccTaps;
    h.prm4[3] = mGiDn ? (float)mGiDn->parity() : 0.f;
    // (the counters only while recordGI copies them out: the GPU timers -- else ~13 same-line atomics
    //  per subgroup of the trace for nothing)
    if (mTsOn) h.cfg[0] |= GI_F_STATS;
    if (cfg.reprojSearch) h.cfg[0] |= GI_F_REPROJ_SEARCH;
    // (#985, A/B) a temporal pick whose ray to its sample crosses no change sphere takes no visibility ray
    if (cfg.visTSkip) h.cfg[0] |= GI_F_VIS_TSKIP;
    if (cfg.upsNormal) h.cfg[0] |= GI_F_UPS_NORMAL;
    // (#1038) DISOCCLUSION ON MOVING GEOMETRY: the reservoirs' temporal candidate follows the motion
    // vectors (a walking avatar's surface, not the background behind where it stood -- set 3 binding
    // 28; only while the frame info says they were written), and a texel left with its fresh sample
    // alone takes the radiance cache's cell as a prior of up to RenderVKGIDisoccCache samples (prm5.w)
    if (cfg.reuseMotion && mMotion.view) h.cfg[0] |= GI_F_REUSE_MV;
    h.prm5[3] = (float)cfg.disoccCache;
    // REDUCED-EXTENT GI, A STABLE SAMPLE LATTICE (clean_gi_common.glsl GI_F_STABLE_SAMPLES): below full extent GI
    // texel m samples the fixed render pixel floor((m + 0.5) r), not a jitter-chosen one -- a receiver a few texels
    // wide (a distant avatar) was sampled at another point every frame and swam. The GI denoiser takes the same
    // lattice (recordGITail: mGiStableNow), the consumers read the phase from this word.
    mGiStableNow = cfg.stable && !mGiVrNow;
    if (mGiStableNow) h.cfg[0] |= GI_F_STABLE_SAMPLES;
    // FULL-RESOLUTION GI ON MOVER PIXELS (RenderVKGIMoverSamples, an A/B; clean_gi_movers.glsl, recordGIMovers): below
    // full GI extent, pixels whose receiver moves (skinned / animesh, or a rigid surface moving against the camera's
    // reprojection) get their own GI sample and history; static surfaces stay at the reduced extent. The buffer (16 B
    // per render pixel + 4 B per list entry, VKMEM_DENOISE) lives while the setting is on; off, a new extent or a new
    // cap: retired (frames in flight may still read it). Refused (#697): off, retried after 30 s.
    {
        const uint32_t dw = mGiDn ? mGiDn->width() : 0u, dh = mGiDn ? mGiDn->height() : 0u;
        // (#1608: one view's per-pixel history and list -- the desktop only; a stereo frame keeps the reduced extent on
        //  movers, named in the stereo-off line)
        const bool want = cfg.movers && !mGiVrNow && !stereoActive() && mMotion.view && mGiMoverPipe && mGiRw != 0u && mGiRh != 0u
                       && dw < mGiRw && dw != 0u && mGiRw <= 0xFFFFu && mGiRh <= 0xFFFFu;
        mGiMovNow = false;
        if (want)
        {
            // (<= 65535 x 64 workgroups, and the history base fits mov.w's 21 bits)
            const uint32_t capMax = std::max<uint32_t>(std::min<uint32_t>(mGiRw * mGiRh, 4000000u), 1024u);
            const uint32_t cap = std::clamp<uint32_t>((uint32_t)(cfg.moverBudget * (F32)dw * (F32)dh), 1024u, capMax);
            if (!mGiMov.buffer || mGiMovW != mGiRw || mGiMovH != mGiRh || mGiMovCap != cap)
            {
                if (mGiMov.buffer) retireBuffer(mGiMov);
                mGiMov = GpuBuffer{};
                mGiMovW = mGiMovH = mGiMovCap = 0;
                mGiMovLastHave = false;
                if (giClock() >= mGiMovRetryAt)
                {
                    // (uvec2 units: 4 header words' worth, the list, 2 parities x the render extent)
                    const VkDeviceSize bytes = ((VkDeviceSize)(4u + (cap + 1u) / 2u) + 2ull * mGiRw * mGiRh) * 8u;
                    GpuBuffer nb{};
                    {
                        VkMemScope memcat(VKMEM_DENOISE);
                        VkDeviceContext::SoftAllocScope soft(mCtx);
                        nb = mCtx->createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
                                                       | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
                    }
                    if (nb.buffer && nb.address)
                    {
                        mGiMov = nb;
                        mGiMovW = mGiRw; mGiMovH = mGiRh; mGiMovCap = cap;
                        mGiMovFill = true;
                        mCtx->setObjectName(VK_OBJECT_TYPE_BUFFER, (uint64_t)mGiMov.buffer, "giMoverSamples");
                        LL_INFOS("Vulkan") << "GI mover samples (RenderVKGIMoverSamples): " << mGiRw << "x" << mGiRh
                                           << " history, list cap " << cap << " px (RenderVKGIMoverBudget "
                                           << cfg.moverBudget << " x the " << dw << "x" << dh << " GI texels), "
                                           << ((bytes + (1u << 19)) >> 20) << " MB" << LL_ENDL;
                    }
                    else
                    {
                        if (nb.buffer) mCtx->destroyBuffer(nb);
                        mGiMovRetryAt = giClock() + 30.0;
                        LL_WARNS("Vulkan") << "GI mover samples: the buffer (" << ((bytes + (1u << 19)) >> 20)
                                           << " MB) was refused (VRAM); the reduced-extent GI on movers, retry in 30 s" << LL_ENDL;
                    }
                }
            }
            mGiMovNow = mGiMov.buffer != VK_NULL_HANDLE;
        }
        else if (mGiMov.buffer)
        {
            retireBuffer(mGiMov);
            mGiMov = GpuBuffer{};
            mGiMovW = mGiMovH = mGiMovCap = 0;
            mGiMovLastHave = false;
            LL_INFOS("Vulkan") << "GI mover samples off: the buffer released" << LL_ENDL;
        }
        // what the consumers read: this frame's pass, or under GI_F_LAG the last one when it ran last frame
        bool rd = false;
        uint32_t st = 0u;
        if (mGiMov.buffer)
        {
            if (mGiLagNow) { rd = mGiMovLastHave && mGiMovLastFrame + 1u == mGiFrame && !mGiLagStale; st = mGiMovLastFrame; }
            else { rd = mGiMovNow && !mGiMovFill; st = mGiFrame; }   // (a new buffer: read from its second pass on)
        }
        if (rd)
        {
            h.mov[0] = (uint32_t)(mGiMov.address & 0xFFFFFFFFull);
            h.mov[1] = (uint32_t)(mGiMov.address >> 32);
            h.mov[2] = mGiMovW | (mGiMovH << 16);
            h.mov[3] = (4u + (mGiMovCap + 1u) / 2u) | (((st & 0xFFFFFu) & 2047u) << 21);   // (clean_gi_movers.glsl giMovHistBase)
        }
    }
    // (GI_F_BACK_STOP / GI_F_POINT_SHADOW: the edge sparkles' leaks -- clean_gi_common.glsl)
    if (cfg.backStop) h.cfg[0] |= GI_F_BACK_STOP;
    if (cfg.pointShadow) h.cfg[0] |= GI_F_POINT_SHADOW;
    // (#978 phase 2, GI_F_LAG) the consumers read LAST frame's trace: its final image and GEOM parity
    if (mGiLagNow)
    {
        h.cfg[0] = (h.cfg[0] & ~GI_F_FINAL_IN) | (mGiLagHdrFinalIn ? GI_F_FINAL_IN : 0u) | GI_F_LAG
                 | (mGiLagStale ? GI_F_LAG_STALE : 0u);
        // + 2: RenderVKGILagRevealSearch (an A/B, not saved; clean_deferred_light.comp deferGiFetchLag): a pixel
        // the camera's motion uncovered behind an occluder looks along its own plane for last frame's GI beside
        // the occluder, then the cache cells beside its own, before it falls to the probes' / sky ambient
        bool reveal = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<bool> s_reveal(gSavedSettings, "RenderVKGILagRevealSearch", true);
        reveal = s_reveal;
#endif
        h.prm4[3] = (float)((mGiLagHdrPar & 1u) | (reveal ? 2u : 0u));
    }
    // CHANGED LIGHTS (GI_F_LIGHT_CHG, RenderVKGILightChangeCut): the lights that MOVED (centre / apex /
    // axis: max(RenderVKReSTIRMoveEps, RenderVKGILightMoveRel x radius)) or whose RADIANCE changed
    // (colour / intensity / radius / falloff beyond RenderVKReSTIRFlickerEps) against their GI
    // reference -- mGiChg (#980; DI's per-frame 1 mm masks flagged every worn light every frame) --
    // plus their OLD records (chg_prev). A picked reused sample is re-shaded (clean_deferred_light.comp)
    // only where those lights' unshadowed light, old or new, is at least RenderVKGIReshadeThreshold of
    // its stored radiance (#980: the significance test; 0 = wherever one reaches). The radiance cache
    // is NOT restarted for them (a club's cycling lights restarted the cells under them every frame: the
    // hits' ambient fell back to the probes -- more noise, a different colour); its exponential mean
    // follows the new colour.
    h.prm5[0] = cfg.reshadeThr;
    // (#1041) the own-fixture rule's box limit for the hits' budgeted point-light rays (pass 15's
    // RenderVKReSTIRFixtureSize: a bulb's own shade / lantern / sconce never shadows its light)
    {
        F32 fixture = 1.5f;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<F32> s_fix(gSavedSettings, "RenderVKReSTIRFixtureSize", 1.5f);
        fixture = s_fix;
#endif
        h.prm5[2] = std::clamp(fixture, 0.f, 16.f);
    }
    if (cfg.lightChg)
    {
        h.cfg[0] |= GI_F_LIGHT_CHG;
        uint32_t n = 0;
        for (int i = 0; i < 8; ++i)
        {
            h.chg[i] = mGiChg[i];
            for (uint32_t b = h.chg[i]; b; b &= b - 1u) ++n;
        }
        h.prm5[1] = (float)mGiChgPrevN;
        std::memcpy(h.chg_prev, mGiChgPrev, sizeof(float) * 8u * mGiChgPrevN);
        // (#1087) THE HOT LIST: the lights flagged as moved within
        // RenderVKGILightChangeHold frames (mGiHot) -- their reused samples' confidence clamped (RenderVKGILightMoveMCap), the changed
        // share held through the whole motion, the radiance cache's window short inside their old / new
        // spheres (RenderVKGILightChangeCacheFrames). Empty where nothing changed: no cost.
        {
            const size_t nh = std::min(mGiHot.size(), kGiHotMax);
            for (size_t k = 0; k < nh; ++k)
            {
                const GiHotLight& e = mGiHot[k];
                std::memcpy(h.hot[4u * k], e.opr, sizeof(e.opr));
                std::memcpy(h.hot[4u * k + 1u], e.ocf, sizeof(e.ocf));
                std::memcpy(h.hot[4u * k + 2u], e.npr, sizeof(e.npr));
                h.hot[4u * k + 3u][0] = (float)e.li;
            }
            h.prm6[0] = (float)nh;
            h.prm6[1] = (float)cfg.moveMCap;
            h.prm6[2] = cfg.moveCache;
            mGiStatAcc.hotLights += (double)nh;
        }
        mGiStatAcc.chgLights += n;
        mGiStatAcc.allLights += (double)std::min<size_t>(mLightsCPU.size(), 256u);
        ++mGiStatAcc.hdrFrames;
    }
    // Independent emitter deltas: old+new CPU row snapshots at the actual dirty upload,
    // including dynamic rows, tint/UV and release. (#1406) The store sizes each sphere by what
    // the change can light (LLCleanInstanceStore::noteGiRadianceDeltaLocked: the emitter's
    // radiance and area, its tint difference or how far it moved), two legs of that reach (the
    // direct bounce + one cache-fed hit), each at most RenderVKGIRayLength; arbitrary recursive
    // transport is not bounded by this sphere. Read-side rejection happens before shading (not
    // only in resolve).
    LLCleanInstanceStore::GiRadianceSpheres radiance{};
    bool radianceOverflow = false;
    LLCleanInstanceStore::GiRadianceStats deltaStats{};
    const U32 nr = LLCleanInstanceStore::instance().takeGiRadianceDirty(radiance, radianceOverflow, deltaStats);
    if (nr != 0u || radianceOverflow) ++mGiSceneSerial;
    if (nr != 0u) LLVKHitch::count("giRadianceSpheres", nr);
    if (radianceOverflow)
    {
        ++mGiGlobalSerial;
        LLVKHitch::count("giRadianceGlobal", 1u);
    }
    s_giPairBudget = 16u;
    // (#1406) SOFT: an emitter's change makes the history inside its sphere adapt fast (the reservoirs'
    // confidence capped, the cache's short mean, fresh rays) instead of restarting it -- a negative
    // radius (clean_gi_common.glsl giSphereHit). Geometry / reset spheres (giNoteSpheres) stay hard.
    // How much it shortens: by the change's strength at each sample (giSoftCap), or (A/B,
    // RenderVKGISoftSphereFlat, not saved) a638ea06f6's flat cap anywhere in the reach.
    {
        constexpr uint32_t GI_F_SPH_FLAT = 1073741824u;   // (clean_gi_common.glsl)
        bool flat = false;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<bool> s_flat(gSavedSettings, "RenderVKGISoftSphereFlat", false);
        flat = s_flat;
#endif
        if (flat) h.cfg[0] |= GI_F_SPH_FLAT;
        // (RenderVKGISpatialPlaneTaps, an A/B, not saved) 0: the spatial taps' old linear depth, no retry
        constexpr uint32_t GI_F_SP_LINEAR = 536870912u;   // (clean_gi_common.glsl)
        bool planeTaps = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<bool> s_plane(gSavedSettings, "RenderVKGISpatialPlaneTaps", true);
        planeTaps = s_plane;
#endif
        if (!planeTaps) h.cfg[0] |= GI_F_SP_LINEAR;
        // (gi-glass) GI THROUGH GLASS and THE CACHE SPECULAR (clean_gi_common.glsl xcfg; A/B settings, not saved):
        // RenderVKGITransmit 0 off (GI rays pass alpha-blend surfaces unseen, as before), 1 composited with the grey
        // (1 - alpha), 2 tinted by the layer colour (giTransmit). RenderVKGICacheSpec 1: receivers rougher than
        // RenderVKGICacheSpecRough take their specular from the radiance cache's directional cells (deferReflIn,
        // deferCacheSpec) and the RT reflections stop tracing them (rtReflDecide: the roughness threshold capped at the
        // cutoff); the cache's direction words are filled only while it is on.
        constexpr uint32_t GIX_TRANSMIT = 1u, GIX_TINT = 2u, GIX_CACHE_DIR = 4u, GIX_CACHE_SPEC = 8u;
        U32 transmit = 0u, cacheSpec = 0u, specDbg = 0u;
        F32 specRough = 0.3f, specGain = 1.f;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<U32> s_transmit(gSavedSettings, "RenderVKGITransmit", 0U);
        static LLCachedControl<U32> s_cspec(gSavedSettings, "RenderVKGICacheSpec", 0U);
        static LLCachedControl<F32> s_crough(gSavedSettings, "RenderVKGICacheSpecRough", 0.3f);
        static LLCachedControl<F32> s_cgain(gSavedSettings, "RenderVKGICacheSpecGain", 1.f);
        static LLCachedControl<U32> s_cdbg(gSavedSettings, "RenderVKGICacheSpecDebug", 0U);
        transmit = std::min<U32>(s_transmit, 2u);
        cacheSpec = std::min<U32>(s_cspec, 1u);
        specDbg = std::min<U32>(s_cdbg, 2u);
        const F32 cr = s_crough, cg = s_cgain;
        specRough = std::isfinite(cr) ? llclamp(cr, 0.05f, 1.f) : 0.3f;
        specGain = std::isfinite(cg) ? llclamp(cg, 0.f, 4.f) : 1.f;
#endif
        const bool cacheOn = h.cfg[2] != 0u && (h.cfg[0] & GI_F_CACHE) != 0u;
        const bool specOn = cacheSpec != 0u && cacheOn && !mGiVrNow;   // (VR GI's tier: no combine reader; unchanged)
        h.xcfg[0] = (transmit != 0u ? GIX_TRANSMIT : 0u) | (transmit == 2u ? GIX_TINT : 0u)
                  | (specOn ? GIX_CACHE_DIR | GIX_CACHE_SPEC : 0u);
        std::memcpy(&h.xcfg[1], &specRough, 4);
        std::memcpy(&h.xcfg[2], &specGain, 4);
        h.xcfg[3] = specOn ? specDbg : 0u;
        // the RT reflections read this next frame (rtReflDecide): one frame late either way (a frame of probe
        // fallback on the receivers between the two cutoffs when it switches on, of no cache specular when off)
        mGiCacheSpecRough = specOn ? specRough : 0.f;
        // prm6.w: a white fullbright's radiance in this frame's units (the sky HDR scale, clean_atmos_common
        // atmosHdrScale) -- the unit of the host's emitter threshold (giSoftCap)
        const float hdr = mAtmos[6 * 4 + 1];
        h.prm6[3] = std::isfinite(hdr) && hdr > 0.f ? hdr : 1.f;
    }
    for (U32 i = 0; i < nr; ++i)   // (the store's spheres are soft already)
        if (giAddSphere(mGiSpheres, mGiSphereN, radiance[i], mShadowEye)) ++mGiSphDropped;
    mGiSphOverflow |= radianceOverflow;
    // Fixed counters distinguish skipped incomplete deltas from real global resets.
    // No object/material-to-scene walk and no GPU readback on the deadline path.
    // (#1438) how often the GI headers carry change spheres (the cache cells inside them take a short
    // mean: GI_SPHERE_CACHE_FRAMES): headers with any / all headers since the last line.
    // (#1406) the emitter deltas: the store's cumulative counts by class (faint = no sphere); since the
    // last line the spheres queued (and the frames with any), the largest with its class (merged: the
    // queue's merging grew it), its emitter's bounding radius and its reach (one leg); and the largest
    // sphere any header carried (emitter or geometry, after the header list's own merging).
    static uint32_t s_sphHdrs = 0, s_allHdrs = 0, s_emFrames = 0;
    static uint64_t s_emSpheres = 0, s_headerHard = 0, s_headerSoft = 0;
    static float s_emLargest = 0.f, s_emLargestEmitter = 0.f, s_emLargestReach = 0.f;
    static uint32_t s_emLargestClass = 0;
    static LLUUID s_emLargestObject;
    static float s_hdrLargest = 0.f, s_hdrLargestSoft = 0.f;
    s_emSpheres += nr;
    if (nr != 0u) ++s_emFrames;
    if (deltaStats.largest > s_emLargest)
    {
        s_emLargest = deltaStats.largest;
        s_emLargestClass = deltaStats.largestClass;
        s_emLargestEmitter = deltaStats.largestEmitter;
        s_emLargestReach = deltaStats.largestReach;
        s_emLargestObject = deltaStats.largestObject;
    }
    const double deltaNow = giClock();
    if (deltaNow >= mGiDeltaLogAt)
    {
        static const char* const kCls[] = {"moved", "emission", "texture", "appear", "release", "merged"};
        mGiDeltaLogAt = deltaNow + 60.0;
        // the largest's object as its local id (Build > Select can find it; no names): 0 = a merge / gone
        U32 localId = 0;
#if defined(LL_HAS_CLEAN_PIPELINE)
        if (s_emLargestObject.notNull())
            if (LLViewerObject* o = gObjectList.findObject(s_emLargestObject)) localId = o->getLocalID();
#endif
        LL_INFOS("VKShadow") << "GI delta totals: skipped unknown_pose=" << deltaStats.unknownPose
            << " bad_bounds=" << deltaStats.badBounds << " nonfinite_bounds=" << deltaStats.nonfiniteBounds
            << " mirror_resize=" << deltaStats.mirrorReset << " material_residency=" << mGiMaterialSkipped
            << " geometry_overflow=" << mGiGeometrySkipped
            << "; global store_reset=" << deltaStats.storeReset << " agent_shift=" << deltaStats.agentShift
            << " dropped_frame=" << mGiDroppedResets << " cache_restart_headers=" << mGiCacheRestarts
            << "; emitters faint=" << deltaStats.faint << " moved=" << deltaStats.cls[0]
            << " emission=" << deltaStats.cls[1] << " texture=" << deltaStats.cls[2]
            << " appear=" << deltaStats.cls[3] << " release=" << deltaStats.cls[4]
            << " reingest_cancelled=" << deltaStats.reingest << " full_queue=" << deltaStats.capMerges
            << " (let go " << deltaStats.capDropped << ", header lists " << mGiSphDropped << ")"
            << ", spheres " << s_emSpheres << " in " << s_emFrames << " frames, largest " << std::fixed
            << std::setprecision(1) << s_emLargest << " m (" << kCls[std::min(s_emLargestClass, 5u)]
            << ", emitter r " << s_emLargestEmitter << " m, reach " << s_emLargestReach << " m, local id "
            << localId << "), largest header sphere: hard " << s_hdrLargest << " m, soft " << s_hdrLargestSoft
            << " m" << std::defaultfloat
            << "; header_hard=" << s_headerHard << "; header_soft=" << s_headerSoft
            << "; local_spheres=" << nr << "; headers_with_spheres=" << s_sphHdrs << "/" << s_allHdrs << LL_ENDL;
        s_sphHdrs = s_allHdrs = s_emFrames = 0;
        s_emSpheres = s_headerHard = s_headerSoft = 0;
        s_emLargest = s_emLargestEmitter = s_emLargestReach = s_hdrLargest = s_hdrLargestSoft = 0.f;
        s_emLargestClass = 0;
        s_emLargestObject.setNull();
    }
    // Moving deltas from the store into this bounded pending list is not an
    // acknowledgement: current-frame AS publication can still prevent recordGI.
    // Snapshot the generation; only recordGITail may consume these invalidations.
    mGiHdrSceneSerial = mGiSceneSerial;
    mGiHdrGlobalSerial = mGiGlobalSerial;
    // A skipped recording still owns its covered batch. Merge new pending events
    // into it, then leave a separate empty queue for geometry arriving after this header.
    for (uint32_t i = 0; i < mGiSphereN; ++i)
        if (giAddSphere(mGiHdrSpheres, mGiHdrSphereN, mGiSpheres[i], mShadowEye)) ++mGiSphDropped;
    mGiSphereN = 0;
    // (GI-4) bounded local changes; only origin/store/drop recovery requests a global restart
    const size_t ns = mGiHdrSphereN;
    ++s_allHdrs;
    if (ns != 0u || mGiSphOverflow) ++s_sphHdrs;
    h.sph_n[0] = (uint32_t)ns;
    h.sph_n[1] = mGiSphOverflow ? 1u : 0u;
    for (size_t i = 0; i < ns; ++i)
    {
        const LLCleanChangeSpheres::Sphere& sp = mGiHdrSpheres[i];
        h.sph[i][0] = sp.c[0]; h.sph[i][1] = sp.c[1]; h.sph[i][2] = sp.c[2]; h.sph[i][3] = sp.w;
        const float r = sp.w;
        if (r >= 0.f) ++s_headerHard; else ++s_headerSoft;
        if (r > 0.f) s_hdrLargest = std::max(s_hdrLargest, r);
        else s_hdrLargestSoft = std::max(s_hdrLargestSoft, -r);
    }
    // Optional publication layout only: leave the pending queues and their merge order
    // untouched. Every sphere survives with identical coordinates/radius/soft strength.
    static LLCachedControl<bool> s_hardPrefix(gSavedSettings, "RenderVKPerfGiHardPrefix", false);
    if (s_hardPrefix)
    {
        const uint32_t hard = LLCleanChangeSpheres::writeHardFirst(mGiHdrSpheres.data(), (uint32_t)ns, h.sph);
        h.sph_n[1] |= (hard + 1u) << 1u;
    }
    if (mGiSphOverflow && !mGiStateFresh)
    {
        // Global recovery is reserved for invalid coordinate/store/submission state,
        // never ordinary streaming, unknown skin poses or bounded queue pressure.
        ++mGiCacheRestarts;
        static double s_next = 0.0;
        const double now = giClock();
        if (now >= s_next)
        {
            s_next = now + 10.0;
            LL_INFOS("VKShadow") << "GI radiance cache restarted: origin/store/submission recovery; "
                                 << mGiCacheRestarts << " restart headers so far"
                                 << LL_ENDL;
        }
    }
    if (mGiStateFresh || mGiSphOverflow)
    {   // a new / restarted cache: every slot empty (the WAR barrier above covers the fill)
        if (mGiStateSlots != ~0u && mGiStateSlots != 0u)
            {
                LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_gi_setup");
                vkCmdFillBuffer(cmd, mGiState.buffer, sizeof(GiHeader), giCacheBytes(mGiStateSlots), 0u);
            }
        mGiStateFresh = false;
    }
    // A recorded cache clear does not refresh reservoirs or temporal history.
    // Keep the global rejection bit sticky through skipped/aborted GI recordings.
    // (#1039) REUSE reads the CHG-TAG stage's tags exactly while this header has GI_F_LIGHT_CHG and the reservoirs
    // (GI_F_RES: the fresh candidate's bit; its taps' bits need the temporal reuse as well -- the stage tests that):
    // recordGISplitReuse runs the stage on this, not on the settings again
    mGiHdrChg = (h.cfg[0] & GI_F_LIGHT_CHG) != 0u && (h.cfg[0] & GI_F_RES) != 0u;
    {
        LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_gi_setup");
        vkCmdUpdateBuffer(cmd, mGiState.buffer, 0, sizeof(h), &h);
    }
}

// invalidateProjShadows (the ingest's static change spheres this frame, before they are cleared):
// kept for the next GI header (the cache invalidates the cells they touch).
void LLCleanRendererVK::giNoteSpheres()
{
    extern std::vector<float> gVKChangedSpheres;
    extern bool gVKChangedOverflow;
    // A transient AS/readiness skip can retain GI resources and old histories.
    // Keep their pending events; continue collecting geometry while resources exist.
    if (!mGiNow && !mGiWanted && !mGiState.buffer) return;
    const size_t n = gVKChangedSpheres.size() / 4;
    if (gVKChangedOverflow)
    {   // Preserve the bounded known regions; skip the unbounded/unknown remainder.
        // Busy geometry must not evict every cache cell on every frame.
        ++mGiGeometrySkipped;
    }
    bool added = false;
    for (size_t i = 0; i < n; ++i)
    {
        const float* s = &gVKChangedSpheres[i * 4];
        if (!std::isfinite(s[0]) || !std::isfinite(s[1]) || !std::isfinite(s[2]) || !std::isfinite(s[3])) continue;
        // (geometry: HARD -- the history inside is wrong; a hard change is never let go)
        giAddSphere(mGiSpheres, mGiSphereN, LLCleanChangeSpheres::make(s, std::fabs(s[3])), mShadowEye);
        added = true;
    }
    if (added) ++mGiSceneSerial;
}

bool LLCleanRendererVK::giTraceReady() const
{
    // (this frame's TLAS: giDecide decided on last frame's build)
    return mGiNow && mGiDn && mGiDn->allocated() && mGiTracePipe && mRtReflLayout && mRT && mRT->tlasValid()
        && mRT->consumerSet() != VK_NULL_HANDLE;
}

// ---- (#978) GI EARLY TRACE (RenderVKGIEarlyTrace, default 1, live) ----
// THE FRAME GRAPH (split deferred lighting, async queue on, the early pre-pass + early probe faces;
// G = graphics queue, A = async compute queue, in recording order; "=>" = a semaphore edge):
//   G: front-end join, terrain bake, alpha sort, #901 shelter + accum (the G-buffer reads them),
//      pre-pass (1a), VRS rate, G-buffer
//   A: RT build (BLAS refit / TLAS: q_rt_build)            => joined on G before the shadow culls
//   G: shadow rig cull, cascades, projector tiles (+ records b19), weather occlusion / sim / snow
//   A: mask / SSAO (q_mask_ao; forked after the projector tiles: the trace reads b19)
//   G: mirror (+ the late particle compute)
//   A: THE DEFERRED CHAIN (q_defer_light, recordDeferredLighting's split / feature-split kernels):
//      prep -> [merged RT shadows: sun certificates, the ReSTIR DI EMISSIVE BUILD (#91 / #986), the
//      #869 occlusion resolve, pass 15 + the projector denoiser] -> SHADOW(+CLASSIFY) -> REFLECT ->
//      RT reflections (#859) -> [unmerged: pass 15 + the emissive build here] -> GI (the trace,
//      pass 11 -DCLEAN_GI_TRACE; the radiance-cache resolve; the GI denoiser) -> BASE + LOCAL (or
//      COMBINE): the lit HDR + glow
//   G: probe faces (pr_early)                              => [old: join the chain: g_join_defer]
//   G: vol fog (A: q_volumetrics), RT probe faces (A: q_probe_rt), sky, mirror occlusion, alpha
//      depth, particle lighting, cel, god rays / vol clouds, ACOUSTICS TRACE, pass 2, post.
// WHAT FORCES THE SERIALIZATION: every graphics pass after the join but the acoustics trace reads
// or draws over the lit HDR, or writes the depth the chain reads (alpha depth); every graphics
// pass before the fork is an input of the chain (the G-buffer, the TLAS, the projector tiles, the
// shelter maps the G-buffer reads). The GI trace itself reads the G-buffer + depth, this frame's
// TLAS, the light records / clusters / light grid, the projector records + atlas (GI hits take
// shadow-mapped projectors: clean_rt_shade.glsl rtProjShadowMapQ0), the weather maps at hits
// (binding 49), last frame's filtered probes, and THIS FRAME'S EMISSIVE BUILD + OCCLUSION LISTS
// (mRegir: EM_F_GI_SPLIT, the light-grid picks) -- built at the chain's head, inside the ReSTIR
// pass (recordProjStoch). It may not run beside the RT reflection kernels (they read the GI header
// + radiance cache the trace inserts / accumulates into and the resolve rewrites, and write the
// light grid's stat words) nor pass 15 (writes mRegir): so it cannot start before them, and on the
// same queue its order there costs nothing. Its consumers: BASE / COMBINE (the denoised GI + the
// header); after the combine the forward lit FS (draw set 0 b53, the cache), next frame's RT
// reflections / GI hits (the cache). Not the vol fog (its own sky-visibility rays), the probe RT
// faces, the acoustics trace.
// EARLY TRACE: the chain stays one async segment (its head + the GI trace start exactly as early
// as before -- nothing on the async queue precedes them but the mask / SSAO), but it STOPS before
// the combine (deferStashCombine: BASE + LOCAL / COMBINE stashed in mDeferTail); the graphics queue
// no longer joins it after the probe faces but records the acoustics trace and forks the vol fog /
// RT probe segments first, and joins it UP TO its own value (the later segments stay outstanding)
// in pass 1b right before the combine, which then runs on graphics (recordDeferTail).
// Needs RenderVKAsyncLayoutsOnGraphics (depth stays DEPTH_READ_ONLY on graphics across the window).
// Timers: g_gi_fork (the fork on graphics), g_gi_window (the window's unmarked rest), g_join_gi (the
// pure wait), g_defer_tail (the combine), derived q_gi_beside (graphics work in the window) and
// q_gi_hidden (q_defer_light - g_join_gi: the chain time graphics did not wait for).
bool LLCleanRendererVK::deferStashCombine(bool feat, const VKDeferPush& p, uint32_t viewOff, uint32_t rw, uint32_t rh)
{
    const bool armed = mGiEarlyArm;
    mGiEarlyArm = false;
    if (!armed) return false;
    mDeferTail.pending = true;
    mDeferTail.giLag = mGiLagNow;   // (#978 phase 2: the trace follows the combine -- giLagRecord)
    mDeferTail.feat = feat;
    mDeferTail.p = p;
    mDeferTail.viewOff = viewOff;
    mDeferTail.rw = rw;
    mDeferTail.rh = rh;
    return true;
}

void LLCleanRendererVK::recordDeferTail(VkCommandBuffer& cmd)
{
    if (!mDeferTail.pending) return;
    mDeferTail.pending = false;
    // A continuation after the join: nothing bound. The split layout's sets + this frame's push,
    // as recordGI / the beforeCombine join leave them.
    const VkDescriptorSet sets[4] = {mDsetDraw0, mDsetDraw1, mDsetDefer, mDsetDeferSplit};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mDeferSplitLayout, 0, 4, sets, 1, &mDeferTail.viewOff);
    vkCmdPushConstants(cmd, mDeferSplitLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mDeferTail.p), &mDeferTail.p);
    recordDeferCombine(cmd, mDeferTail.p, mDeferTail.feat, mDeferTail.rw, mDeferTail.rh, false);
}

// ---- (#978 PHASE 2) GI LATENCY FRAME (RenderVKGILatencyFrame, default 1 since 2026-11-09, live) ----
// Phase 1 left the GI trace (c_gi_trace 9-13 ms in the owner's sim) inside this frame's q_defer_light:
// graphics still waited for it (g_join_gi) before the combine, because the combine reads its result.
// ONE FRAME OF LATENCY: the combine reads LAST frame's GI -- reprojected (clean_deferred_light.comp
// deferGiFetchLag; header GI_F_LAG) -- and this frame's trace + cache resolve + GI denoiser run AFTER
// the combine, on their own async segment, beside everything graphics still has to record:
//   G: ... G-buffer, cascades / projector tiles / weather, mirror      (as phase 1)
//   A: q_mask_ao, q_defer_light = THE HEAD ONLY: prep -> [merged RT shadows: sun certificates, ReSTIR DI
//      emissive build, occlusion resolve, pass 15 + projector denoiser] -> SHADOW(+CLASSIFY) -> REFLECT ->
//      RT reflections -> [unmerged: pass 15] -- stops BEFORE recordGI (deferStashCombine, giLag)
//   G: probe faces, acoustics trace, vol fog / RT probe forks (the phase-1 window) -> join up to the head
//      (g_join_gi: now the shadows + reflections only) -> the combine (g_defer_tail; reads GI(N-1))
//      -> giLagRecord: depth snapshot + the forward state copy -> FORK q_gi_lag
//   A: q_gi_lag: [GI trace -> cache resolve -> GI denoiser] for frame N (reads the snapshot, writes the
//      GI images / reservoirs / state the combine of frame N + 1 reads)
//   G: scope 2 (sky, mirror occlusion, ALPHA DEPTH), particle lighting, cel, god rays (kept on graphics:
//      an async segment forked after q_gi_lag would queue behind it and its join would wait for GI),
//      pass 2 (water, alpha, particles), the DLSS / FSR resolve
//      -> g_gi_lag_window | join up to q_gi_lag (g_join_gi_lag: the pure wait, ~0 when GI fit beside)
//   G: the SSR copy, motion blur, bloom, exposure, tonemap, FX, selection + HUD, P (pipelining), probe filters (q_post
//      or graphics), the caller's UI replay, present. (giLagJoin again after the HUD: a backstop, no-op.)
// WHY THE JOIN BEFORE THE SSR COPY: the GI kernel's static descriptor use (what sync validation checks,
// whatever runs at run time) covers draw set 0's previous colour / depth (b7 / b8: the SSR copy writes
// them) and set 2 / 3 images on the render-target alias block (the G-buffer, below render scale 1 the
// split intermediates mDsImg: the bloom pyramid / cross filter -- written after the resolve -- share that
// memory). Everything between the combine and the SSR copy writes none of the GI kernel's resources (the
// HDR / glow / reactive / refraction targets, the TAA / DLSS state, particle records, cel targets: not
// bound to it), so nothing else is double-buffered. Going further (overlapping bloom .. HUD too) needs a
// draw set 0 copy without b7 / b8 and the G-buffer + mDsImg off the alias block (~20 B / render px).
// WHY NOT ACROSS THE FRAME BOUNDARY (to frame N + 1's combine): besides the G-buffer and depth the trace
// reads this frame's TLAS (frame N + 1's build refits it in place), the light records / light info /
// frame info (single buffers: frame N + 1's light pass rewrites them, in the front end -- possibly on the
// async queue itself, frame pipelining), the projector records + atlas, the weather maps, the emissive
// build + occlusion lists in mRegir (frame N + 1's ReSTIR pass), the probe arrays (this frame's filters)
// and bindless textures (retired against the in-flight FENCE, which covers the graphics tail only).
// Double-buffering all of that is several hundred MB plus a ring for each of those writers; joining in the
// frame keeps every lifetime as it is (the tail, and so the fence, follows the join; asyncBeginFrame's
// re-wait of mAsyncJoinedLast orders it before frame N + 1's first graphics submission too).
// WHAT IS SNAPSHOT / DOUBLE-BUFFERED (only while the setting is on; logged with sizes at creation):
//   - the pre-pass DEPTH (4 B / render px): the alpha-depth pass writes the live depth in scope 2 and the
//     layout returns to ATTACHMENT; copied right after the combine (graphics) into mGiLagDepth, which the
//     trace reads through mGiLagSet (set 2: binding 4 = the copy, the rest copied from mDsetDefer every
//     lag frame -- one set per frame slot, updated while no submitted work uses it: asyncBeginFrame
//     host-waited the slot's compute work). The G-buffer itself is not written again before frame N + 1's
//     G-buffer pass (and its alias partner only after the join);
//   - the FORWARD STATE (header + resolved cells, 32 B / cache slot): the trace inserts / accumulates /
//     resolves into mGiState while pass 2's forward lit FS reads the cache (draw set 0 b53): b53 names
//     mGiStateFwd, copied from mGiState before the fork (the forward pass reads GI(N-1)'s cache with this
//     frame's header -- the deferred pixels' lag exactly);
//   - the GI OUTPUTS need no second copy: the combine (graphics, before the fork) is their only reader in
//     the frame; the next writer is this frame's q_gi_lag (ordered by the fork's semaphore).
// INVALIDATION: the header's GI_F_LAG_STALE (giDecide: mGiLagLast -- last recording traced into these
// images at this extent, consecutive GI frames; new images / state, a resize, a GI setting change that
// re-creates them, an offscreen / dropped recording in between) and the frame info's history reset
// (teleport / region crossing / camera cut: ssr.taa_params.z) -> the combine takes the radiance cache's
// cell, else the probes. Disocclusions (camera motion) per pixel the same. Probe faces / cube snapshots /
// the mirror never run the trace (forward views: they read the forward copy); an offscreen recording runs
// no GI at all. Resources are (re)created only at the frame start (giPrestep), never in a recording; a
// frame whose resources are not in place runs GI_F_LAG off or the trace on graphics after the combine.
// FALLBACKS (always "trace after the combine", so the header's GI_F_LAG stays true for the frame): no
// async fork / the snapshot at another extent / the set missing -> the trace on graphics right after
// the combine (live depth, no snapshot); the chain not stashed (RenderVKGIEarlyTrace paths off, the
// lighting on graphics) -> the trace right after the combine inside recordDeferredLighting.
// TIMERS: g_join_gi (the head), g_gi_lag_fork (the snapshot + copy + fork), q_gi_lag (the segment, async
// clock: c_gi_trace / c_gi_cache / c_gi_dn_* inside), g_gi_lag_window (graphics work beside it, since the
// last mark), g_join_gi_lag (the wait), derived q_gi_lag_hidden = q_gi_lag - g_join_gi_lag.
bool LLCleanRendererVK::giLagReady(uint32_t rw, uint32_t rh) const
{
    return mGiFwdBound && mGiStateFwd.buffer && mGiState.buffer && mGiStateFwdSlots == mGiStateSlots
        && mGiLagDepth.image && mGiLagDepthW == rw && mGiLagDepthH == rh && mGiLagDepthFmt == mDepthFormat
        && mGiLagSet[0] && mGiLagSet[kAsyncSlots - 1];
}

void LLCleanRendererVK::giLagDestroyRes()
{
    if (!mCtx) return;
    if (mGiStateFwd.buffer) { mCtx->destroyBuffer(mGiStateFwd); mGiStateFwd = GpuBuffer{}; }
    if (mGiLagDepth.image) { mCtx->destroyImage(mGiLagDepth); mGiLagDepth = GpuImage{}; }
    mGiStateFwdSlots = ~0u;
    mGiLagDepthW = mGiLagDepthH = 0;
    mGiLagDepthFmt = VK_FORMAT_UNDEFINED;
    mGiLagLast.have = false;
}

void LLCleanRendererVK::giLagFree()
{
    if (!mCtx || (!mGiStateFwd.buffer && !mGiLagDepth.image)) return;
    vkDeviceWaitIdle(mCtx->device());
    giLagDestroyRes();
    writeGiBindings();   // (b53 back on mGiState)
    LL_INFOS("Vulkan") << "one-frame-latency GI (#978) off: its depth snapshot / forward state copy freed" << LL_ENDL;
}

// The mode's resources at render extent rw x rh (giPrestep, or in place with RenderVKResizeAtFrameStart 0):
// each change waits for the device; b53 rewritten. False: refused (#697: retry in 30 s, the mode stays off).
bool LLCleanRendererVK::giLagEnsure(uint32_t rw, uint32_t rh)
{
    if (!mCtx || !mGiState.buffer || !mDslDefer || !mDsetDefer || !rw || !rh) return false;
    VkDevice dev = mCtx->device();
    const double now = giClock();
    bool changed = false;
    const uint32_t slots = mGiStateSlots == ~0u ? 0u : mGiStateSlots;
    if (!mGiStateFwd.buffer || mGiStateFwdSlots != slots)
    {
        vkDeviceWaitIdle(dev);
        if (mGiStateFwd.buffer) { mCtx->destroyBuffer(mGiStateFwd); mGiStateFwd = GpuBuffer{}; }
        const VkDeviceSize bytes = sizeof(GiHeader) + (VkDeviceSize)slots * 32u + 16u;
        {
            VkMemScope memcat(VKMEM_DENOISE);
            VkDeviceContext::SoftAllocScope soft(mCtx);
            mGiStateFwd = mCtx->createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        }
        if (!mGiStateFwd.buffer)
        {
            mGiLagRetryAt = now + 30.0;
            LL_WARNS("Vulkan") << "one-frame-latency GI (#978): the forward state copy (" << ((bytes + (1u << 19)) >> 20)
                               << " MB) was refused (VRAM); the mode stays off, retry in 30 s" << LL_ENDL;
            writeGiBindings();
            return false;
        }
        mCtx->setObjectName(VK_OBJECT_TYPE_BUFFER, (uint64_t)mGiStateFwd.buffer, "giStateFwd");
        // (zeroed: a forward pass before the first copy reads GI_F_ON clear -- the probes)
        const VkBuffer fb = mGiStateFwd.buffer;
        mCtx->submitImmediate([fb](VkCommandBuffer c) { vkCmdFillBuffer(c, fb, 0, VK_WHOLE_SIZE, 0u); });
        mGiStateFwdSlots = slots;
        changed = true;
    }
    if (!mGiLagDepth.image || mGiLagDepthW != rw || mGiLagDepthH != rh || mGiLagDepthFmt != mDepthFormat)
    {
        vkDeviceWaitIdle(dev);
        if (mGiLagDepth.image) { mCtx->destroyImage(mGiLagDepth); mGiLagDepth = GpuImage{}; }
        {
            // (#1608: a layer per view, read through its 2D_ARRAY view -- set 2 binding 4 is the lighting's
            //  texture2DArray uDepthG; one view: the 2D image of before + a 1-layer 2D_ARRAY view, no memory)
            VkMemScope memcat(VKMEM_DENOISE);
            VkDeviceContext::SoftAllocScope soft(mCtx);
            mGiLagDepth = mCtx->createScreenImage(rw, rh, mViewCount, mDepthFormat,
                                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                                  VK_IMAGE_ASPECT_DEPTH_BIT);
            if (mGiLagDepth.image && !mCtx->addArrayView(mGiLagDepth, VK_IMAGE_ASPECT_DEPTH_BIT))
            {
                mCtx->destroyImage(mGiLagDepth);
                mGiLagDepth = GpuImage{};
            }
        }
        mGiLagDepthW = mGiLagDepth.image ? rw : 0u;
        mGiLagDepthH = mGiLagDepth.image ? rh : 0u;
        mGiLagDepthFmt = mGiLagDepth.image ? mDepthFormat : VK_FORMAT_UNDEFINED;
        changed = true;
        if (!mGiLagDepth.image)
        {
            mGiLagRetryAt = now + 30.0;
            LL_WARNS("Vulkan") << "one-frame-latency GI (#978): the " << rw << "x" << rh << " depth snapshot was refused (VRAM);"
                                  " the mode stays off, retry in 30 s" << LL_ENDL;
            writeGiBindings();
            return false;
        }
        mCtx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)mGiLagDepth.image, "giLagDepth");
    }
    if (!mGiLagSet[0])
    {
        // set 2 copies for the q_gi_lag segment (one per frame slot; same layout as mDsetDefer)
        if (mGiLagPool.handle() == VK_NULL_HANDLE
            && mGiLagPool.create(dev, "GI lag set 2", {{mDslDefer, kAsyncSlots}}) != VK_SUCCESS)
        {
            mGiLagRetryAt = now + 30.0;
            return false;
        }
        for (uint32_t i = 0; i < kAsyncSlots; ++i)
            if (mGiLagPool.allocate(mDslDefer, &mGiLagSet[i]) != VK_SUCCESS) mGiLagSet[i] = VK_NULL_HANDLE;
        if (!mGiLagSet[0] || !mGiLagSet[kAsyncSlots - 1])
        {
            mGiLagRetryAt = now + 30.0;
            LL_WARNS("Vulkan") << "one-frame-latency GI (#978): its descriptor sets were refused; the mode stays off" << LL_ENDL;
            return false;
        }
    }
    if (changed)
    {
        vkDeviceWaitIdle(dev);
        mGiLagLast.have = false;
        writeGiBindings();
        LL_INFOS("Vulkan") << "one-frame-latency GI (#978, RenderVKGILatencyFrame): depth snapshot " << rw << "x" << rh
                           << " " << (((uint64_t)rw * rh * 4u + (1u << 19)) >> 20) << " MB, forward state copy "
                           << ((sizeof(GiHeader) + (uint64_t)slots * 32u + (1u << 19)) >> 20) << " MB (" << slots
                           << " cache slots)" << LL_ENDL;
    }
    return true;
}

// mGiState (header + resolved cells) -> mGiStateFwd for pass 2's forward lit FS (b53): once per GI frame,
// on graphics -- before q_gi_lag's fork on a lag frame, else after the lighting (+ its GI) is complete.
void LLCleanRendererVK::giFwdCopy(VkCommandBuffer cmd)
{
    if (mGiFwdCopied) return;
    mGiFwdCopied = true;
    if (!mGiFwdBound || !mGiStateFwd.buffer || !mGiState.buffer || mGiStateFwdSlots != mGiStateSlots) return;
    const VkDeviceSize slots = mGiStateSlots == ~0u ? 0u : mGiStateSlots;
    // WAR: last frame's forward reads (FRAGMENT) / copy; RAW: this frame's header upload (transfer), the
    // lighting's / an in-queue trace's storage writes (COMPUTE). (q_gi_lag of the last frame: joined.)
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                   | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    m.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &d);
    const VkBufferCopy c{0, 0, sizeof(GiHeader) + slots * 32u};   // (the accumulators: the trace's own)
    {
        LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_gi_setup");
        vkCmdCopyBuffer(cmd, mGiState.buffer, mGiStateFwd.buffer, 1, &c);
    }
    m.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    m.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &d);
}

// recordWorld pass 1b, right after the stashed combine (recordDeferTail; depth still DEPTH_READ_ONLY
// when depthRoHeld): this frame's GI trace. On the async queue (q_gi_lag) when everything it needs is in
// place -- then depth leaves for the attachment layout here (depthRoHeld cleared); else on graphics now.
void LLCleanRendererVK::giLagRecord(VkCommandBuffer& cmd, bool& depthRoHeld)
{
    if (!mDeferTail.giLag) return;
    mDeferTail.giLag = false;
    const VKDeferPush p = mDeferTail.p;
    const uint32_t viewOff = mDeferTail.viewOff;
    // (no trace: the next frame's header says GI_F_LAG_STALE. Depth not READ_ONLY -- never expected: the
    //  stash needs RenderVKAsyncLayoutsOnGraphics, which holds it so -- set 2 binding 4 could not be read.)
    if (!giTraceReady() || !depthRoHeld) return;
    const VkDescriptorSet set = mGiLagSet[mAsyncSlot % kAsyncSlots];
    const bool async = mAsyncNow.on && set && mDepth.image
                    && giLagReady(mDepthW, mDepthH) && mDepthW == p.width && mDepthH == p.height;
    if (!async)
    {
        // (graphics, the live depth still READ_ONLY: set 2 as the combine had it)
        if (mAsyncNow.on) LLVKHitch::count("giLagOnGraphics", 1u);
        recordGI(cmd, p, viewOff, false);
        return;
    }
    // 1. THE DEPTH SNAPSHOT (the combine's / the head's sampled reads are done: the join + this queue)
    {
        VkImageMemoryBarrier2 b[2]{};
        b[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b[0].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b[0].srcAccessMask = VK_ACCESS_2_NONE;
        b[0].dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; b[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        b[0].oldLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL; b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b[0].image = mDepth.image; b[0].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, mViewCount};   // (#1608: every view)
        // (last frame's q_gi_lag reads of the copy: joined last frame; contents discarded)
        b[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b[1].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; b[1].srcAccessMask = VK_ACCESS_2_NONE;
        b[1].dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; b[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b[1].image = mGiLagDepth.image; b[1].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, mViewCount};
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.imageMemoryBarrierCount = 2; d.pImageMemoryBarriers = b;
        vkCmdPipelineBarrier2(cmd, &d);
        VkImageCopy r{};
        r.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, mViewCount};   // (#1608: layer v -> layer v)
        r.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, mViewCount};
        r.extent = {mDepthW, mDepthH, 1};
        {
            LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_gi_setup");
            vkCmdCopyImage(cmd, mDepth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mGiLagDepth.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        }
        // the live depth -> the attachment layout for scope 2 (what depthToAttachGfx does from READ_ONLY);
        // the copy -> the trace's sampled reads (set 2 binding 4: DEPTH_READ_ONLY)
        b[0].srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; b[0].srcAccessMask = VK_ACCESS_2_NONE;
        b[0].dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        b[0].dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b[0].newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        b[1].srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; b[1].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b[1].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; b[1].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(cmd, &d);
        depthRoHeld = false;
    }
    // 2. pass 2's forward copy of the state (GI(N-1)'s cache, this frame's header)
    giFwdCopy(cmd);
    // 3. this slot's set 2: mDsetDefer's descriptors (the G-buffer, the light grid, the
    //    froxels, the view UBO, the reservoir grid; not 7 / 11: the trace uses neither) + binding 4 = the copy.
    //    Legal here: nothing submitted uses it (asyncBeginFrame host-waited this slot's compute work) and
    //    nothing recorded so far binds it.
    {
        static const uint32_t kCopy[] = {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u, 10u, 12u};
        VkCopyDescriptorSet cp[sizeof(kCopy) / sizeof(kCopy[0])]{};
        uint32_t n = 0;
        for (uint32_t b : kCopy)
        {
            VkCopyDescriptorSet& c = cp[n++];
            c.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
            c.srcSet = mDsetDefer; c.srcBinding = b; c.srcArrayElement = 0;
            c.dstSet = set; c.dstBinding = b; c.dstArrayElement = 0;
            c.descriptorCount = 1;
        }
        // (#1608: the 2D_ARRAY view -- the lighting kernels' texture2DArray; the 2D view did not match it)
        const VkDescriptorImageInfo di{VK_NULL_HANDLE, mGiLagDepth.arrayView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set; w.dstBinding = 4; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; w.pImageInfo = &di;
        vkUpdateDescriptorSets(mCtx->device(), 1, &w, n, cp);
    }
    // 4. the fork: q_gi_lag waits everything recorded on graphics so far (the combine's reads of the GI
    //    images / state, the copies above); NOT joined here -- giLagJoin before the SSR copy. The outstanding
    //    value of the segments forked before it (vol fog, RT probe faces) is kept for their own joins.
    const uint64_t prevOut = mAsyncOutstanding;
    VkCommandBuffer lc = asyncFork(cmd, "q_gi_lag");
    mGiSet2 = set;
    if (!lc)
    {
        // (no fork -- the segment budget: on graphics, from the snapshot)
        LLVKHitch::count("giLagOnGraphics", 1u);
        recordGI(cmd, p, viewOff, false);
        mGiSet2 = VK_NULL_HANDLE;
        return;
    }
    recordGI(lc, p, viewOff, true);
    mGiSet2 = VK_NULL_HANDLE;
    asyncEndCompute(lc, "q_gi_lag");
    mGiLagValue = mAsyncOutstanding;
    mAsyncOutstanding = prevOut;
    gpuSubMark(cmd, "g_gi_lag_fork");
}

// recordWorld, after the TAA / upscaler resolve, before the SSR copy (+ a backstop after the HUD, before
// P / the probe filters): the q_gi_lag segment's join (up to its value: anything forked after it stays
// outstanding).
void LLCleanRendererVK::giLagJoin(VkCommandBuffer& cmd)
{
    if (!mGiLagValue) return;
    const uint64_t v = mGiLagValue;
    gpuSubMark(cmd, "g_gi_lag_window");   // Overlap-window boundary, not a pass sample.
    asyncJoinUpTo(cmd, v);
    if (mAsyncJoinedLast >= v)
    {
        mGiLagValue = 0;
        gpuSubMark(cmd, "g_join_gi_lag");   // Join boundary; uncovered queue time is a gap.
        return;
    }
    // (no graphics continuation left: asyncSubmitFrame's tail waits it -- correct, later)
    LLVKHitch::count("giLagJoinDeferred", 1u);
    LL_WARNS_ONCE("Vulkan") << "#978: the one-frame-latency GI segment's join moved to the frame's tail "
                               "(graphics continuation budget)" << LL_ENDL;
}

// After the RT reflections and the projector sampler, before BASE / COMBINE (both split paths):
// the GI trace, the cache's resolve, the GI denoiser; the split layout's sets + push re-bound after.
void LLCleanRendererVK::recordGI(VkCommandBuffer cmd, const VKDeferPush& p, uint32_t viewOff, bool onAsync)
{
    if (!giTraceReady()) return;
    // (#1608 STEREO) the TLAS on this queue whatever it is (VR lights on the async queue; the one-frame-latency trace
    // on q_gi_lag) -- the stereo mask / pass 15 / VR GI rule (c7a688462d); a no-op when already taken
    if (stereoActive()) releaseTlas(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    const VkDescriptorSet rtSet = mRT->consumerSet();
    const GiCfg cfg = giCfg();
    const uint32_t dw = mGiDn->width(), dh = mGiDn->height();
    const bool split = mGiSplitNow && giSplitReady(dw, dh);
    // (#1036; its HIT / MISS list headers were reset by THE LIGHT PREP ahead of the lighting -- a frame whose early
    //  point did not, never expected, shades with SHADE)
    const bool shadeSplit = split && mGiShadeSplitNow && giShadeSplitReady()
                         && mLightPrep.headersDone && mLightPrep.giLists;
    // (#1039) THE VISIBILITY QUEUE (llcleanvisqueuevk.cpp): HIT-MAT the material, HIT-REQ queues the hits' sun rays
    // and the budget's light rays, the walk traces them, HIT-LIGHT resolves (no ray query in it); the hits past the
    // region take the fused HIT-LIGHT as a tail. RenderVKVisQueue bit 64: M1 (HIT-MAT queues the sun rays alone,
    // HIT-LIGHT keeps the light walk). No region this recording: the fused HIT-MAT + HIT-LIGHT.
    const uint32_t vs = mVqSetGi;
    const VkPipeline vqMat = vs == 1u ? mGiShadePipe[GSS_HITMAT] : mGiVqPipe[0][VQS_MAT];
    const bool vq = shadeSplit && mVqGiNow && mVq.buffer && mVisWalkPipe && vqMat && mGiVqPipe[vs][VQS_LIGHT]
                 && (vs == 0u || mGiVqPipe[vs][VQS_REQ]);
    if (vq) visQueueBegin(cmd, VQR_GI, 0u, 0u);
    // (#1036: the HIT / MISS lists' headers -- count 0, dispatch (0, 0, 1) -- are THE LIGHT PREP's: reset after
    //  last frame's stages read them, published to TRACE's appends and the stages' indirect reads by its closing
    //  barrier on this queue / the join on graphics. No COMPUTE -> TRANSFER drain here any more.)
    // WAR on the GI images / reservoirs / cache (last frame's readers) + the header upload
    // (#1031: + the re-shade list's header, last frame's indirect dispatches read it). THE SPLIT, its trace before
    // the combine (no one-frame-latency GI): not needed -- TRACE (+ the tags' late pass) writes only the GI records,
    // the list headers / appends and the tag words; every reader of those (last frame's stages, here or on
    // q_gi_lag) is earlier on this queue behind recordDeferLight's opening barrier (src COMPUTE: an execution
    // dependency, the indirect reads included as logically earlier stages) or on graphics behind the fork's
    // semaphore; the header upload is the front end's (that barrier / the semaphore). Nothing recorded before it in
    // the chain (pass 15 + its denoiser, SHADOW / CLASSIFY, REFLECT, the RT reflections) writes what TRACE reads or
    // reads what it writes (the frame walk's dependency audit, llcleanvisqueuevk.cpp); the GI images, reservoirs
    // and cache are written only by the later stages, behind TRACE's own RAW barriers. Kept: the fused kernel
    // (writes everything at once -- the RT reflections just before it read the cache) and the one-frame-latency
    // GI (its trace follows BASE / LOCAL, which read the GI images / state, on the same queue).
    if (!split || mGiLagNow)
    {
        VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
        vkCmdPipelineBarrier2(cmd, &d);
    }
    VKRtReflPush rp{};
    rp.d = p;
    rp.d.flags = p.flags & 0x00FFFFFFu;
    rp.rtr[0] = cfg.rayLen;
    // rtrOpts: bits 1-3 the hit's projector shadow-ray budget, bit 4 the light grid (built this
    // recording), bit 7 off (no RT reflection cost counters), bits 8-9 0 (raster-parity alpha test)
    rp.rtr[1] = (float)((std::min<U32>(cfg.hitShadow, 4u) << 1) | (mRegirNow ? 16u : 0u));
    rp.rtr[2] = 0.f;
    rp.rtr[3] = std::max(mSamplerAniso, 1.f);
    rp.rtr2[0] = (float)dw;
    rp.rtr2[1] = (float)dh;
    rp.rtr2[2] = (float)(mGiFrame & 0xFFFFFu);
    rp.rtr2[3] = (float)mGiDn->parity();
    // (RenderVKGIDebug 11 / 12) THE GI SPECK OVERLAY: its per-texel buffer (made / cleared on demand, retired when the
    // views go off), its address in rtr3.zw for the GI finish (no GI stage that takes this push reads rtr3.zw: the
    // visibility queue's stages take vp below, whose zw name the region)
    if (cfg.debug >= 11u)
    {
        if (!mGiDbgBuf.buffer || mGiDbgW != dw || mGiDbgH != dh)
        {
            if (mGiDbgBuf.buffer) retireBuffer(mGiDbgBuf);
            mGiDbgBuf = GpuBuffer{};
            mGiDbgW = mGiDbgH = 0;
            VkMemScope memcat(VKMEM_DENOISE);
            VkDeviceContext::SoftAllocScope soft(mCtx);
            GpuBuffer nb = mCtx->createBuffer((VkDeviceSize)dw * dh * 4u,
                                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
            if (nb.buffer && nb.address)
            {
                mGiDbgBuf = nb;
                mGiDbgW = dw; mGiDbgH = dh;
                mGiDbgFill = true;
                mCtx->setObjectName(VK_OBJECT_TYPE_BUFFER, (uint64_t)mGiDbgBuf.buffer, "giDebugOverlay");
            }
            else if (nb.buffer) mCtx->destroyBuffer(nb);
        }
        if (mGiDbgBuf.buffer && mGiDbgFill)
        {
            {
                LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_gi_setup");
                vkCmdFillBuffer(cmd, mGiDbgBuf.buffer, 0, VK_WHOLE_SIZE, 0u);
            }
            VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            m.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
            m.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
            vkCmdPipelineBarrier2(cmd, &d);
            mGiDbgFill = false;
        }
        if (mGiDbgBuf.buffer)
        {
            rp.rtr3[2] = (uint32_t)(mGiDbgBuf.address & 0xFFFFFFFFull);
            rp.rtr3[3] = (uint32_t)(mGiDbgBuf.address >> 32);
        }
    }
    else if (mGiDbgBuf.buffer)
    {
        retireBuffer(mGiDbgBuf);
        mGiDbgBuf = GpuBuffer{};
        mGiDbgW = mGiDbgH = 0;
    }
    // (set 2: #978 phase 2's per-slot copy with binding 4 = the depth snapshot, on the q_gi_lag segment)
    const VkDescriptorSet set2 = mGiSet2 ? mGiSet2 : mDsetDefer;
    const VkDescriptorSet sets[5] = {mDsetDraw0, mDsetDraw1, set2, mDsetDeferSplit, rtSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mRtReflLayout, 0, 5, sets, 1, &viewOff);
    VK_PUSH_BUDGET_CHECK(rp);
    vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rp), &rp);
    // (#1039) the queue's stages' push (rtr3.zw = the region), and the walk and HIT-LIGHT as functions: recorded below,
    // or (M6) in the frame walk and after it
    VKRtReflPush vp = rp;
    if (vq)
    {
        const VkDeviceAddress qa = visQueueAddr(VQR_GI);
        vp.rtr3[2] = (uint32_t)(qa & 0xFFFFFFFFull);
        vp.rtr3[3] = (uint32_t)(qa >> 32);
    }
    auto walk = [this, rp, onAsync](VkCommandBuffer c)
    {
        visQueueWalk(c, VQR_GI, rp, onAsync ? "c_gi_walk" : "g_gi_walk");
        gpuSubMark(c, onAsync ? "c_gi_walk" : "g_gi_walk");
    };
    auto light = [this, rp, vp, vs, dw, dh, onAsync](VkCommandBuffer c)
    {
        vkCmdPushConstants(c, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(vp), &vp);
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, mGiVqPipe[vs][VQS_LIGHT]);
        {
            GpuPassScope scope(*this, c, onAsync ? "c_gi_hitlight" : "g_gi_hitlight");
            vkCmdDispatchIndirect(c, mGiHit.buffer, 1u * 16u + 4u);
        }
        if (mVqRegion[VQR_GI].entries < dw * dh * mViewCount)   // (#1608: every view's texels)
        {
            // the hits past the region: the fused HIT-LIGHT from the first of them (independent texels)
            VKRtReflPush tp = rp;
            tp.rtr3[1] = mVqRegion[VQR_GI].entries;
            vkCmdPushConstants(c, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(tp), &tp);
            vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, mGiShadePipe[GSS_HITLIGHT]);
            {
                GpuPassScope scope(*this, c, onAsync ? "c_gi_hitlight" : "g_gi_hitlight");
                vkCmdDispatchIndirect(c, mGiHit.buffer, 1u * 16u + 4u);
            }
        }
        gpuSubMark(c, onAsync ? "c_gi_hitlight" : "g_gi_hitlight");
        vkCmdPushConstants(c, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rp), &rp);
        visQueueStats(c, VQR_GI);
    };
    if (split)
    {
        // (#1031) THE STAGES (clean_deferred_light.comp THE GI KERNEL SPLIT), each reading what the one
        // before it wrote into the records (compute -> compute RAW); RESHADE and TAIL take their
        // workgroup count from the re-shade list's header (REUSE's appends: + the indirect read).
        // (#1039 M6 LEVELS, llcleanvisqueuevk.cpp THE LEVELS) the chain schedules the producers instead: TRACE, MISS +
        // HIT-MAT, HIT-REQ (M2) as its stages 0-2 (each binds its sets + push: other consumers' stages run between
        // them), the walk in the wave, HIT-LIGHT as its head, REUSE and the rest after -- the same commands.
        if (shadeSplit && vq && set2 == mDsetDefer && visQueueM6Leveled(VQC_GI))
        {
            const bool chgLate = mGiHdrChg && !mLightPrep.chgDone && mGiSplitPipe[GIS_CHGTAG];
            if (chgLate)
            {
                LLVKHitch::count("giChgTagLate", 1u);
                mLightPrep.chgDone = true;
            }
            VKRtReflPush tp = rp;
            tp.rtr[2] = 1.f;   // (TRACE: + the HIT / MISS lists)
            auto trace = [this, rp, tp, sets, viewOff, chgLate, dw, dh, onAsync](VkCommandBuffer c)
            {
                vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, mRtReflLayout, 0, 5, sets, 1, &viewOff);
                if (chgLate)
                {
                    vkCmdPushConstants(c, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rp), &rp);
                    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, mGiSplitPipe[GIS_CHGTAG]);
                    {
                        GpuPassScope scope(*this, c, onAsync ? "c_gi_chgtag" : "g_gi_chgtag");
                        vkCmdDispatch(c, (dw + 7u) / 8u, (dh + 7u) / 8u, mViewCount);   // (#1608: z = the views)
                    }
                    gpuSubMark(c, onAsync ? "c_gi_chgtag" : "g_gi_chgtag");
                }
                vkCmdPushConstants(c, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(tp), &tp);
                vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, rtBcPipe(mGiSplitPipe[GIS_TRACE]));   // (#1128)
                {
                    GpuPassScope scope(*this, c, onAsync ? "c_gi_trace" : "g_gi_trace");
                    vkCmdDispatch(c, (dw + 7u) / 8u, (dh + 7u) / 8u, mViewCount);   // (#1608: z = the views)
                }
                gpuSubMark(c, onAsync ? "c_gi_trace" : "g_gi_trace");
            };
            auto mat = [this, rp, vp, vqMat, sets, viewOff, onAsync](VkCommandBuffer c)
            {
                vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, mRtReflLayout, 0, 5, sets, 1, &viewOff);
                vkCmdPushConstants(c, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rp), &rp);
                vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, mGiShadePipe[GSS_MISS]);
                {
                    GpuPassScope scope(*this, c, onAsync ? "c_gi_miss" : "g_gi_miss");
                    vkCmdDispatchIndirect(c, mGiHit.buffer, 2u * 16u + 4u);
                }
                gpuSubMark(c, onAsync ? "c_gi_miss" : "g_gi_miss");
                vkCmdPushConstants(c, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(vp), &vp);
                vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, vqMat);
                {
                    GpuPassScope scope(*this, c, onAsync ? "c_gi_hitmat" : "g_gi_hitmat");
                    vkCmdDispatchIndirect(c, mGiHit.buffer, 1u * 16u + 4u);
                }
                gpuSubMark(c, onAsync ? "c_gi_hitmat" : "g_gi_hitmat");
            };
            std::function<void(VkCommandBuffer)> req;
            if (vs == 1u)
                req = [this, vp, vs, sets, viewOff, onAsync](VkCommandBuffer c)
                {
                    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, mRtReflLayout, 0, 5, sets, 1, &viewOff);
                    vkCmdPushConstants(c, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(vp), &vp);
                    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, mGiVqPipe[vs][VQS_REQ]);
                    {
                        GpuPassScope scope(*this, c, onAsync ? "c_gi_hitreq" : "g_gi_hitreq");
                        vkCmdDispatchIndirect(c, mGiHit.buffer, 1u * 16u + 4u);
                    }
                    gpuSubMark(c, onAsync ? "c_gi_hitreq" : "g_gi_hitreq");
                };
            visQueueM6Prod(VQC_GI, 0u, {trace, mat, req});
            visQueueM6Defer(VQC_GI, walk,
                [this, rp, p, viewOff, onAsync, sets](VkCommandBuffer c)
                {
                    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, mRtReflLayout, 0, 5, sets, 1, &viewOff);
                    vkCmdPushConstants(c, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rp), &rp);
                    recordGISplitReuse(c, rp, onAsync);
                    recordGITail(c, p, viewOff, onAsync);
                });
            mVqM6Head[VQC_GI] = [this, light, sets, viewOff](VkCommandBuffer c)
            {
                vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, mRtReflLayout, 0, 5, sets, 1, &viewOff);
                light(c);
            };
            return;
        }
        auto raw = [cmd](bool indirect)
        {
            VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            if (indirect)
            {
                m.dstStageMask |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
                m.dstAccessMask |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
            }
            VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
            vkCmdPipelineBarrier2(cmd, &d);
        };
        // (#1039) the CHANGE TAGS' bits 0-2 belong to the light prep (recordLightPrep: ahead of the lighting, it
        // reads only last frame's reservoirs + this frame's header). Not recorded there this frame (never
        // expected: giChgTagLate): here, beside TRACE -- it touches only the tag words and last frame's reservoir
        // layer, neither of which TRACE reads or writes; the RAW after TRACE orders it before HIT-LIGHT's /
        // SHADE's bit 3 and REUSE.
        if (mGiHdrChg && !mLightPrep.chgDone && mGiSplitPipe[GIS_CHGTAG])
        {
            LLVKHitch::count("giChgTagLate", 1u);
            mLightPrep.chgDone = true;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mGiSplitPipe[GIS_CHGTAG]);
            {
                GpuPassScope scope(*this, cmd, onAsync ? "c_gi_chgtag" : "g_gi_chgtag");
                vkCmdDispatch(cmd, (dw + 7u) / 8u, (dh + 7u) / 8u, mViewCount);   // (#1608: z = the views)
            }
            gpuSubMark(cmd, onAsync ? "c_gi_chgtag" : "g_gi_chgtag");
        }
        // 1 TRACE: the fresh rays' walks -> hit records (+ the list's header reset; #1036: + the HIT /
        // MISS lists, push rtr.z = 1)
        if (shadeSplit)
        {
            VKRtReflPush tp = rp;
            tp.rtr[2] = 1.f;
            vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(tp), &tp);
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rtBcPipe(mGiSplitPipe[GIS_TRACE]));   // (#1128 beacon twin when on)
        {
            GpuPassScope scope(*this, cmd, onAsync ? "c_gi_trace" : "g_gi_trace");
            vkCmdDispatch(cmd, (dw + 7u) / 8u, (dh + 7u) / 8u, mViewCount);   // (#1608: z = the views)
        }
        gpuSubMark(cmd, onAsync ? "c_gi_trace" : "g_gi_trace");
        if (shadeSplit)
        {
            // (#1036) 2 as MISS + HIT-MAT (independent texels: no barrier between them) -> HIT-LIGHT, each
            // indirect over its list (TRACE's appends: + the indirect read)
            vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rp), &rp);
            raw(true);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mGiShadePipe[GSS_MISS]);
            {
                GpuPassScope scope(*this, cmd, onAsync ? "c_gi_miss" : "g_gi_miss");
                vkCmdDispatchIndirect(cmd, mGiHit.buffer, 2u * 16u + 4u);
            }
            gpuSubMark(cmd, onAsync ? "c_gi_miss" : "g_gi_miss");
            // (#1039: the queue's stages -- push rtr3.zw = the region)
            if (vq) vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(vp), &vp);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vq ? vqMat : mGiShadePipe[GSS_HITMAT]);
            {
                GpuPassScope scope(*this, cmd, onAsync ? "c_gi_hitmat" : "g_gi_hitmat");
                vkCmdDispatchIndirect(cmd, mGiHit.buffer, 1u * 16u + 4u);
            }
            gpuSubMark(cmd, onAsync ? "c_gi_hitmat" : "g_gi_hitmat");
            if (vq)
            {
                if (vs == 1u)
                {
                    // (M2) HIT-REQ: HIT-MAT's RT LIT RECORDS -> the hits' requests, sum / share slots, the walk's
                    // raised dispatch (the push stays the queue's)
                    raw(false);
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mGiVqPipe[vs][VQS_REQ]);
                    {
                        GpuPassScope scope(*this, cmd, onAsync ? "c_gi_hitreq" : "g_gi_hitreq");
                        vkCmdDispatchIndirect(cmd, mGiHit.buffer, 1u * 16u + 4u);
                    }
                    gpuSubMark(cmd, onAsync ? "c_gi_hitreq" : "g_gi_hitreq");
                }
                // HIT-MAT's (M2: HIT-REQ's) requests + the raised dispatch -> the walk; the results -> HIT-LIGHT
                raw(true);
                walk(cmd);
                raw(false);
                light(cmd);
            }
            else
            {
                raw(false);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mGiShadePipe[GSS_HITLIGHT]);
                {
                    GpuPassScope scope(*this, cmd, onAsync ? "c_gi_hitlight" : "g_gi_hitlight");
                    vkCmdDispatchIndirect(cmd, mGiHit.buffer, 1u * 16u + 4u);
                }
                gpuSubMark(cmd, onAsync ? "c_gi_hitlight" : "g_gi_hitlight");
            }
        }
        else
        {
            // 2 SHADE: the hits' (or the sky's) radiance -> the samples
            raw(false);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mGiSplitPipe[GIS_SHADE]);
            {
                GpuPassScope scope(*this, cmd, onAsync ? "c_gi_shade" : "g_gi_shade");
                vkCmdDispatch(cmd, (dw + 7u) / 8u, (dh + 7u) / 8u, mViewCount);   // (#1608: z = the views)
            }
            gpuSubMark(cmd, onAsync ? "c_gi_shade" : "g_gi_shade");
        }
        recordGISplitReuse(cmd, rp, onAsync);
    }
    else
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rtBcPipe(mGiTracePipe));   // (#1128 beacon twin when on)
        {
            GpuPassScope scope(*this, cmd, onAsync ? "c_gi_trace" : "g_gi_trace");
            vkCmdDispatch(cmd, (dw + 7u) / 8u, (dh + 7u) / 8u, mViewCount);   // (#1608: z = the views)
        }
        gpuSubMark(cmd, onAsync ? "c_gi_trace" : "g_gi_trace");
    }
    recordGITail(cmd, p, viewOff, onAsync);
}

// The GI split from its REUSE stage on (REUSE, RESHADE + its tail) -- a function of its own so the #1039 M6 frame walk
// can record it after the walks. The RT layout's sets bound, rp = the GI push.
void LLCleanRendererVK::recordGISplitReuse(VkCommandBuffer cmd, const VKRtReflPush& rp, bool onAsync)
{
    const GiCfg cfg = giCfg();
    const uint32_t dw = mGiDn->width(), dh = mGiDn->height();
    auto raw = [cmd](bool indirect)
    {
        VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        if (indirect)
        {
            m.dstStageMask |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
            m.dstAccessMask |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        }
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
        vkCmdPipelineBarrier2(cmd, &d);
    };
    // (the shade stages' samples -- and HIT-LIGHT's / SHADE's bit 3 on the change tags, whose bits 0-2 the light
    //  prep wrote before this frame's GI: #1039, no CHG-TAG pass here any more -> REUSE)
    raw(false);
    // 3 REUSE: the reservoirs, the visibility rays, every store but the listed texels'
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mGiSplitPipe[GIS_REUSE]);
    {
        GpuPassScope scope(*this, cmd, onAsync ? "c_gi_reuse" : "g_gi_reuse");
        vkCmdDispatch(cmd, (dw + 7u) / 8u, (dh + 7u) / 8u, mViewCount);   // (#1608: z = the views)
    }
    gpuSubMark(cmd, onAsync ? "c_gi_reuse" : "g_gi_reuse");
    // 4 RESHADE + 3 again as TAIL over the re-shade list: only a frame whose picks can want a
    // re-shade (the temporal reuse + GI_F_LIGHT_CHG); empty lists dispatch no workgroup
    if ((mGiFlags & GI_F_TEMPORAL) == 0u || !cfg.lightChg) return;
    raw(true);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mGiSplitPipe[GIS_RESHADE]);
    {
        GpuPassScope scope(*this, cmd, onAsync ? "c_gi_reshade" : "g_gi_reshade");
        vkCmdDispatchIndirect(cmd, mGiHit.buffer, 4u);
    }
    gpuSubMark(cmd, onAsync ? "c_gi_reshade" : "g_gi_reshade");
    raw(true);
    VKRtReflPush tp = rp;
    tp.rtr[2] = 1.f;   // (TAIL: the list's texels, with their re-shade results)
    vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(tp), &tp);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mGiSplitPipe[GIS_REUSE]);
    {
        GpuPassScope scope(*this, cmd, onAsync ? "c_gi_tail" : "g_gi_tail");
        vkCmdDispatchIndirect(cmd, mGiHit.buffer, 4u);
    }
    gpuSubMark(cmd, onAsync ? "c_gi_tail" : "g_gi_tail");
    vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rp), &rp);
}

// (RenderVKGIMoverSamples; clean_gi_movers.glsl) FULL-RESOLUTION GI ON MOVER PIXELS: after every GI trace stage (their
// reservoirs / GEOM this frame's) -- the header words cleared, LIST over the render extent (the movers, up to the cap),
// TRACE indirect over the list (a fresh ray + the covering texels' reservoirs + the pixel's own history), the counters
// into the readback ring (the log line). The RT layout's sets and the GI push are bound again here (the #1039 M6 walk
// records the tail apart) and the plain GI push restored after.
void LLCleanRendererVK::recordGIMovers(VkCommandBuffer cmd, const VKDeferPush& p, uint32_t viewOff, bool onAsync)
{
    if (!mGiMovNow || !mGiMov.buffer || !mGiMoverPipe || !mRT || !mGiDn) return;
    const VkDescriptorSet rtSet = mRT->consumerSet();
    if (rtSet == VK_NULL_HANDLE || p.width != mGiMovW || p.height != mGiMovH) return;
    const GiCfg cfg = giCfg();
    const uint32_t dw = mGiDn->width(), dh = mGiDn->height();
    auto barrier = [cmd](VkPipelineStageFlags2 src, VkAccessFlags2 srcA, VkPipelineStageFlags2 dst, VkAccessFlags2 dstA)
    {
        VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        m.srcStageMask = src; m.srcAccessMask = srcA;
        m.dstStageMask = dst; m.dstAccessMask = dstA;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
        vkCmdPipelineBarrier2(cmd, &d);
    };
    // the header / indirect words (last frame's TRACE read them, its copy too): cleared; a new buffer whole
    barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (mGiMovFill)
    {
        {
            LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_gi_setup");
            vkCmdFillBuffer(cmd, mGiMov.buffer, 0, VK_WHOLE_SIZE, 0u);
        }
        mGiMovFill = false;
        // (the words below overwrite the fill's: ordered)
        barrier(VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    const uint32_t init[8] = {0u, 0u, 0u, 0u, 0u, 1u, 1u, 0u};   // (e[0..1] the counters, e[2..3] the indirect x, 1, 1)
    {
        LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_gi_setup");
        vkCmdUpdateBuffer(cmd, mGiMov.buffer, 0, sizeof(init), init);
    }
    // + the GI trace stages' reservoirs / GEOM (TRACE reads them)
    barrier(VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    // the GI trace's push (recordGI), + rtr.z (LIST: -cap, TRACE: cap) and rtr3.zw (the buffer)
    VKRtReflPush rp{};
    rp.d = p;
    rp.d.flags = p.flags & 0x00FFFFFFu;
    rp.rtr[0] = cfg.rayLen;
    rp.rtr[1] = (float)((std::min<U32>(cfg.hitShadow, 4u) << 1) | (mRegirNow ? 16u : 0u));
    rp.rtr[2] = 0.f;
    rp.rtr[3] = std::max(mSamplerAniso, 1.f);
    rp.rtr2[0] = (float)dw;
    rp.rtr2[1] = (float)dh;
    rp.rtr2[2] = (float)(mGiFrame & 0xFFFFFu);
    rp.rtr2[3] = (float)mGiDn->parity();
    VKRtReflPush mp = rp;
    mp.rtr[2] = -(float)mGiMovCap;
    mp.rtr3[2] = (uint32_t)(mGiMov.address & 0xFFFFFFFFull);
    mp.rtr3[3] = (uint32_t)(mGiMov.address >> 32);
    const VkDescriptorSet set2 = mGiSet2 ? mGiSet2 : mDsetDefer;
    const VkDescriptorSet sets[5] = {mDsetDraw0, mDsetDraw1, set2, mDsetDeferSplit, rtSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mRtReflLayout, 0, 5, sets, 1, &viewOff);
    VK_PUSH_BUDGET_CHECK(mp);
    vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mp), &mp);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mGiMoverPipe);
    {
        GpuPassScope scope(*this, cmd, onAsync ? "c_gi_mov_list" : "g_gi_mov_list");
        vkCmdDispatch(cmd, (p.width + 7u) / 8u, (p.height + 7u) / 8u, 1);
    }
    gpuSubMark(cmd, onAsync ? "c_gi_mov_list" : "g_gi_mov_list");
    barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    mp.rtr[2] = (float)mGiMovCap;
    vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mp), &mp);
    {
        GpuPassScope scope(*this, cmd, onAsync ? "c_gi_mov_trace" : "g_gi_mov_trace");
        vkCmdDispatchIndirect(cmd, mGiMov.buffer, 16u);
    }
    gpuSubMark(cmd, onAsync ? "c_gi_mov_trace" : "g_gi_mov_trace");
    vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rp), &rp);
    mGiMovLastHave = true;
    mGiMovLastFrame = mGiFrame;
    // the counters (movers seen, cap hits) into the 3-slot ring; the slot written 3 recordings ago read back
    {
        VkMemScope memcat(VKMEM_READBACK);
        if (!mGiMovRb.buffer)
            mGiMovRb = mCtx->createBuffer(3u * 16u, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        const uint32_t slot = mGiMovRbSlot;
        mGiMovRbSlot = (mGiMovRbSlot + 1u) % 3u;
        if (mGiMovRbUsed[slot] && mGiMovRb.mapped)
        {
            const uint32_t* s = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mGiMovRb.mapped) + (size_t)slot * 16u);
            const double listed = (double)std::min(s[0], mGiMovCap);
            mGiMovStat.listed += listed;
            mGiMovStat.hits += (double)s[1];
            mGiMovStat.peak = std::max(mGiMovStat.peak, listed);
            mGiMovStat.capFrames += s[1] != 0u ? 1.0 : 0.0;
            mGiMovStat.frames += 1.0;
        }
        mGiMovRbUsed[slot] = false;
        if (mGiMovRb.buffer)
        {
            barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            const VkBufferCopy c{0, (VkDeviceSize)slot * 16u, 8u};
            {
                LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_gi_setup");
                vkCmdCopyBuffer(cmd, mGiMov.buffer, mGiMovRb.buffer, 1, &c);
            }
            mGiMovRbUsed[slot] = true;
        }
    }
    // THE LOG LINE (every 10 s while on): the listed pixels and the cap hits
    const double now = giClock();
    if (mGiMovStat.logAt == 0.0) mGiMovStat.logAt = now + 10.0;
    if (now >= mGiMovStat.logAt && mGiMovStat.frames > 0.0)
    {
        const double f = mGiMovStat.frames;
        LL_INFOS("Vulkan") << "GI mover samples: " << (uint64_t)(mGiMovStat.listed / f + 0.5) << " px listed per frame (peak "
                           << (uint64_t)mGiMovStat.peak << ", cap " << mGiMovCap << " = "
                           << std::fixed << std::setprecision(1) << 100.0 * (double)mGiMovCap / (double)std::max(dw * dh, 1u)
                           << " % of the GI texels); cap hit in " << (uint64_t)mGiMovStat.capFrames << " of " << (uint64_t)f
                           << " frames (" << (uint64_t)(mGiMovStat.hits / f + 0.5) << " px a frame left at the reduced extent)"
                           << std::defaultfloat << LL_ENDL;
        mGiMovStat = GiMovStat{};
        mGiMovStat.logAt = now + 10.0;
    }
}

// recordGI after its trace stages (the radiance cache, the counters, the GI denoiser, the split layout re-bound) --
// a function of its own so the #1039 M6 frame walk can record it after the walks.
// (RenderVKGILayerFill; clean_deferred_light.comp -DCLEAN_GI_LAYER) THE FRONT BLEND LAYER AS A GI RECEIVER: the radiance
// cache is filled by the opaque G-buffer's GI texels only, so a surface only an alpha-blend layer occupies (a cloud's
// underside, a pane away from any wall) had few or no cells and its forward read fell back cell by cell -- the static
// mosaic of #1499. Before the cache's resolve: one lane per 2 x 2 group of half-resolution texels (the four in turn, by
// the GI frame) finds the front blend layer along pass 14's block ray, traces one fresh cosine ray from it (the GI
// trace's hit shading) and adds the estimate to the layer point's cell (inserted if new). Desktop only (a stereo GI
// frame has its own trace). The RT layout's sets and the GI push are bound here (the #1039 M6 walk records the tail
// apart). Cost: one first-hit camera ray per lane + one GI ray where it meets a lit blend layer.
void LLCleanRendererVK::recordGILayers(VkCommandBuffer cmd, const VKDeferPush& p, uint32_t viewOff, bool onAsync)
{
    if (!mGiLayerPipe || !mRT || !mGiDn || mGiVrNow) return;
    if ((mGiFlags & GI_F_CACHE) == 0u || (mGiFlags & GI_F_CACHE_FILL) == 0u || mGiStateSlots == ~0u || mGiStateSlots == 0u) return;
    const GiCfg cfg = giCfg();
    if (!cfg.layerFill) return;
    const VkDescriptorSet rtSet = mRT->consumerSet();
    if (rtSet == VK_NULL_HANDLE) return;
    VKRtReflPush rp{};
    rp.d = p;
    rp.d.flags = p.flags & 0x00FFFFFFu;
    rp.rtr[0] = cfg.rayLen;
    rp.rtr[1] = (float)((std::min<U32>(cfg.hitShadow, 4u) << 1) | (mRegirNow ? 16u : 0u));
    rp.rtr[2] = 0.f;
    rp.rtr[3] = std::max(mSamplerAniso, 1.f);
    rp.rtr2[0] = (float)mGiDn->width();
    rp.rtr2[1] = (float)mGiDn->height();
    rp.rtr2[2] = (float)(mGiFrame & 0xFFFFFu);
    rp.rtr2[3] = (float)mGiDn->parity();
    {
        // the GI trace stages' cache inserts / accumulations (and the mover pass's) before this pass's: ordered, as
        // recordGIMovers orders its own after them (with RenderVKGIMoverSamples off nothing else did)
        VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
        vkCmdPipelineBarrier2(cmd, &d);
    }
    const VkDescriptorSet set2 = mGiSet2 ? mGiSet2 : mDsetDefer;
    const VkDescriptorSet sets[5] = {mDsetDraw0, mDsetDraw1, set2, mDsetDeferSplit, rtSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mRtReflLayout, 0, 5, sets, 1, &viewOff);
    VK_PUSH_BUDGET_CHECK(rp);
    vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rp), &rp);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mGiLayerPipe);
    const uint32_t hw = (p.width + 1u) / 2u, hh = (p.height + 1u) / 2u;   // (the half-resolution grid)
    const uint32_t lx = (hw + 1u) / 2u, ly = (hh + 1u) / 2u;               // (one lane per 2 x 2 of it)
    {
        GpuPassScope scope(*this, cmd, onAsync ? "c_gi_layer" : "g_gi_layer");
        vkCmdDispatch(cmd, (lx + 7u) / 8u, (ly + 7u) / 8u, 1);
    }
    gpuSubMark(cmd, onAsync ? "c_gi_layer" : "g_gi_layer");
}

void LLCleanRendererVK::recordGITail(VkCommandBuffer cmd, const VKDeferPush& p, uint32_t viewOff, bool onAsync)
{
    recordGIMovers(cmd, p, viewOff, onAsync);   // (RenderVKGIMoverSamples: after every trace stage, before the cache's resolve)
    recordGILayers(cmd, p, viewOff, onAsync);   // (RenderVKGILayerFill: the front blend layer's cells, before the resolve)
    const GiCfg cfg = giCfg();
    const uint32_t dw = mGiDn->width(), dh = mGiDn->height();
    const VkDescriptorSet set2 = mGiSet2 ? mGiSet2 : mDsetDefer;
    bool cacheResolved = (mGiFlags & GI_F_CACHE) == 0u;
    if ((mGiFlags & GI_F_CACHE) != 0u && mGiResolvePipe && mGiStateSlots != ~0u && mGiStateSlots != 0u)
    {
        // (GI-4) the radiance cache: this frame's accumulations into the means, eviction, invalidation
        VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
        vkCmdPipelineBarrier2(cmd, &d);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mGiResolvePipe);
        const uint32_t groups = (mGiStateSlots + 63u) / 64u;   // (<= 2^22 / 64 = 65536 -> split in y)
        const uint32_t gx = std::min<uint32_t>(groups, 32768u);
        {
            GpuPassScope scope(*this, cmd, onAsync ? "c_gi_cache" : "g_gi_cache");
            vkCmdDispatch(cmd, gx, (groups + gx - 1u) / gx, 1);
        }
        gpuSubMark(cmd, onAsync ? "c_gi_cache" : "g_gi_cache");
        cacheResolved = true;
    }
    // stats: the header's counters into the ring (GPU timers on)
    if (mTsOn)
    {
        VkMemScope memcat(VKMEM_READBACK);   // #407
        constexpr VkDeviceSize kBytes = 8u * 16u;   // stat .. stat4, sph_n (z: the re-shades skipped, #980); + (gi-glass) stat5 .. stat7
        static_assert(offsetof(GiHeader, stat4) == offsetof(GiHeader, stat) + 3u * 16u, "GI stat words contiguous");
        static_assert(offsetof(GiHeader, sph_n) == offsetof(GiHeader, stat) + 4u * 16u, "GI stat words contiguous");
        const uint32_t slot = mGiStatSlot;
        mGiStatSlot = (mGiStatSlot + 1u) % 3u;
        if (mGiStatUsed[slot] && mGiStatRb.mapped)
        {
            const uint32_t* s = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mGiStatRb.mapped)
                                                                   + (size_t)slot * kBytes);
            for (int k = 0; k < 32; ++k) mGiStatAcc.s[k] += s[k];
            mGiStatAcc.livePeak = std::max(mGiStatAcc.livePeak, (double)s[12]);
            ++mGiStatAcc.frames;
        }
        mGiStatUsed[slot] = false;
        if (!mGiStatRb.buffer)
            mGiStatRb = mCtx->createBuffer(3u * kBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mGiStatRb.buffer)
        {
            VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            m.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; m.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
            vkCmdPipelineBarrier2(cmd, &d);
            const VkBufferCopy c[2] = {{offsetof(GiHeader, stat), (VkDeviceSize)slot * kBytes, 5u * 16u},
                                       {offsetof(GiHeader, stat5), (VkDeviceSize)slot * kBytes + 5u * 16u, 3u * 16u}};
            {
                LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_gi_setup");
                vkCmdCopyBuffer(cmd, mGiState.buffer, mGiStatRb.buffer, 2, c);
            }
            mGiStatUsed[slot] = true;
        }
    }
    // the GI denoiser (its own images; the #85 passes unchanged)
    LLCleanDenoiserVK::Params dp;
    dp.atrous = mGiAtrous;
    dp.maxHistDiffuse = cfg.history;
    dp.clampGamma = cfg.clamp;
    dp.wCut = 0.f;   // (no light-set reference: the clamp is the GI history's anti-lag)
    dp.info = true;   // (an untraced texel without reuse writes a < 0: a weight-0 sample, RenderVKGITraceEvery)
    // (GI_F_LIGHT_CHG) CHANGED LIGHTS: the trace's MOT w = the changed lights' share of the texel's
    // candidates -> the moving-light history cap (clean_dn_temporal.comp kMv): a club light cycling
    // its colour keeps RenderVKGILightChangeHistory frames of GI history there, not RenderVKGIHistory
    // (the per-channel clamp alone kept the old hue for ~30 frames: the noisy GI raw's box is wide)
    dp.moveHist = cfg.lightChg && cfg.chgHist > 0.f ? (U32)std::clamp(cfg.chgHist + 0.5f, 1.f, 15.f) : 0u;
    dp.moveDiffuseOnly = true;   // (layer 1 is the sky visibility: no light in it)
    dp.giChangedCut = true;      // (the sealed-room fade: MOT w bit 8, a CHANGED cache cell -- clean_gi_common.glsl GI_MOT_CHANGED)
    // (#87) DISOCCLUSION: young histories (< RenderVKGIDisoccFix frames) get the SVGF / ReBLUR spatial
    // boost; a failed bilinear reprojection searches the 3x3 (RenderVKGIReprojSearch) -- GI only, the
    // projector denoiser keeps its rules (clean_dn_common.glsl DN_HFIX_SHIFT / DN_F_REPROJ_SEARCH)
    dp.histFix = cfg.disoccFix;
    dp.reprojSearch = cfg.reprojSearch;
    // (#1038) a whole surface revealed at once (an avatar walking out from behind a wall): the no-history
    // texels start from the same-surface 3x3 mean, young a-trous centres relax their normal stop / widen
    // their step; a moving surface's history taps tolerate its own depth change
    dp.youngBoost = cfg.youngBoost;
    dp.moveZTol = cfg.moveZTol;
    // (GI_F_STABLE_SAMPLES) the trace's texels sampled the fixed lattice: the history's motion vector is read there
    dp.giStableSamples = mGiStableNow;
    // (RenderVKGIHistoryJitter) the history was traced on LAST frame's jittered sample lattice and the image under it
    // was jittered by last frame's offset, while the motion vectors point at last frame's UNJITTERED position: invert
    // that lattice (the stochastic shadows' rule, clean_dn_common.glsl dnSampleHistoryHc) instead of reading the
    // history up to half a texel off along last frame's jitter -- a sub-texel resample every frame (blur) and edge
    // taps that pass or fail the geometry tests with the jitter (shimmer). Not with the fixed lattice
    // (GI_F_STABLE_SAMPLES: dnSampleHistoryHc takes the jitter as the lattice's phase below full extent).
    dp.sampleGridHistory = cfg.sampleGrid && !mGiStableNow;
    dp.rasterJitter[0] = mTaaJitterOn ? mTaaJitter[0] : 0.f;
    dp.rasterJitter[1] = mTaaJitterOn ? mTaaJitter[1] : 0.f;
    // RenderVKGIDebug -> the denoiser's view (clean_pdn_common.glsl PDN_DBG_*): 2 / 5 / 6 / 8 / 9 / 10 / 11 / 12 raw
    // (the trace writes the false colour of 5 / 6 / 8 / 9 / 10 / 11 / 12; 11 / 12 are drawn from their own overlay
    // buffer anyway), 3 the history length, 4 the variance; 1 the final image
    static const uint32_t kDnDbg[14] = {0u, 0u, 1u, 2u, 3u, 1u, 1u, 0u, 1u, 1u, 1u, 1u, 1u, 1u};
    dp.debug = kDnDbg[std::min<uint32_t>(mGiDebug, 13u)];
    dp.frame = mGiFrame;
    dp.raw = rrRawSignal(kRrRawGi, mGiDn->width(), mGiDn->height());   // #870 raw routing (DLSS RR reconstructs the GI)
    // (RenderVKNoiseStats) the noise probe: the trace's RAW irradiance (IN layer 0) before the temporal pass, the
    //  FINAL denoised one after the a-trous -- each texel against itself last frame, still camera only (measurement)
    bool noiseGi = false;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<bool> s_noiseGi(gSavedSettings, "RenderVKNoiseStats", false);
    noiseGi = s_noiseGi && !dp.raw;
#endif
    if (noiseGi)
    {
        mGiDn->noiseBegin(cmd);
        mGiDn->recordNoise(cmd, 0u, false, dp.atrous, 1u, mNoiseCamStill, this, onAsync ? "c_noise" : "g_noise");
        gpuSubMark(cmd, onAsync ? "c_noise" : "g_noise");
    }
    else mGiDn->noiseRelease();
    mGiDn->recordTemporal(cmd, mDsetDraw0, p.width, p.height, dp, this, onAsync ? "c_gi_dn_tmp" : "g_gi_dn_tmp");
    gpuSubMark(cmd, onAsync ? "c_gi_dn_tmp" : "g_gi_dn_tmp");
    mGiDn->recordSpatial(cmd, p.width, p.height, dp, this, onAsync ? "c_gi_dn_sp" : "g_gi_dn_sp");
    gpuSubMark(cmd, onAsync ? "c_gi_dn_sp" : "g_gi_dn_sp");
    if (noiseGi)
    {
        mGiDn->recordNoise(cmd, 1u, true, dp.atrous, 1u, mNoiseCamStill, this, onAsync ? "c_noise" : "g_noise");
        mGiDn->noiseEnd(cmd);
        gpuSubMark(cmd, onAsync ? "c_noise" : "g_noise");
    }
    // Common completion point for fused, split, deferred-walk and lagged paths.
    // Retire precisely the header-covered batch, even if later geometry changed
    // the scene serial. Those later events are in mGiSpheres, never this batch.
    // A missing resolve/trace or newer drop/global recovery consumes neither.
    if (cacheResolved && mGiGlobalSerial == mGiHdrGlobalSerial)
    {
        mGiHdrSphereN = 0;
        mGiSphOverflow = false;
    }
    // (#978 phase 2) the trace the next frame's combine reads under GI_F_LAG (giDecide: mGiLagStale)
    mGiLagLast.have = true;
    mGiLagLast.finalIn = LLCleanDenoiserVK::finalIsIn(mGiAtrous);
    mGiLagLast.par = mGiDn->parity();
    mGiLagLast.w = dw; mGiLagLast.h = dh;
    mGiLagLast.rw = p.width; mGiLagLast.rh = p.height;
    mGiLagLast.frame = mGiFrame;
    // back to the split layout (incompatible push range: every set re-bound)
    const VkDescriptorSet ssets[4] = {mDsetDraw0, mDsetDraw1, set2, mDsetDeferSplit};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mDeferSplitLayout, 0, 4, ssets, 1, &viewOff);
    vkCmdPushConstants(cmd, mDeferSplitLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
}

std::string LLCleanRendererVK::giStatsText()
{
    GiStatAcc& a = mGiStatAcc;
    if (!a.frames) return std::string();
    const double k = 1.0 / a.frames;
    const double* s = a.s;
    const double rays = s[0];
    std::ostringstream os;
    os << std::fixed << std::setprecision(0) << "GI over " << a.frames << " frames (RenderVKGI): rays/frame "
       << rays * k << ", hits " << std::setprecision(3) << (rays > 0.0 ? s[1] / rays : 0.0) << " (sky "
       << (rays > 0.0 ? s[2] / rays : 0.0) << "), mean hit distance " << std::setprecision(2)
       << (s[1] > 0.0 ? s[3] * 0.1 / s[1] : 0.0) << " m";
    if (s[4] + s[5] + s[6] + s[7] > 0.0)
        os << "; reuse: temporal kept/ray " << std::setprecision(3) << (rays > 0.0 ? s[4] / rays : 0.0)
           << ", spatial kept/ray " << (rays > 0.0 ? s[5] / rays : 0.0) << ", reused samples rejected by visibility/ray "
           << (rays > 0.0 ? s[6] / rays : 0.0) << " (temporal picks kept without one, #985 RenderVKGIReuseVisSkip: "
           << (rays > 0.0 ? s[19] / rays : 0.0) << "/ray), re-shaded for a changed light/ray "
           << (rays > 0.0 ? s[15] / rays : 0.0) << " (skipped, its changed lights insignificant there: "
           << (rays > 0.0 ? s[18] / rays : 0.0) << "/ray, RenderVKGIReshadeThreshold), mean M " << std::setprecision(2)
           << (rays > 0.0 ? s[7] * 0.25 / rays : 0.0);
    if (s[8] + s[12] > 0.0)
        os << "; cache: hit lookups/frame " << std::setprecision(0) << s[8] * k << " found " << std::setprecision(3)
           << (s[8] > 0.0 ? s[9] / s[8] : 0.0) << ", inserts/frame " << std::setprecision(0) << s[10] * k
           << " (table full " << s[11] * k << "), live cells " << s[12] * k << " (peak " << a.livePeak << ") of "
           << (mGiStateSlots == ~0u ? 0u : mGiStateSlots) << ", evicted/frame " << s[13] * k
           << ", invalidated/frame " << s[14] * k << ", restarts (global recovery) " << mGiCacheRestarts;
    // (gi-glass) THE RAY OUTCOMES (fresh + re-shade + layer-fill rays; stat5 .. stat7): where GI rays end, and the
    // alpha-blend layers they crossed while RenderVKGITransmit is on (off: blend surfaces are invisible to GI rays)
    {
        const double n = s[20] + s[21] + s[22] + s[23] + s[24] + s[27];
        if (n > 0.0)
        {
            const double q = 1.0 / n;
            os << "; ray outcomes (of " << std::setprecision(0) << n * k << "/frame): sky " << std::setprecision(3)
               << s[20] * q << ", opaque " << s[21] * q << ", alpha-mask " << s[22] * q << ", terrain/water "
               << s[23] * q << ", avatar " << s[24] * q << ", back-face stop " << s[27] * q << "; fullbright/emissive hits "
               << s[25] * q << " (emission dropped, #91 split: " << s[26] * q << "); through alpha-blend layers "
               << s[28] * q << " (layers/ray crossing " << std::setprecision(2) << (s[28] > 0.0 ? s[29] / s[28] : 0.0)
               << ", RenderVKGITransmit " << (s[28] + s[30] > 0.0 ? "on" : "off") << ")";
        }
    }
    if (a.hdrFrames)
        os << "; changed lights/frame " << std::setprecision(1) << a.chgLights / a.hdrFrames << " of "
           << a.allLights / a.hdrFrames << " (moved or radiance changed against their GI reference: RenderVKGILightChangeCut, RenderVKGILightMoveRel)"
           << ", hot lights/frame " << a.hotLights / a.hdrFrames << " (#1087: moved within RenderVKGILightChangeHold frames)";
    mGiStatAcc = GiStatAcc{};
    return os.str();
}

// ---- (#1139) VR GI: THE WORLD-CACHE TIER (SlipstreamVRGI, default off; owner: GI off in VR until it is fast) ----
// A layered stereo frame (#1075) lights its N views with the uber kernel; the desktop's GI (the split's trace ->
// reservoirs -> GI denoiser -> COMBINE / BASE) is one view's. This tier gives the eyes GI from what is SHARED:
//   SHARED, once per frame   the GI header and the world radiance cache (clean_gi_common.glsl GI-4): its fill,
//                            its resolve (mGiResolvePipe, unchanged), the change spheres, the hot lights.
//   PER EYE (z = N)          the fresh rays: ONE fused trace dispatch over every eye's GI texels (-DCLEAN_GI_STEREO_
//                            TRACE, kViewCount = N: each eye's G-buffer layer and camera, its own seeds), each
//                            estimate added to its world cell; and the reader: the stereo uber kernel (-DCLEAN_GI_
//                            STEREO_READ) takes the resolved cells around each eye's pixel (giForwardRefl -- the
//                            forward lit surfaces' reader -- through draw set 0 b53), the forward lit FS likewise.
//   NOT BUILT (this tier)    per-eye screen reservoirs, a per-eye GI denoiser, the split's stages / the visibility
//                            queue's GI regions, one-frame-latency GI. There is no per-eye screen history at all, so
//                            nothing can bleed between the eyes -- and both eyes read the same world values (no
//                            binocular disagreement of two independently denoised signals). The price: the cache's
//                            resolution (RenderVKGICacheCellSize, 0.25 m near the head, doubling every
//                            RenderVKGICacheLevelDistance), no sub-cell contact darkening, a cell's 32-frame mean.
// BUDGET (fixed by construction): GI texels = (render extent / 4)^2 per eye, x N eyes, / SlipstreamVRGITraceEvery
// traced per frame (2: complementary checkerboards). Index at DLSS / FSR Quality (~1650 x 1830 per eye): ~94k
// rays per eye, ~190k per frame -- about a fifth of the desktop default's 920k (1440p at RenderVKGIScale 0.5),
// with no reuse ray, no re-shade and no denoiser behind them. The cache resolve is fixed (one lane per slot,
// 2^RenderVKGICacheSlotsLog2), the reader 1-8 hash finds per lit pixel (RenderVKGIForwardCacheFilter). Per-ray
// cost is the scene's (hit materials, lights): measure c_gi_vr_trace / g_gi_vr_trace, c_gi_vr_cache / g_gi_vr_cache
// (#1595: the VR scopes) and c_defer_light / g_defer_light with SlipstreamVRGI 0 / 1. VRAM: the state buffer (header + 64 B per slot: 16 MB
// at 2^18), the light grid the hits take their lights from (RenderVKGIHitLightGrid), 1 x 1 GI denoiser images
// (set 3's placeholders; nothing writes them in a stereo frame).
void LLCleanRendererVK::giInitVr(VkDevice dev)
{
    if (mViewCount <= 1u) return;   // (the desktop: no VR GI pipeline -- VR never costs non-VR)
    if (mRtReflLayout && !mGiTraceVrPipe)
    {
        VkShaderModule m = giModule(dev, kWorldDeferGiTraceVrSpv, kWorldDeferGiTraceVrSpv_bytes, "DeferGiTraceVr");
        rtDeferredCompute(dev, m, mRtReflLayout, 0u, giVrSpec(mViewCount), &mGiTraceVrPipe, "mGiTraceVrPipe (DeferGiTraceVr)");
        destroyShaderModuleDeferred(dev, m, nullptr);
    }
    // the stereo uber kernel with the cache reader: the generic lighting constants (the spec sets' folded settings
    // are not built for it -- correct, a little slower than the specialized stereo uber; VR GI frames only). The
    // shader-clock twins (RenderVKShaderClockLighting, a startup diagnostic) have no VR GI reader: giDecideVr waits.
    if (mDeferLayout && !mGiVrUberPipe && !mClkOn)
    {
        const uint32_t* code = mDeferG11 ? kWorldDeferredLightGiVrG11Spv : kWorldDeferredLightGiVrSpv;
        const size_t bytes = mDeferG11 ? kWorldDeferredLightGiVrG11Spv_bytes : kWorldDeferredLightGiVrSpv_bytes;
        VkShaderModule m = giModule(dev, code, bytes, mDeferG11 ? "DeferredLightGiVrG11" : "DeferredLightGiVr");
        rtDeferredCompute(dev, m, mDeferLayout, 0u, giVrSpec(mViewCount), &mGiVrUberPipe, "mGiVrUberPipe (DeferredLightGiVr)");
        destroyShaderModuleDeferred(dev, m, nullptr);
    }
}

// giDecide's stereo branch (the light pass, before the frame info): SlipstreamVRGI, the path, the state buffer.
void LLCleanRendererVK::giDecideVr(uint32_t rw, uint32_t rh)
{
    bool want = false;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<bool> s_vr(gSavedSettings, "SlipstreamVRGI", false);
    want = s_vr;
#endif
    if (!want)
    {
        // (as before #1139: a stereo frame runs no GI. The on -> off edge frees what VR GI held -- one device wait
        //  at the toggle; whatever the desktop's GI left before the session stays as it was.)
        if (mGiVrLogged)
        {
            mGiVrLogged = false;
            mGiVrLogKey = ~0u;
            mGiModeSetting = 0u;
            mGiWanted = false;
            LLCleanInstanceStore::instance().setGiRadianceCollect(false, 0.f, nullptr);
            LL_INFOS("VR") << "VR GI off (SlipstreamVRGI 0): both eyes take the probes' diffuse irradiance" << LL_ENDL;
            if (mCtx && (mGiState.buffer || mGiRes.buffer || (mGiDn && mGiDn->allocated())))
            {
                if (resizeAtFrameStart())
                {
                    mGiReq = GiReq{};
                    mGiReq.pending = true;
                    mGiReq.free = true;
                }
                else giFreeAll();
            }
        }
        return;
    }
    const GiCfg cfg = giVrCfg();
    LLCleanInstanceStore::instance().setGiRadianceCollect(true, cfg.rayLen, mShadowEye);
    mGiModeSetting = cfg.mode;   // (rtConsumerActive: the TLAS has a consumer)
    uint32_t why = 0u;
    if (mOffscreen) why |= 2u;
    if (!mDeferSplitOk || !mDsetDeferSplit) why |= 4u;   // (set 3: the GI state / placeholders the trace binds)
    if (!mGiDn || !mGiDn->pipelinesReady() || !mGiTraceVrPipe || !mGiResolvePipe || !mRtReflLayout) why |= 8u;
    if (!mRT) why |= 16u;
    else if (!(mRT->tlasValid() || mRT->tlasBuiltLast())) why |= 32u;
    else if (mRT->consumerSet() == VK_NULL_HANDLE) why |= 64u;
    if (!mRsResDummy.buffer) why |= 128u;
    if (mTexPressure) why |= 256u;
    if (!mGiVrUberPipe) why |= mClkOn ? 1024u : 512u;
    const bool path = why == 0u;
    mGiWanted = path;
    const uint32_t w = giDim(rw, cfg.scale), h = giDim(rh, cfg.scale);
    const uint32_t slots = 1u << cfg.slotsLog2;
    {
        const uint32_t key = (path ? 1u : 0u) | (cfg.everyLog << 1) | (why << 4) | (std::min(w, 4095u) << 20);
        if (!mGiVrLogged || key != mGiVrLogKey)
        {
            mGiVrLogged = true;
            mGiVrLogKey = key;
            LL_INFOS("VR") << "VR GI (SlipstreamVRGI, #1139): world-cache tier -- " << mViewCount << " eyes x " << w << "x" << h
                           << " GI texels (1/4 of the per-eye render extent " << rw << "x" << rh << " per axis), 1 in "
                           << (1u << cfg.everyLog) << " traced per frame (SlipstreamVRGITraceEvery): ~"
                           << ((uint64_t)w * h * mViewCount >> cfg.everyLog) << " rays/frame, one bounce into the shared "
                           << "radiance cache (" << slots << " slots), the hits' ambient from the cache; read per eye by "
                           << "the stereo lighting. No per-eye reservoirs / GI denoiser in this tier; ";
            if (path) LL_CONT << "ACTIVE";
            else
            {
                LL_CONT << "waiting on:";
                if (why & 2u)    LL_CONT << " an offscreen recording;";
                if (why & 4u)    LL_CONT << " the split lighting's set 3 not ready;";
                if (why & 8u)    LL_CONT << " the VR GI trace / cache resolve / GI denoiser placeholders not ready (compiling);";
                if (why & 16u)   LL_CONT << " no ray tracing;";
                if (why & 32u)   LL_CONT << " no TLAS built last frame;";
                if (why & 64u)   LL_CONT << " no RT consumer set;";
                if (why & 128u)  LL_CONT << " set 3's placeholder buffer missing;";
                if (why & 256u)  LL_CONT << " texture memory pressure;";
                if (why & 512u)  LL_CONT << " the VR GI lighting kernel not ready (compiling);";
                if (why & 1024u) LL_CONT << " RenderVKShaderClockLighting (its instrumented kernels have no VR GI reader);";
            }
            LL_CONT << LL_ENDL;
        }
    }
    if (!path)
    {
        mGiResPrevOk = false;
        return;   // (a stand-down keeps the state: the toggle frees it)
    }
    const double now = giClock();
    if (now < mGiRetryAt) return;
    // THE RESOURCES: the state buffer (header + cache) alone. The GI denoiser's images shrink to 1 x 1 (set 3's
    // bindings 18-21 stay valid; the stereo trace writes none of them), the reservoirs / split records go.
    const bool needImg = !mGiDn->allocated() || mGiDn->width() != 1u || mGiDn->height() != 1u || !mGiState.buffer
                      || mGiStateSlots != slots;
    const bool needRes = mGiRes.buffer != VK_NULL_HANDLE;
    const bool needHit = mGiHit.buffer != VK_NULL_HANDLE;
    if (needImg || needRes || needHit)
    {
        if (resizeAtFrameStart())
        {
            mGiReq = GiReq{};
            mGiReq.pending = true;
            mGiReq.w = 1u; mGiReq.h = 1u; mGiReq.slots = slots; mGiReq.wantRes = false; mGiReq.scale = cfg.scale;
            mGiReq.wantHit = false;
            if (needImg) return;
        }
        else if (!giApply(1u, 1u, slots, false, cfg.scale, false))
            return;
    }
    // (#978 phase 2's resources are the desktop's mode: b53 must name mGiState itself -- the trace writes it
    //  and both readers read it this frame)
    if (mGiStateFwd.buffer || mGiLagDepth.image)
    {
        if (resizeAtFrameStart()) { mGiReq.pending = true; mGiReq.lagFree = true; mGiReq.lag = false; }
        else giLagFree();
    }
    if (mGiFwdBound) return;
    mGiNow = true;
    mGiVrNow = true;
    mGiVrW = w;
    mGiVrH = h;
    mGiLastActive = now;
    ++mGiFrame;
    mGiDebug = cfg.debug;
    mGiAtrous = 1u;
    mGiFlags = GI_F_ON | GI_F_CACHE | GI_F_CACHE_FILL;   // (no GI_F_RES: one bounce, no reservoirs)
    mGiResPrevOk = false;
    mGiLagLast.have = false;
}

// recordDeferredLighting's uber path in a stereo frame, before the lighting binds its own sets / push (the RT
// pre-trace's place): the VR GI trace over every eye's texels (z = N) on the RT layout, the cache resolve, the
// counters. True: this frame's cache (just resolved, or -- no TLAS this frame -- last frame's) is valid to read,
// so the uber dispatch takes mGiVrUberPipe. Same queue as the lighting (graphics or the async compute segment).
bool LLCleanRendererVK::recordGIVr(VkCommandBuffer cmd, const VKDeferPush& p, uint32_t viewOff)
{
    // (mGiNow: reset by every recording -- a recording whose light pass decided no GI never reads the cache)
    if (!mGiNow || !mGiVrNow || !mGiVrUberPipe || mDeferDbgNow) return false;   // (the lit debug twin: no GI reader)
    const bool onAsync = mAsyncAvail && vkAsyncIsAsyncCmd(cmd);
    const GiCfg cfg = giVrCfg();
    const uint32_t dw = mGiVrW, dh = mGiVrH;
    const bool cache = mGiState.buffer && mGiStateSlots != ~0u && mGiStateSlots != 0u;
    const bool trace = cache && dw && dh && mGiTraceVrPipe && mGiResolvePipe && mRtReflLayout && mDsetDeferSplit && mGiDn
                    && mRT && mRT->tlasValid() && mRT->consumerSet() != VK_NULL_HANDLE;
    if (trace)
    {
        // (on this queue, as the stereo RT shadow mask does -- its first consumer usually took it already: no-op then)
        releaseTlas(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        // WAR: last frame's cache readers (the lighting, the hits) -> this frame's fill (the header upload: the
        // light pass's barrier)
        {
            VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
            vkCmdPipelineBarrier2(cmd, &d);
        }
        VKRtReflPush rp{};
        rp.d = p;
        rp.d.flags = p.flags & 0x00FFFFFFu;
        rp.rtr[0] = cfg.rayLen;
        // rtrOpts: bits 1-3 the hit's projector shadow-ray budget, bit 4 the light grid (built this recording)
        rp.rtr[1] = (float)((std::min<U32>(cfg.hitShadow, 4u) << 1) | (mRegirNow ? 16u : 0u));
        rp.rtr[2] = 0.f;
        rp.rtr[3] = std::max(mSamplerAniso, 1.f);
        rp.rtr2[0] = (float)dw;
        rp.rtr2[1] = (float)dh;
        rp.rtr2[2] = (float)(mGiFrame & 0xFFFFFu);
        rp.rtr2[3] = (float)mGiDn->parity();
        const VkDescriptorSet sets[5] = {mDsetDraw0, mDsetDraw1, mDsetDefer, mDsetDeferSplit, mRT->consumerSet()};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mRtReflLayout, 0, 5, sets, 1, &viewOff);
        VK_PUSH_BUDGET_CHECK(rp);
        vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rp), &rp);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mGiTraceVrPipe);
        {
            GpuPassScope scope(*this, cmd, onAsync ? "c_gi_vr_trace" : "g_gi_vr_trace");
            vkCmdDispatch(cmd, (dw + 7u) / 8u, (dh + 7u) / 8u, mViewCount);   // (z = the eyes: CLEAN_VIEW_Z)
        }
        gpuSubMark(cmd, onAsync ? "c_gi_vr_trace" : "g_gi_vr_trace");   // (#1595: the VR GI scopes, apart from the desktop's)
        // the cache: this frame's accumulations into the means, eviction, invalidation (as recordGITail)
        {
            VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
            vkCmdPipelineBarrier2(cmd, &d);
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mGiResolvePipe);
        const uint32_t groups = (mGiStateSlots + 63u) / 64u;
        const uint32_t gx = std::min<uint32_t>(groups, 32768u);
        {
            GpuPassScope scope(*this, cmd, onAsync ? "c_gi_vr_cache" : "g_gi_vr_cache");
            vkCmdDispatch(cmd, gx, (groups + gx - 1u) / gx, 1);
        }
        gpuSubMark(cmd, onAsync ? "c_gi_vr_cache" : "g_gi_vr_cache");
        // the counters into the ring (GPU timers on: giStatsText's rays/frame, cache cells)
        if (mTsOn)
        {
            VkMemScope memcat(VKMEM_READBACK);
            constexpr VkDeviceSize kBytes = 8u * 16u;   // stat .. stat4, sph_n (z: the re-shades skipped, #980); + (gi-glass) stat5 .. stat7
            const uint32_t slot = mGiStatSlot;
            mGiStatSlot = (mGiStatSlot + 1u) % 3u;
            if (mGiStatUsed[slot] && mGiStatRb.mapped)
            {
                const uint32_t* s = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mGiStatRb.mapped)
                                                                       + (size_t)slot * kBytes);
                for (int k = 0; k < 32; ++k) mGiStatAcc.s[k] += s[k];
                mGiStatAcc.livePeak = std::max(mGiStatAcc.livePeak, (double)s[12]);
                ++mGiStatAcc.frames;
            }
            mGiStatUsed[slot] = false;
            if (!mGiStatRb.buffer)
                mGiStatRb = mCtx->createBuffer(3u * kBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (mGiStatRb.buffer)
            {
                VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                m.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; m.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
                vkCmdPipelineBarrier2(cmd, &d);
                const VkBufferCopy c[2] = {{offsetof(GiHeader, stat), (VkDeviceSize)slot * kBytes, 5u * 16u},
                                           {offsetof(GiHeader, stat5), (VkDeviceSize)slot * kBytes + 5u * 16u, 3u * 16u}};
                {
                    LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_gi_setup");
                    vkCmdCopyBuffer(cmd, mGiState.buffer, mGiStatRb.buffer, 2, c);
                }
                mGiStatUsed[slot] = true;
            }
        }
        // the header's change spheres were applied by this resolve: retire exactly that batch (recordGITail's rule)
        if (mGiGlobalSerial == mGiHdrGlobalSerial)
        {
            mGiHdrSphereN = 0;
            mGiSphOverflow = false;
        }
    }
    // the resolved cells -> every eye's reader: the uber kernel here (compute) and the forward lit FS in pass 2
    // (graphics: on the async segment its join orders it)
    {
        VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                       | (onAsync ? (VkPipelineStageFlags2)0 : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
        m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
        vkCmdPipelineBarrier2(cmd, &d);
    }
    return true;
}
