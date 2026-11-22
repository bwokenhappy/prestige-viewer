/**
 * @file llcleanprojstochvk.cpp
 * @brief Ray-traced projector shadows, STOCHASTIC + DENOISED (#85 phase 2,
 *        RenderVKProjStochastic): the first producer of the diffuse / specular denoiser
 *        (llcleandenoisevk.h) -- and the test bed of the API ReSTIR DI (#869) will use.
 *
 * EXACT PATH (the default, RenderVKProjStochastic 0): the RT shadow mask traces up to 4
 * ray-traced projectors per mask texel (clean_rt_shadow_mask.comp; a 5th+ overlapping one is
 * lit unshadowed), its denoiser filters each slot, and the deferred light walk evaluates every
 * projector at every pixel -- gobo fetch, BRDF, the slot lookup (c_rtmask*, c_defer_local).
 *
 * STOCHASTIC PATH (1; split deferred frames with ray-traced projector records and a valid TLAS):
 *   1. the RT mask leaves the projector slots empty (rtMaskPushFill: negative clamp width) --
 *      its trace keeps the sun only;
 *   2. PASS 15 (clean_deferred_light.comp CLEAN_DEFER_PASS 15, mRtReflLayout), after the
 *      reflect passes: per denoiser texel (full or half the render extent, by
 *      RenderVKRTShadowMaskScale) RIS over the pixel cluster's ray-traced projectors with a gobo-free target, K = RenderVKProjStochastic
 *      Samples picks, the full terms + ONE shadow ray each (a soft disc sampled once), weighted
 *      W / (K p) -- unbiased shadowed direct light of EVERY projector (no 4-slot cap); exact
 *      (no noise) where a pixel has <= K candidates. Written demodulated with the guides;
 *   3. the DENOISER: temporal (c_dn_tmp) + RenderVKDenoiseAtrous a-trous iterations (c_dn_sp);
 *   4. the combine / LOCAL (DEFER_FLAG_PSTOCH): RATIO ESTIMATOR (Heitz et al. 2018) -- pass 15
 *      writes the per-pixel VISIBILITY RATIO (shadowed / unshadowed estimate of the same picks,
 *      diffuse and specular, rgb), the denoiser filters that, and localLight evaluates every RT
 *      projector's light exactly and fresh (colour, gobo, motion: nothing accumulated) times the
 *      denoised ratio instead of the mask's slot lookup. A light changing colour / intensity /
 *      position updates at once; only the shadow lags (cut on a W change). The cost moved: the
 *      mask's projector traces and slot lookups are gone, the per-pixel light evaluation stays.
 *   5. (#869) THE SPLIT (RenderVKProjStochasticSplitK, default 2, live; clean_pdn_common.glsl SPLIT
 *      CHANNEL): ONE shared ratio leaked -- a club projector behind a sealed room's wall took the
 *      room lamps' ratio (its gobo on the room's wall, sharp, dimmer) and darkened the lamps. Now
 *      the K strongest lights per texel (gobo-free target, the pixel cluster's list, hysteresis 1.5,
 *      ties by id) get their own ray and their own denoised visibility, keyed by a persistent
 *      per-light split id (Light.tex.w, setLights -> splitIdsFrame); the shared ratio covers the
 *      rest only. Generic core only (forced while K > 0; the per-light-type kernel writes empty
 *      slots). Cost: <= K rays more per lit texel; +64 B per denoiser texel (+80 B without the sun
 *      channel): 1280x720 full ~56 MB, 4K half ~127 MB, 4K full ~506 MB; no extra dispatch (the
 *      temporal / a-trous passes carry the channel). Debug: RenderVKDenoiseDebug 8 / 9. DLSS RR raw
 *      routing (kRrRawProj): the split lights' visibility goes raw with the rest's ratio.
 * Not covered (they keep their own rules, in either mode): water and probe / mirror captures
 * (RT projectors unshadowed), particles (their own per-particle / inline rule), alpha-blend
 * surfaces (RenderVKRTBlendProjShadows: unshadowed by default, 1 = one inline ray per projector
 * per fragment -- before that setting they ALWAYS traced inline, this comment said otherwise),
 * the uber kernel (debug twins, split refused), cel-shaded surfaces.
 * MERGED RT SHADOWS (#195 + #85, RenderVKRTShadowMerge, default on): when the sun / moon is ray
 * traced too, pass 15's merge variant (mPdnMergePipe) traces each texel's sun / moon visibility
 * with its OWN ray(s) (never in the projector pick: its intensity would starve the projectors)
 * beside the projector pick -- the same G-buffer reads, receiver and normal -- into the denoiser's
 * sun channel (llcleandenoisevk.h), and runs BEFORE the SHADOW kernel, which reads the denoised
 * sun instead of the RT shadow mask (clean_deferred_light.comp deferMergedSunT). The mask does not
 * run that frame (c_rtmask* gone; c_defer_shadowmerge instead of c_defer_pstoch) and the lighting
 * stays deferred without it. Only then: every other combination (sun mapped / off, projectors
 * mapped / off, the stochastic path off, RT off, the setting 0) runs exactly the previous path --
 * see shadowPathLog. A WEAK sun / moon (RenderVKRTShadowMergeSunMin: the light colour's luminance
 * x the sun scale against a threshold, a smooth ramp over [t, 2t]) fades its shadow toward lit and
 * below t traces no sun ray at all (night, a dim moon).
 * VRAM: the denoiser's images only (llcleandenoisevk.h), held kRtReflHoldSecs after the last
 * wanting frame, freed under texture pressure; refusals step full -> half extent -> exact path
 * (the sun channel first: a refusal of it keeps the projector-only images and today's paths).
 * GPU: c_defer_pstoch (pass 15), c_dn_tmp, c_dn_sp (g_ prefixes when the lighting is on the
 * graphics queue); compare with c_rtmask / c_rtmask_tmp / c_rtmask_sp / c_defer_local.
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

#include "llviewercontrol.h"
#include "../llcleanemissivediag.h"
#if defined(LL_HAS_CLEAN_PIPELINE)
#include "llcleaninstancestore.h"
#include "llviewerobjectlist.h"
#include "llappviewer.h"
#include "llcharacter.h"
#include "llmeshrepository.h"
#include "lltexturefetch.h"
#include "llstartup.h"
#include "llviewerobject.h"
#include "llvovolume.h"
#include "lldrawable.h"
#include "indra_constants.h"
#include "llselectmgr.h"
#endif
#include "../llvkhitch.h"
#include "vk_common.h"
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace
{
VkShaderModule pdnModule(VkDevice dev, const uint32_t* code, size_t bytes, const char* name)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
    vkNoteShaderModule(m, code, bytes, name);
    return m;
}
// The lighting kernels' debug specialization OFF (llcleanrenderervk.cpp litDebugSpec(false):
// clean_draw_common.glsl kDebugViews, constant_id 1) and the shadow core's parking (clean_shadow_core.glsl kShParkOn,
// constant_id 41: s_pdnPark, set by pdnParkDecide before the kernels are queued). Static: a queued pipeline keeps the
// pointer.
bool s_pdnPark = true;
// (#1608 LAYERED STEREO) views > 1: + kViewCount (clean_view.glsl, constant 20) = views -- pass 15's per-view kernels
// in a stereo session (clean_deferred_light.comp g_pdn_view: the grid kernels z = N, the list kernels by entry). views 1:
// exactly the two-entry infos of before.
const VkSpecializationInfo* pdnLitSpec(uint32_t views = 1)
{
    static const VkBool32 kOn[2] = {VK_FALSE, VK_TRUE}, kOff[2] = {VK_FALSE, VK_FALSE};
    static const VkSpecializationMapEntry kEntries[2] = {{1u, 0u, sizeof(VkBool32)}, {41u, sizeof(VkBool32), sizeof(VkBool32)}};
    static const VkSpecializationInfo kSiPark{2u, kEntries, 2u * sizeof(VkBool32), kOn};
    static const VkSpecializationInfo kSiNoPark{2u, kEntries, 2u * sizeof(VkBool32), kOff};
    if (views <= 1u) return s_pdnPark ? &kSiPark : &kSiNoPark;
    struct ViewSpecData { VkBool32 dbg; VkBool32 park; int32_t views; };
    static_assert(sizeof(ViewSpecData) == 12, "kViewEntries' offsets");
    static const VkSpecializationMapEntry kViewEntries[3] = {{1u, 0u, sizeof(VkBool32)}, {41u, 4u, sizeof(VkBool32)},
                                                             {GraphicsPipelineDesc::kViewCountConstId, 8u, sizeof(int32_t)}};
    static const ViewSpecData kData[2][3] = {{{VK_FALSE, VK_FALSE, 2}, {VK_FALSE, VK_FALSE, 3}, {VK_FALSE, VK_FALSE, 4}},
                                             {{VK_FALSE, VK_TRUE, 2}, {VK_FALSE, VK_TRUE, 3}, {VK_FALSE, VK_TRUE, 4}}};
    static const VkSpecializationInfo kInfo[2][3] = {
        {{3u, kViewEntries, sizeof(ViewSpecData), &kData[0][0]}, {3u, kViewEntries, sizeof(ViewSpecData), &kData[0][1]},
         {3u, kViewEntries, sizeof(ViewSpecData), &kData[0][2]}},
        {{3u, kViewEntries, sizeof(ViewSpecData), &kData[1][0]}, {3u, kViewEntries, sizeof(ViewSpecData), &kData[1][1]},
         {3u, kViewEntries, sizeof(ViewSpecData), &kData[1][2]}}};
    return &kInfo[s_pdnPark ? 1 : 0][std::min<uint32_t>(views, 4u) - 2u];
}
// (RenderVKShadowPark, read when the kernels are built) 0 off, 1 on, 2 auto: the shadow core parks its rays' inputs in
// shared memory except on AMD -- there the driver keeps the ray-query traversal stack in the same LDS, and the 11 KB
// park halved the VIS kernels' waves (15 KB per workgroup, 8 workgroups per WGP); NVIDIA's limit is the register file,
// where parking buys warps.
void pdnParkDecide(VkPhysicalDevice phys)
{
    U32 mode = 2u;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_park(gSavedSettings, "RenderVKShadowPark", 2U);
    mode = s_park;
#endif
    VkPhysicalDeviceProperties pp{};
    vkGetPhysicalDeviceProperties(phys, &pp);
    s_pdnPark = mode == 1u || (mode != 0u && pp.vendorID != 0x1002u);
    LL_INFOS("VKShadow") << "shadow core parking (RenderVKShadowPark " << mode << "): " << (s_pdnPark ? "on" : "off")
                         << " (vendor 0x" << std::hex << pp.vendorID << std::dec << ")" << LL_ENDL;
}
double pdnClock()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
uint32_t pdnDim(uint32_t r, float s) { return std::max(1u, (uint32_t)std::ceil((float)r * s)); }
#if defined(LL_HAS_CLEAN_PIPELINE)
struct EmDiagName { LLUUID id; uint32_t frame = 0, local = 0; double until = 0.0; };
std::array<EmDiagName, 15> s_emDiagNames;
uint32_t s_emDiagNameCount = 0;
std::string emDiagSafeName(std::string name)
{
    if (name.size() > 160u) name.resize(160u);
    for (char& c : name) if ((unsigned char)c < 32u || c == '"' || c == '\\') c = ' ';
    return name;
}
#endif
} // namespace

bool llEmissiveDiagProperties(const LLUUID& id, const std::string& name)
{
#if defined(LL_HAS_CLEAN_PIPELINE)
    const double now = pdnClock();
    auto it = std::find_if(s_emDiagNames.begin(), s_emDiagNames.begin() + s_emDiagNameCount, [&](const EmDiagName& n) { return n.id == id; });
    if (it == s_emDiagNames.begin() + s_emDiagNameCount) return false;
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKReSTIREmissiveDiagnostics", false);
    LLViewerObject* o = gObjectList.findObject(id);
    if (s_on && now <= it->until && o && !o->isAvatar() && !o->isAttachment() && !o->getAvatar())
        LL_INFOS("VKEmissive") << "emitter name: gpu_frame=" << it->frame << " local_id=" << it->local
                              << " uuid=" << id << " name=\"" << emDiagSafeName(name)
                              << "\" (asynchronous object properties; no owner/avatar names)" << LL_ENDL;
    *it = s_emDiagNames[--s_emDiagNameCount];
    return true;
#else
    return false;
#endif
}

// Init (initDeferSplit, inside the world PipelineBatchScope): the denoiser always -- set 3
// bindings 13-16 are declared by the combine / LOCAL kernels and must name its placeholders --
// and pass 15 when the RT layout exists.
void LLCleanRendererVK::pdnInit(VkDevice dev)
{
    if (!mPdn)
    {
        mPdn = new LLCleanDenoiserVK();
        // (#1608: its images a layer set per view, its passes z = N -- clean_dn_common.glsl DN_L)
        if (!mPdn->init(mCtx, mDslDraw0, mDepthSampler, "Projector denoiser", false, mViewCount))
        {
            LL_WARNS("Vulkan") << "Projector denoiser: init failed; split lighting unavailable" << LL_ENDL;
            mPdn->destroy();
            delete mPdn;
            mPdn = nullptr;
            return;
        }
        writePdnMotion();
    }
    pdnParkDecide(mCtx->physicalDevice());
    // (#1029) The ray-query kernels below join the deferred ray-query group (rtDeferredCompute):
    // compiled after the world pipelines, installed together; every user checks its handle per frame.
    // (#1608: pass 15's per-view kernels carry kViewCount = N in a stereo session -- pdnLitSpec(mViewCount); the
    //  world-space ones, the emissive build and the LVC maps, keep one view)
    if (mRtReflLayout && !mPdnSamplePipe)
    {
        VkShaderModule m = pdnModule(dev, kWorldDeferProjStochSpv, kWorldDeferProjStochSpv_bytes, "DeferProjStoch");
        rtDeferredCompute(dev, m, mRtReflLayout, 0u, pdnLitSpec(mViewCount), &mPdnSamplePipe,
                          "mPdnSamplePipe (DeferProjStoch)");
        destroyShaderModuleDeferred(dev, m, nullptr);
    }
    if (mRtReflLayout && !mPdnMergePipe)
    {
        // (#195 + #85) the merged RT shadow pass: pass 15 + the sun / moon trace (CLEAN_SHADOW_MERGE)
        VkShaderModule m = pdnModule(dev, kWorldDeferShadowMergeSpv, kWorldDeferShadowMergeSpv_bytes, "DeferShadowMerge");
        rtDeferredCompute(dev, m, mRtReflLayout, 0u, pdnLitSpec(mViewCount), &mPdnMergePipe,
                          "mPdnMergePipe (DeferShadowMerge)");
        destroyShaderModuleDeferred(dev, m, nullptr);
    }
    // #85 / #869 Phase 1: the generic-core twins (RenderVKShadowGenericCore; both compiled so the
    // setting stays live -- recordProjStoch falls back to the pipes above until they land)
    if (mRtReflLayout && !mPdnSampleCorePipe)
    {
        VkShaderModule m = pdnModule(dev, kWorldDeferProjStochCoreSpv, kWorldDeferProjStochCoreSpv_bytes, "DeferProjStochCore");
        rtDeferredCompute(dev, m, mRtReflLayout, 0u, pdnLitSpec(mViewCount), &mPdnSampleCorePipe,
                          "mPdnSampleCorePipe (DeferProjStochCore)");
        destroyShaderModuleDeferred(dev, m, nullptr);
    }
    if (mRtReflLayout && !mRsEmPipe)
    {
        // (#91 / #869) the emissive light records (RenderVKReSTIREmissive): pass 15's layout
        VkShaderModule m = pdnModule(dev, kWorldDeferRsEmissiveSpv, kWorldDeferRsEmissiveSpv_bytes, "DeferRsEmissive");
        rtDeferredCompute(dev, m, mRtReflLayout, 0u, pdnLitSpec(), &mRsEmPipe, "mRsEmPipe (DeferRsEmissive)");
        destroyShaderModuleDeferred(dev, m, nullptr);
    }
    if (mRtReflLayout && !mPdnMergeCorePipe)
    {
        VkShaderModule m = pdnModule(dev, kWorldDeferShadowMergeCoreSpv, kWorldDeferShadowMergeCoreSpv_bytes, "DeferShadowMergeCore");
        rtDeferredCompute(dev, m, mRtReflLayout, 0u, pdnLitSpec(mViewCount), &mPdnMergeCorePipe,
                          "mPdnMergeCorePipe (DeferShadowMergeCore)");
        destroyShaderModuleDeferred(dev, m, nullptr);
    }
    if (mRtReflLayout)
    {
        // (#1031) the generic core split: SELECT (plain, merged), SUNVIS (merged), VIS + SHADE (both passes)
        // (distinct names: RenderVKPipelineStats)
        struct Stage { const uint32_t* code; size_t bytes; const char* mod; const char* name; VkPipeline* pipe; };
        const Stage st[12] = {
            // (LVC stage 3, RenderVKShadowReachMap) the reach maps' maintenance kernel
            {kWorldDeferLvcBuildSpv, kWorldDeferLvcBuildSpv_bytes, "DeferLvcBuild", "mLvcPipe (DeferLvcBuild)", &mLvcPipe},
            // (LVC stage 0, RenderVKReSTIROcclusionCull 3) VIS's statistics variant
            {kWorldDeferShadowSplitVisLvcSpv, kWorldDeferShadowSplitVisLvcSpv_bytes, "DeferShadowSplitVisLvc",
             "mPdnVisLvcPipe (DeferShadowSplitVisLvc)", &mPdnVisLvcPipe},
            // (RenderVKProjStochasticSamples / AdaptiveSamples 5-16) the high-K SELECT (K <= 4 never binds it)
            {kWorldDeferProjStochSelectHkSpv, kWorldDeferProjStochSelectHkSpv_bytes, "DeferProjStochSelectHk",
             "mPdnSelectHkPipe (DeferProjStochSelectHk)", &mPdnSelectHkPipe[0]},
            {kWorldDeferShadowMergeSelectHkSpv, kWorldDeferShadowMergeSelectHkSpv_bytes, "DeferShadowMergeSelectHk",
             "mPdnSelectHkPipe (DeferShadowMergeSelectHk)", &mPdnSelectHkPipe[1]},
            {kWorldDeferShadowMergeSunVisIsoSpv, kWorldDeferShadowMergeSunVisIsoSpv_bytes, "DeferShadowMergeSunVisIso",
             "mPdnSunVisIsoPipe (DeferShadowMergeSunVisIso)", &mPdnSunVisIsoPipe},
            {kWorldDeferShadowSplitVisIsoSpv, kWorldDeferShadowSplitVisIsoSpv_bytes, "DeferShadowSplitVisIso",
             "mPdnVisIsoPipe (DeferShadowSplitVisIso)", &mPdnVisIsoPipe},
            {kWorldDeferProjStochSelectSpv, kWorldDeferProjStochSelectSpv_bytes, "DeferProjStochSelect",
             "mPdnSelectPipe (DeferProjStochSelect)", &mPdnSelectPipe[0]},
            {kWorldDeferShadowMergeSelectSpv, kWorldDeferShadowMergeSelectSpv_bytes, "DeferShadowMergeSelect",
             "mPdnSelectPipe (DeferShadowMergeSelect)", &mPdnSelectPipe[1]},
            {kWorldDeferShadowMergeSunVisSpv, kWorldDeferShadowMergeSunVisSpv_bytes, "DeferShadowMergeSunVis",
             "mPdnSunVisPipe (DeferShadowMergeSunVis)", &mPdnSunVisPipe},
            {kWorldDeferShadowSplitVisSpv, kWorldDeferShadowSplitVisSpv_bytes, "DeferShadowSplitVis",
             "mPdnVisPipe (DeferShadowSplitVis)", &mPdnVisPipe},
            {kWorldDeferShadowSplitShadeSpv, kWorldDeferShadowSplitShadeSpv_bytes, "DeferShadowSplitShade",
             "mPdnShadePipe (DeferShadowSplitShade)", &mPdnShadePipe},
            // (RenderVKProjStochasticAdaptive) the needs-rays mask + the adaptive tile list
            {kWorldDeferShadowSplitMaskSpv, kWorldDeferShadowSplitMaskSpv_bytes, "DeferShadowSplitMask",
             "mPdnMaskPipe (DeferShadowSplitMask)", &mPdnMaskPipe}};
        for (const Stage& sg : st)
        {
            if (*sg.pipe) continue;
            VkShaderModule m = pdnModule(dev, sg.code, sg.bytes, sg.mod);
            rtDeferredCompute(dev, m, mRtReflLayout, 0u, pdnLitSpec(sg.pipe == &mLvcPipe ? 1u : mViewCount), sg.pipe, sg.name);
            destroyShaderModuleDeferred(dev, m, nullptr);
        }
    }
    if (mRtReflLayout) sunCertInit(dev);   // (#771 Phase 2) the sun exposure certificates' maintenance kernel
    giInit(dev);   // (#87) the GI denoiser (set 3 bindings 18-23) + the GI kernels
}

void LLCleanRendererVK::pdnDestroy()
{
    if (!mCtx) return;
    if (mPdnSamplePipe) { vkDestroyPipeline(mCtx->device(), mPdnSamplePipe, nullptr); mPdnSamplePipe = VK_NULL_HANDLE; }
    if (mPdnMergePipe) { vkDestroyPipeline(mCtx->device(), mPdnMergePipe, nullptr); mPdnMergePipe = VK_NULL_HANDLE; }
    if (mPdnSampleCorePipe) { vkDestroyPipeline(mCtx->device(), mPdnSampleCorePipe, nullptr); mPdnSampleCorePipe = VK_NULL_HANDLE; }
    if (mPdnMergeCorePipe) { vkDestroyPipeline(mCtx->device(), mPdnMergeCorePipe, nullptr); mPdnMergeCorePipe = VK_NULL_HANDLE; }
    for (VkPipeline* sp : {&mPdnSelectPipe[0], &mPdnSelectPipe[1], &mPdnSunVisPipe, &mPdnVisPipe, &mPdnShadePipe,
                           &mPdnSunVisIsoPipe, &mPdnVisIsoPipe, &mPdnMaskPipe,   // (#1031; the adaptive mask)
                           &mPdnVisLvcPipe, &mLvcPipe,                            // (LVC stages 0 / 3)
                           &mPdnSelectHkPipe[0], &mPdnSelectHkPipe[1]})          // (K > 4)
        if (*sp) { vkDestroyPipeline(mCtx->device(), *sp, nullptr); *sp = VK_NULL_HANDLE; }
    // (views of the RT lighting scratch: giDestroy below frees the buffer itself)
    mPdnSel = mPdnVis = GpuBuffer{};
    mPdnSelBytes = mPdnVisBytes = mPdnSelOff = mPdnVisOff = 0u;
    mPdnSelW = mPdnSelH = mPdnVisSlots = 0;
    mPdnVisExact = false;
    mPdnVisTiles = false;
    if (mPdnAdaptRb.buffer) { retireBuffer(mPdnAdaptRb); mPdnAdaptRb = GpuBuffer{}; }
    std::fill(std::begin(mPdnAdaptRbUsed), std::end(mPdnAdaptRbUsed), false);
    sunCertDestroy();
    if (mRsEmDiagRb.buffer) { retireBuffer(mRsEmDiagRb); mRsEmDiagRb = GpuBuffer{}; }
    std::fill(std::begin(mRsEmDiagUsed), std::end(mRsEmDiagUsed), false);
    giDestroy();   // (#87)
    if (mRsEmPipe) { vkDestroyPipeline(mCtx->device(), mRsEmPipe, nullptr); mRsEmPipe = VK_NULL_HANDLE; }
    mShadowMergeTry = mShadowMergeNow = false;
    if (mPdn)
    {
        mPdn->destroy();
        delete mPdn;
        mPdn = nullptr;
    }
    mPdnNow = false;
    mPdnFlags = 0u;
    mPdnReq = PdnReq{};
    mPdnStressPhase = -1;
    mRsResSampled = false;
}

// Set 3 bindings 13-16: the denoiser's IN / GEOM / MOT / OUT (its extent or its placeholders).
// (#869 Phase 5b: + binding 17, the reservoirs -- freed with the denoiser's images.)
void LLCleanRendererVK::writePdnBindings()
{
    if (!mDsetDeferSplit || !mPdn || !mCtx) return;
    if (!mPdn->allocated() && mRsRes.buffer)
    {   // (the callers waited for the device: the images were just released)
        mCtx->destroyBuffer(mRsRes);
        mRsRes = GpuBuffer{};
        mRsResW = mRsResH = 0;
        mRsResPrevOk = false;
        writeRsResBinding();
    }
    if (!mPdn->allocated() && (mPdnSel.buffer || mPdnVis.buffer))
        pdnSelFree();   // (#1031: the split core's descriptors + visibilities go with them)
    const VkImageView v[4] = {mPdn->inView(), mPdn->geomView(), mPdn->motView(), mPdn->outView()};
    VkDescriptorImageInfo ii[4]{};
    VkWriteDescriptorSet w[4]{};
    for (uint32_t i = 0; i < 4; ++i)
    {
        ii[i] = {VK_NULL_HANDLE, v[i], VK_IMAGE_LAYOUT_GENERAL};
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = mDsetDeferSplit;
        w[i].dstBinding = kDsBindingPdnIn + i; w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(mCtx->device(), 4, w, 0, nullptr);
    writeGiBindings();   // (#87: 18-23 -- and their fallback views follow this denoiser's)
}

// The motion vectors for the denoiser's reprojection (rides on writeRtMaskMotionDescriptor:
// same callers, same "no frame in flight" guarantee).
void LLCleanRendererVK::writePdnMotion()
{
    if (mPdn)
    {
        // (#1608: the 2D_ARRAY view -- binding 5 is a sampler2DArray, a layer per view)
        if (mMotion.view && mCtx->addArrayView(mMotion, VK_IMAGE_ASPECT_COLOR_BIT))
            mPdn->setMotion(mMotion.arrayView, mDepthSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        else
            mPdn->setMotion(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL);
    }
    writeGiMotion();   // (#87: the GI denoiser reprojects with the same motion vectors; #1038 + the GI trace)
}

// Once per split frame (deferSplitFrame, after the split targets): the settings, the denoiser's
// images (held / released / stepped down), and whether this frame samples (mPdnNow + the
// DEFER_FLAG_PSTOCH bits the caller ORs into the split flags). sample = false (a frame without the
// split that the merged RT shadow pass wants): the images only, mPdnNow stays false.
double LLCleanRendererVK::leanHistHold() const
{
    F32 hold = 0.f;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<F32> s_hold(gSavedSettings, "RenderVKLeanHistHoldSecs", 0.f);
    hold = s_hold;
#endif
    return std::isfinite(hold) ? std::clamp((double)hold, 0.0, 10.0) : 0.0;
}

void LLCleanRendererVK::pdnFrame(uint32_t rw, uint32_t rh, bool sample)
{
    mPdnNow = false;
    mPdnFlags = 0u;
    U32 mode = 0, k = 4, dbg = 0, atrous = 3, splitK = 2, passes = 1, adapt = 0, adaptCap = 0, adaptHist = 0;
    U32 adaptRule = 0, visCost = 0, adaptK = 0;
    F32 scale = 0.5f;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_mode(gSavedSettings, "RenderVKProjStochastic", 1U);
    static LLCachedControl<U32> s_k(gSavedSettings, "RenderVKProjStochasticSamples", 4U);
    static LLCachedControl<F32> s_scale(gSavedSettings, "RenderVKRTShadowMaskScale", 0.5f);   // (the one RT shadow resolution)
    static LLCachedControl<U32> s_dbg(gSavedSettings, "RenderVKDenoiseDebug", 0U);
    static LLCachedControl<U32> s_atrous(gSavedSettings, "RenderVKDenoiseAtrous", 3U);
    static LLCachedControl<U32> s_split(gSavedSettings, "RenderVKProjStochasticSplitK", 2U);
    mode = s_mode; k = s_k; scale = s_scale; dbg = s_dbg; atrous = s_atrous; splitK = s_split;
    static LLCachedControl<U32> s_passes(gSavedSettings, "RenderVKProjStochasticPasses", 1U);
    passes = s_passes;
    static LLCachedControl<U32> s_adapt(gSavedSettings, "RenderVKProjStochasticAdaptive", 0U);
    static LLCachedControl<U32> s_adaptCap(gSavedSettings, "RenderVKProjStochasticAdaptiveCap", 0U);
    static LLCachedControl<U32> s_adaptHist(gSavedSettings, "RenderVKProjStochasticAdaptiveHistory", 0U);
    adapt = s_adapt; adaptCap = s_adaptCap; adaptHist = s_adaptHist;
    static LLCachedControl<U32> s_adaptRule(gSavedSettings, "RenderVKProjStochasticAdaptiveRule", 0U);
    static LLCachedControl<U32> s_visCost(gSavedSettings, "RenderVKProjStochVisCost", 0U);
    adaptRule = s_adaptRule; visCost = s_visCost;
    static LLCachedControl<U32> s_adaptK(gSavedSettings, "RenderVKProjStochasticAdaptiveSamples", 0U);
    adaptK = s_adaptK;
#endif
    // (RenderVKProjStochasticAdaptiveSamples: 0 = the extra passes take RenderVKProjStochasticSamples, else their own K)
    mPdnKx = std::min<U32>(adaptK, kPdnKMax);
    mPdnAdaptRule = adaptRule != 0u ? 1u : 0u;
    mPdnVisCost = std::min<U32>(visCost, 2u);
    scale = rtSignalScale(scale);   // (RenderVKDlssFullResSignalsMode)
    mPdnWant = mode != 0u;
    // (1-16: K > 2 adds picks 2-3 in the selection descriptor's extension, K > 4 picks 4..15 in the second one --
    //  pdnSelBytesWanted; K > 4 runs the high-K SELECT, pdnCtlWord)
    mPdnK = std::clamp<U32>(k, 1u, kPdnKMax);
    // (RenderVKProjStochasticPasses, debug: N passes of the K picks a frame -- recordProjStochPasses)
    mPdnPasses = std::clamp<U32>(passes, 1u, 8u);
    // (RenderVKProjStochasticAdaptive: 0 off, 1-7 extra passes on the flagged tiles -- the pass index's 3 bits; the cap a
    //  percentage of the tiles, 0 = none -- recordProjStochPasses)
    mPdnAdapt = std::min<U32>(adapt, 7u);
    mPdnAdaptCap = std::min<U32>(adaptCap, 100u);
    mPdnAdaptHist = adaptHist != 0u;
    // (7: the ReSTIR view, #869; 8 / 9: the split lights by id / x their visibility -- the denoiser
    //  writes those into layer 0, the lit pass shows layer 0 as view 4)
    mPdnDebug = dbg <= 14u ? dbg : 0u;   // (13: the light-drop view, clean_dn_temporal.comp PDN_DBG_DROP; 14 the mover-exact set)
    // (#869) THE SPLIT (RenderVKProjStochasticSplitK, live, 0..4): the K dominant lights' own
    // visibility (clean_pdn_common.glsl SPLIT CHANNEL); its images only while K > 0
    mPdnSplitK = std::min<U32>(splitK, 4u);
    const bool split = mPdnSplitK > 0u || (mPdnRadiance && mPdnExactSpecular);
    // Under DLSS Ray Reconstruction one a-trous iteration (owner A/B 2026-11-09: 1 looks the same as 3 there,
    // the network does the wide filtering; ~0.65 ms at 1280x696 on a 5090). RenderVKDenoiseAtrous is the
    // quality setting for every other frame.
    if (dlssRrFrame()) atrous = 1u;
    mPdnAtrous = LLCleanDenoiserVK::clampIters(atrous);
    if (!mPdn) return;
    // This frame can sample: the setting, the kernels, ray-traced projector records, a valid TLAS.
    // (#869 / #439 ReSTIR DI: a scene whose only shadowed lights are point lights / record-less
    //  projectors samples too -- once this recording's light pass built the ReSTIR population,
    //  mRsNow: without it pass 15 would have nothing to trace)
    // (#1608 STEREO: pass 15, its lists and the denoiser's images are per view -- a layered stereo split frame samples
    //  as the desktop's does, unless SlipstreamVRPointShadows picked the layered RT shadow mask's cheaper local lights:
    //  vrStochShadows)
    const bool can = sample && vrStochShadows() && mPdnWant && mPdnSamplePipe && mRtReflLayout && mPdn->pipelinesReady() && mRT
                  && mRT->tlasValid() && mRT->consumerSet() != VK_NULL_HANDLE && mProjMode == 2u
                  && (mProjRecords > 0u || mRsNow) && mDsetDraw0;
    const double now = pdnClock();
    if (can) mPdnLastWant = now;
    // (#195 + #85) THE SUN / MOON CHANNEL: while the merged RT shadow pass wants it (+ the same
    // hold); a refusal of it backs off 30 s with the projector-only images (today's paths).
    const bool sunWant = shadowMergeWanted(false) && now >= mPdnSunRetryAt;
    if (sunWant) mPdnSunLastWant = mPdnLastWant = now;
    bool sun = sunWant || (mPdn->hasSun() && !mTexPressure && now - mPdnSunLastWant < kRtReflHoldSecs
                           && now >= mPdnSunRetryAt);
    // Held past the last sampling frame (projectors drifting out of view and back must not
    // re-allocate every time); freed at once under texture pressure or with the setting off.
    const bool keep = can || sunWant || (mPdnWant && !mTexPressure && now - mPdnLastWant < kRtReflHoldSecs);
    if (!keep)
    {
        mPdnStressPhase = -1;
        if (mPdn->allocated()) pdnRequestRelease();   // (applied before the next recording: pdnPrestep)
        return;
    }
    // REALLOCATION STRESS (RenderVKDenoiseReallocStress, seconds; 0 = off): the sun channel is
    // flipped every N seconds while the images are held, so the denoiser re-allocates on demand
    // through exactly the request path below (pdnPrestep) -- the Beta 5 Windows DEVICE_LOST
    // trigger, reproducible for a soak test (Vulkan validation on Linux; Windows NVIDIA).
    // RenderVKDenoiseReallocStressMode picks what flips: 0 the sun / moon channel (above), 1 the EXTENT full <-> half (the
    // VRAM ladder's step; the 2026-11-21 DEVICE_LOST: a 1920x1044 -> 3840x2088 re-create under DLSS RR left the RR guide
    // set naming the freed MOT through a recycled view handle), 2 the HISTORY lean <-> full (a history-only re-create:
    // MOT and the consumers' views must stay -- THE LEAN HISTORY, the lean hold). Modes 0 / 1 re-create MOT every flip.
    bool stressExtent = false, stressHist = false;
    float want_sc = scale > 0.75f ? 1.f : 0.5f;   // (full only at 1, else half: the autotune's 1 -> 0.75 rung halves it)
    {
        F32 stress = 0.f;
        U32 stressMode = 0u;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<F32> s_stress(gSavedSettings, "RenderVKDenoiseReallocStress", 0.f);
        static LLCachedControl<U32> s_stressMode(gSavedSettings, "RenderVKDenoiseReallocStressMode", 0U);
        stress = s_stress;
        stressMode = s_stressMode;
#endif
        if (stress > 0.f && std::isfinite(stress))
        {
            const int64_t phase = (int64_t)(now / std::max(stress, 0.25f));
            const bool flip = (phase & 1) != 0;
            if (stressMode == 1u) stressExtent = flip;
            else if (stressMode == 2u) stressHist = flip;
            else sun = sun != flip;
            if (stressExtent) want_sc = want_sc > 0.75f ? 0.5f : 1.f;
            if (phase != mPdnStressPhase)
            {
                if (mPdnStressPhase >= 0)
                    LL_INFOS("Vulkan") << "Projector denoiser STRESS (RenderVKDenoiseReallocStress " << stress
                                       << " s, mode " << stressMode << "): forcing "
                                       << (stressMode == 1u ? (want_sc > 0.75f ? "the FULL extent" : "the HALF extent")
                                           : stressMode == 2u ? (flip ? "the history lean <-> full flip" : "the history as decided")
                                           : (sun ? "the sun / moon channel ON" : "the sun / moon channel OFF"))
                                       << " -> re-allocation " << (resizeAtFrameStart() ? "before the next recording"
                                                                                      : "mid-recording (old path)")
                                       << LL_ENDL;
                mPdnStressPhase = phase;
            }
        }
        else
            mPdnStressPhase = -1;
    }
    const uint32_t w0 = pdnDim(rw, want_sc), h0 = pdnDim(rh, want_sc);
    // (THE LEAN HISTORY, llcleandenoisevk.h) DLSS RR has taken the projector channels / the merged sun raw for the
    // dwell (rrRawSignal: never the radiance-mode projectors): their history is left out -- nothing reads it then
    uint32_t hist = LLCleanDenoiserVK::HIST_FULL;
    {
        const uint32_t hm = mPdn->allocated() ? mPdn->histMode() : (uint32_t)LLCleanDenoiserVK::HIST_FULL;
        const double hold = leanHistHold();
        const bool leanP = leanHistDue(rrRawSignal(kRrRawProj, w0, h0), mPdnRawSince, mPdnRawOff,
                                       (hm & LLCleanDenoiserVK::HIST_NO_COLOUR) != 0u, now, hold);
        // (the sun's history is out only while the images carry the channel: a sun switched off is no gap to bridge)
        const bool leanS = leanHistDue(sun && rrRawSignal(kRrRawMergedSun, w0, h0), mPdnSunRawSince, mPdnSunRawOff,
                                       sun && mPdn->hasSun() && (hm & LLCleanDenoiserVK::HIST_NO_SUN) != 0u, now, hold);
        hist = (leanP ? (uint32_t)LLCleanDenoiserVK::HIST_NO_COLOUR : 0u) | (leanS ? (uint32_t)LLCleanDenoiserVK::HIST_NO_SUN : 0u);
    }
    // (RenderVKDenoiseReallocStressMode 2: the colour history flipped -- a history-only re-create every flip; a lean
    //  frame that is not raw runs without that history, as the lean hold's frames do)
    if (stressHist) hist ^= (uint32_t)LLCleanDenoiserVK::HIST_NO_COLOUR;
    // (a history-only change on the current images: requested like an extent change, this frame keeps them -- lean
    //  images on a frame that is not raw run without that history, clean_dn_common.glsl dnColourHist / dnSunHistImg)
    if (mPdn->allocated() && mPdn->width() == w0 && mPdn->height() == h0 && mPdn->hasSun() == sun
        && mPdn->hasSplit() == split && mPdn->histMode() != hist && now >= mPdnRetryAt && resizeAtFrameStart())
        pdnRequestEnsure(w0, h0, sun, sun, split, hist);
    if (!mPdn->allocated() || mPdn->width() != w0 || mPdn->height() != h0 || mPdn->hasSun() != sun
        || mPdn->hasSplit() != split)
    {
        // Already at a stepped-down size for this request: keep it until the retry time.
        const bool stepped = mPdn->allocated() && mPdn->hasSun() == sun && mPdn->hasSplit() == split && want_sc > 0.5f
                          && mPdn->width() == pdnDim(rw, 0.5f) && mPdn->height() == pdnDim(rh, 0.5f);
        if (now < mPdnRetryAt && !stepped) return;   // (refused recently: the exact path meanwhile)
        if (now >= mPdnRetryAt)
        {
            // VRAM PRE-CHECK (#226 / #697): the requested extent, else half, else none (the exact path).
            // With the sun channel wanted: those two with it first, then without it (the merge
            // waits; the projector path keeps its images).
            struct Opt { float s; bool sun; };
            std::vector<Opt> opts;
            std::vector<VramCand> cands;
            for (int pass = 0; pass < (sun ? 2 : 1); ++pass)
            {
                const bool osun = sun && pass == 0;
                float last = 2.f;
                for (float s : {want_sc, 0.5f})
                {
                    if (s >= last) continue;
                    last = s;
                    opts.push_back(Opt{s, osun});
                    cands.push_back(VramCand{std::string(s > 0.75f ? "full" : "half") + (osun ? " + sun" : "") + " ("
                                                 + std::to_string(pdnDim(rw, s)) + "x" + std::to_string(pdnDim(rh, s)) + ")",
                                             mPdn->bytesAt(pdnDim(rw, s), pdnDim(rh, s), osun, split, hist)});
                }
            }
            // The current allocation (a stepped-down half extent) needs nothing new: vramPick's keep
            // index, so a re-allocation it defers or refuses never drops a working set.
            int keep = -1;
            for (size_t i = 0; i < opts.size(); ++i)
                if (mPdn->allocated() && mPdn->width() == pdnDim(rw, opts[i].s) && mPdn->height() == pdnDim(rh, opts[i].s)
                    && mPdn->hasSun() == opts[i].sun && mPdn->hasSplit() == split)
                    keep = (int)i;
            VramRoom room;
            const int pick = vramPick("projector denoiser", cands, mPdn->bytes(), keep, &room);
            if (pick < 0)
            {
                mPdnRetryAt = now + 30.0;
                if (sun) mPdnSunRetryAt = now + 30.0;
                if (mPdn->allocated()) pdnRequestRelease();
                return;
            }
            const Opt o = opts[(size_t)pick];
            const uint32_t w = pdnDim(rw, o.s), h = pdnDim(rh, o.s);
            if (sun && !o.sun) mPdnSunRetryAt = now + 30.0;   // (the sun channel did not fit: later)
            if (o.s < want_sc) mPdnRetryAt = now + 30.0;   // (re-try the requested extent later)
            if (!mPdn->allocated() || mPdn->width() != w || mPdn->height() != h || mPdn->hasSun() != o.sun
                || mPdn->hasSplit() != split)
            {
                // THE DEVICE_LOST FIX (Beta 5.1): never re-create the images here. This runs inside
                // the frame's recording (deferSplitFrame, after the front end / culls / RT build and
                // the graphics work before pass 1 are recorded); ensure() waits for the device,
                // destroys the images, re-creates them and rewrites the denoiser's set + set 3 b13-16
                // -- vkDeviceWaitIdle does not cover the recorded-but-unsubmitted commands of this
                // frame, and none of these sets is UPDATE_AFTER_BIND. The request is applied by
                // pdnPrestep before the next frame's command buffer begins (#521's rule).
                pdnRequestEnsure(w, h, o.sun, sun, split, hist);
                if (resizeAtFrameStart())
                {
                    // This frame: the current images while they fit the extent (only the sun / split
                    // channel differs: the projector path does not read the sun's, the merge is gated
                    // on hasSun(), and pass 15 splits only into images that carry the split layers --
                    // it writes empty slots into images that carry them while K = 0); a different
                    // extent -- or none yet -- takes the exact path this frame.
                    if (!mPdn->allocated() || mPdn->width() != w || mPdn->height() != h) return;
                }
                else
                {
                    // RenderVKResizeAtFrameStart off: the old mid-recording re-allocation (A/B only).
                    pdnPrestep();
                    if (!mPdn->allocated()) return;
                }
            }
        }
    }
    if (!can || !mPdn->allocated())   // (held, not sampled this frame)
    {
        mRsResPrevOk = false;   // (#869: the reservoirs' history is a frame old at most)
        return;
    }
    mPdnNow = true;
    mPdn->beginFrame();
    ++mPdnFrame;
    // (#869 Phase 5b: the reservoirs at this extent -- re-created before the next recording, like
    // the denoiser's images (syncFromStoresPrestep: rsResFrame writes set 3 b17 behind a device
    // wait); recordProjStoch uses them only once they match this extent)
    mRsResSampled = true;
    if (!resizeAtFrameStart()) { rsResFrame(); pdnSelFrame(); }
    // clean_deferred_light.comp DEFER_FLAG_PSTOCH (18) / _HALF (19) / _IN (20) / debug (21-23).
    const bool half = mPdn->width() < rw || mPdn->height() < rh;
    // (the lit pass's debug field is 3 bits: split views 8 / 9 and receiver view 10 show layer 0 --
    //  view 4 -- into which the denoiser's last a-trous iteration writes them)
    const uint32_t litDbg = mPdnDebug >= 8u ? 4u : mPdnDebug;
    mPdnFlags = (1u << 18) | (half ? (1u << 19) : 0u) | (LLCleanDenoiserVK::finalIsIn(mPdnAtrous) ? (1u << 20) : 0u)
              | (litDbg << 21);
}

// Frames without the split (uber / forward / split refused): nothing samples; the images are
// released once the hold ran out (or at once under texture pressure).
void LLCleanRendererVK::pdnIdle()
{
    if (!mPdn || !mPdn->allocated()) return;
    if (!mTexPressure && pdnClock() - mPdnLastWant < kRtReflHoldSecs) return;
    mPdnStressPhase = -1;
    pdnRequestRelease();   // (applied before the next recording: pdnPrestep)
}

// ---- DEFERRED (RE)ALLOCATION (the Beta 5 Windows NVIDIA DEVICE_LOST, 2026-10-22) ----
// pdnFrame / pdnIdle run INSIDE the frame's recording; they only record what the images should
// be. pdnPrestep (syncFromStoresPrestep, before the frame's command buffer begins -- the place
// resizeProbesIfNeeded moved to for #521) applies it: the device wait there covers every
// submitted frame and nothing of the next one is recorded yet, so destroying the images and
// rewriting the denoiser's set + set 3 b13-16 cannot touch a command buffer that names them.
bool LLCleanRendererVK::resizeAtFrameStart() const
{
    bool on = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKResizeAtFrameStart", true);
    on = s_on;
#endif
    return on;
}

void LLCleanRendererVK::pdnRequestEnsure(uint32_t w, uint32_t h, bool sun, bool sunWanted, bool split, uint32_t hist)
{
    mPdnReq.pending = true;
    mPdnReq.release = false;
    mPdnReq.w = w;
    mPdnReq.h = h;
    mPdnReq.sun = sun;
    mPdnReq.sunWanted = sunWanted;
    mPdnReq.split = split;
    mPdnReq.hist = hist;
}

void LLCleanRendererVK::pdnRequestRelease()
{
    mPdnReq = PdnReq{};
    mPdnReq.pending = true;
    mPdnReq.release = true;
    if (!resizeAtFrameStart()) pdnPrestep();   // (RenderVKResizeAtFrameStart off: the old path, A/B only)
}

void LLCleanRendererVK::pdnPrestep()
{
    if (!mPdnReq.pending) return;
    const PdnReq r = mPdnReq;
    mPdnReq = PdnReq{};
    if (!mPdn || !mCtx) return;
    if (r.release)
    {
        if (!mPdn->allocated()) return;
        mPdn->release();   // (waits for the device)
        writePdnBindings();
        return;
    }
    const double now = pdnClock();
    if (mPdn->allocated() && mPdn->width() == r.w && mPdn->height() == r.h && mPdn->hasSun() == r.sun
        && mPdn->hasSplit() == r.split)
    {
        // (THE LEAN HISTORY: a history-only change re-creates HIST / SUNHIST alone -- set 3's views stay; a refused
        //  growth runs without that history and retries in 30 s)
        if (mPdn->histMode() != r.hist && !mPdn->ensure(r.w, r.h, r.sun, r.split, r.hist)) mPdnRetryAt = now + 30.0;
        return;
    }
    bool ok = mPdn->ensure(r.w, r.h, r.sun, r.split, r.hist);   // (soft; waits for the device)
    if (!ok && r.sun)
    {
        // the sun channel refused after the pre-check: the projector-only images now, the sun
        // channel later (today's paths meanwhile)
        mPdnSunRetryAt = now + 30.0;
        ok = mPdn->ensure(r.w, r.h, false, r.split, r.hist);
    }
    writePdnBindings();
    if (!ok)
    {
        mPdnRetryAt = now + 30.0;
        if (r.sunWanted) mPdnSunRetryAt = now + 30.0;
    }
}

// ======================= THE LIGHT PREP =======================
// The lighting's head that reads nothing the lighting's other inputs are made of: the SUN EXPOSURE CERTIFICATES
// (C: the CPU's sun state + its own table, a push-only kernel), the ReSTIR EMISSIVE BUILD (E: the instance store,
// the RT geometry rows of this frame's RT build, the materials / bindless textures, the light table and the grid
// header of the front end -- no G-buffer, no depth, no screen-space data; the draw view only for its time), the
// OCCLUSION RESOLVE (O: last frame's notes + this frame's light remap in the grid header) and (#1039) the GI
// CHANGE TAGS' bits 0-2 (stage 9: last frame's reservoirs + this frame's GI header and light table). recordWorld
// plans + records it BEFORE pass 1a: its own async segment q_light_prep (after the RT build on the async queue,
// beside the pre-pass / G-buffer / shadow views on graphics), or inline on graphics there. The lighting chain
// later on the same queue reads it through the closing barrier below (queue order), a graphics lighting through
// its join (asyncJoinUpTo before recordDeferLight). Pass 15, the lit kernels and the GI read the results;
// recordProjStoch reads the plan.
bool LLCleanRendererVK::lightPrepPlan(uint32_t rw, uint32_t rh)
{
    LightPrep& lp = mLightPrep;
    lp = LightPrep{};
    lp.planned = true;
    lightPrepPlanShadow();
    // the GI split this frame (recordGI's own test; giTraceReady: this frame's TLAS)
    const bool giSplit = mGiSplitNow && giTraceReady() && giSplitReady(mGiDn->width(), mGiDn->height());
    // (#1039) the GI CHANGE TAGS: on the frames whose header has changed lights + reservoirs (mGiHdrChg) and whose
    // GI runs the split (REUSE reads the tags)
    lp.chg = giSplit && mGiHdrChg && mGiSplitPipe[GIS_CHGTAG];
    // (#1036) the GI shade split's HIT / MISS list headers (recordGI's shadeSplit) and (#827) the feature split's
    // tile list header (recordDeferredLighting's feat)
    lp.giLists = giSplit && mGiShadeSplitNow && giShadeSplitReady();
    lp.dsTiles = deferFeatFrame(rw, rh);
    // (#1039) the visibility queue's region headers (visQueueDecide ran: the regions and the buffer are this recording's)
    lp.vq = visQueuePrepMask();
    return lp.sxc || lp.em || lp.occ || lp.split || lp.chg || lp.giLists || lp.dsTiles || lp.vq != 0u;
}

// The split core's test (lightPrepPlanShadow; moverRouteDecide asks it before the plan, for RenderVKShadowMoverMask 2):
// the generic core runs (a ReSTIR / split frame or RenderVKShadowGenericCore) and the descriptors + visibility slots
// fit this extent and the settings' longest sample list.
bool LLCleanRendererVK::pdnSplitPlanned() const
{
    if (!mPdnNow || !mPdn || !mPdnSamplePipe || !mRtReflLayout || !mRT || mRT->consumerSet() == VK_NULL_HANDLE)
        return false;
    U32 core = 0;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_core(gSavedSettings, "RenderVKShadowGenericCore", 1U);
    core = s_core;
#endif
    const uint32_t splitK = mPdn->hasSplit() ? std::max(mPdnSplitK, mPdnExactSpecular ? 1u : 0u) : 0u;
    if (!(core != 0u || mRsNow || splitK != 0u)) return false;
    return pdnSplitWanted() && mPdnSel.buffer && mPdnVis.buffer && mPdnSelW == mPdn->width() && mPdnSelH == mPdn->height()
        && mPdnVisSlots >= pdnVisSlotsWanted() && mPdnSelBytes >= pdnSelBytesWanted(mPdnSelW, mPdnSelH);
}

void LLCleanRendererVK::lightPrepPlanShadow()
{
    LightPrep& lp = mLightPrep;
    lp.pdn = lp.merged = lp.sunSkip = lp.split = lp.exact = lp.sxc = lp.em = lp.occ = false;
    lp.pipe = VK_NULL_HANDLE;
    if (!mPdnNow || !mPdn || !mPdnSamplePipe || !mRtReflLayout || !mRT || mRT->consumerSet() == VK_NULL_HANDLE)
        return;
    lp.pdn = true;
    U32 core = 0;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_core(gSavedSettings, "RenderVKShadowGenericCore", 1U);
    core = s_core;
#endif
    // (#195 + #85) merged RT shadows: the same pass + the sun / moon trace (recordWorld's settle
    // checked the pipeline and the denoiser's sun images).
    lp.merged = mShadowMergeNow && mPdnMergePipe && mPdn->hasSun();
    // SUN-CHANNEL SKIP: the weak-sun gate traces no sun / moon ray this frame (weight 0: every
    // texel's raw sun is exactly lit), so the denoiser skips the sun channel's temporal + a-trous
    // work and the SHADOW kernel reads lit without it (frame info rt_mask.x = -2, rtMaskFrameInfo).
    // Its history is not written: the next traced frame restarts it (DN_F_SUN_HIST off) while
    // the weight ramps in from 0 (the shadow fades in with the light, never pops).
    lp.sunSkip = lp.merged && !(mShadowMergeSunW > 0.f);
    const uint32_t splitK = mPdn->hasSplit() ? std::max(mPdnSplitK, mPdnExactSpecular ? 1u : 0u) : 0u;
    lp.pipe = lp.merged ? mPdnMergePipe : mPdnSamplePipe;
    // (#869 Phase 5) a ReSTIR frame runs the generic core (its candidates, reservoirs and light
    // samples live there; rsWanted made sure both core kernels exist)
    // (#869 SPLIT: only the generic core splits -- the per-light-type kernel writes empty slots)
    // (#1031) ... as SELECT -> SUNVIS + VIS -> SHADE (RenderVKShadowSplitKernels) while the descriptors and
    // the visibility slots fit this extent and the settings' longest sample list
    // (one decision per frame: the certificates below and pass 15 take the same kernel even if a core
    //  pipeline finishes compiling in between)
    if (core != 0u || mRsNow || splitK != 0u)
    {
        const VkPipeline cp = lp.merged ? mPdnMergeCorePipe : mPdnSampleCorePipe;
        if (cp != VK_NULL_HANDLE) lp.pipe = cp;   // (still compiling: the per-light-type kernel this frame)
        lp.split = pdnSplitPlanned();
    }
    // (RenderVKShadowMoverMask 2) the exact selection runs where moverRouteDecide confirmed it -- the same split test,
    // made earlier in the same frame: they agree (never "the mask skipped its local mover passes, pass 15 is not exact")
    lp.exact = lp.split && mMoverExactNow && mPdnVisExact;
    if (mMoverExactNow && !lp.exact) LLVKHitch::count("moverExactLate", 1u);
    lp.sxc = lp.merged && !lp.sunSkip;
    lp.em = mRsEmNow && mRsNow && mRsEmPipe && mRegir.buffer;
    lp.occ = mRsOccNow && mRsNow && mRegir.buffer;
}

// The planned work not recorded yet, on cmd (once per frame). viewIdx / viewOff: the main view's draw-view entry
// (the emissive build reads its time: texture animation).
void LLCleanRendererVK::recordLightPrep(VkCommandBuffer cmd, uint32_t viewIdx, uint32_t viewOff, bool onAsync, bool early)
{
    LightPrep& lp = mLightPrep;
    const bool shadow = lp.pdn && !lp.shadowDone && (lp.sxc || lp.em || lp.occ || lp.split);
    const bool chg = lp.chg && !lp.chgDone;
    const bool hdrs = early && !lp.headersDone && (lp.dsTiles || lp.giLists || lp.vq != 0u);
    if (!shadow && !chg && !hdrs) return;
    lp.shadowDone = lp.shadowDone || shadow;
    lp.chgDone = lp.chgDone || chg;
    lp.headersDone = lp.headersDone || hdrs;
    const bool sxc = shadow && lp.sxc, em = shadow && lp.em, occ = shadow && lp.occ;
    auto memBarrier = [cmd](VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da)
    {
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = ss; b.srcAccessMask = sa; b.dstStageMask = ds; b.dstAccessMask = da;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1; di.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &di);
    };
    constexpr VkAccessFlags2 kRW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    // OPENING BARRIER (one for C, E, O and the tags -- each had its own WAR barrier): RAW from this frame's RT build
    // (the geometry rows E reads; on the async queue before this segment, or on graphics before the fork: the
    // semaphore) and the front end's header uploads, and WAR against every earlier reader of their regions on this
    // queue (last frame's pass 15, lit kernels, GI trace / REUSE, indirect reads, stats copies; last frame's
    // q_gi_lag). Last frame's graphics-side readers (the #978 combine) are behind the fork's semaphore.
    memBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
               VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT
               | VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    // THE CHAIN'S LIST HEADERS (each was a COMPUTE -> TRANSFER -> COMPUTE drain pair inside the lighting chain):
    // reset here, after the opening barrier (WAR: last frame's appends + indirect reads) and published by the
    // closing barrier (the appends' atomics + the indirect dispatches). Nothing between here and the chain
    // touches them.
    if (shadow && lp.split)
    {   // pass 15's VIS job list: count 0, (0, 1, 1) workgroups -- SELECT appends, VIS dispatches indirect over it
        static const uint32_t kHdr[4] = {0u, 0u, 1u, 1u};
        {
            LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_light_prep_setup");
            vkCmdUpdateBuffer(cmd, mPdnVis.buffer, mPdnVisOff, sizeof(kHdr), kHdr);
        }   // (its range of the RT lighting scratch)
        // (RenderVKShadowMoverMask 2) THE MOVER-EXACT JOB LIST's header the same way: SELECT appends, the second VIS
        // dispatch runs indirect over it
        if (lp.exact) {
            LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_light_prep_setup");
            vkCmdUpdateBuffer(cmd, mPdnVis.buffer, mPdnVisOff + pdnExactWordOff(), sizeof(kHdr), kHdr);
        }
    }
    if (hdrs && lp.dsTiles)
    {   // (#827) the feature split's tile list: VkDispatchIndirectCommand (0, 1, 1) + pad -- CLASSIFY counts x up,
        // BASE / LOCAL dispatch indirect over it
        static const uint32_t kHdr[4] = {0u, 1u, 1u, 0u};
        {
            LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_light_prep_setup");
            vkCmdUpdateBuffer(cmd, mDsTileList.buffer, 0, sizeof(kHdr), kHdr);
        }
    }
    if (hdrs && lp.giLists)
    {   // (#1036) the GI shade split's HIT / MISS lists: count 0, dispatch (0, 0, 1) each -- TRACE appends, MISS /
        // HIT-MAT / HIT-LIGHT dispatch indirect over them
        static const uint32_t kHdr[8] = {0u, 0u, 0u, 1u, 0u, 0u, 0u, 1u};
        {
            LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_light_prep_setup");
            vkCmdUpdateBuffer(cmd, mGiHit.buffer, 16u, sizeof(kHdr), kHdr);
        }
    }
    // (#771 Phase 2) SUN EXPOSURE CERTIFICATES: the table's header + maintenance (its own pipeline); pass 15
    // finds the table through the push (rtr3.xy, mLightPrep.sxcAddr) and skips the sun sample where a
    // certificate proves it blocked. (#1031: the split core's TRACE reads it -- the generic core's rules)
    if (sxc) lp.sxcAddr = sunCertRecord(cmd, onAsync, lp.split || lp.pipe == mPdnMergeCorePipe);
    // (#1039) the visibility queue's region headers: counters 0, the walks' dispatches (0, 0, 1), the capacities --
    // the producers append to them, the walks dispatch indirect over them (llcleanvisqueuevk.cpp THE HEADERS IN THE
    // LIGHT PREP). The closing barrier below publishes the transfer writes.
    if (hdrs && lp.vq != 0u) visQueuePrepHeaders(cmd, lp.vq);
    if (em || occ || chg)
    {
        // (pass 15's / the GI's layout and sets: set 2 is never read by these kernels -- the plain one)
        const VkDescriptorSet sets[5] = {mDsetDraw0, mDsetDraw1, mDsetDefer, mDsetDeferSplit, mRT->consumerSet()};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mRtReflLayout, 0, 5, sets, 1, &viewOff);
    }
    // the build kernel's push: the draw view (its time) + rtr3.zw = the mode / the record parity
    VKRtReflPush rp{};
    rp.d.view = viewIdx;
    if (em)
    {
        // Bounded discovery/admission, fresh capped records, then bounded attempted
        // reach/count/scan/fill and short/parallel-long list reduction. CPU-owned identity
        // input is only read; all persistent cache/cell writes remain on the GPU.
        const bool cold = !mRsEmBuiltOnce;
        const uint32_t par = mRsEmParity;
        mRsEmParity ^= 1u;
        mRsEmBuiltOnce = true;   // (the next header can validate previous-build identities)
        // (WAR -- the hash's earlier readers: last build's GI trace / pass 15 / lit kernels; #1006 + last build's
        //  indirect read of mode 7's dispatch words, the stats copy of the cache header's per-build words and the
        //  last shift's scratch copy; the cells' last writers -> the shift's copy: the prep's opening barrier)
        {
            LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_light_prep_setup");
            vkCmdFillBuffer(cmd, mRegir.buffer, kRsEmHashOffset + (VkDeviceSize)par * kRsEmHashN * 8u,
                        (VkDeviceSize)kRsEmHashN * 8u, 0u);
        }
        {
            LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_light_prep_setup");
            vkCmdFillBuffer(cmd, mRegir.buffer, kRsEmEntOffset + (VkDeviceSize)par * kRsEmInstCap * kRsEmEntBytes,
                        (VkDeviceSize)kRsEmInstCap * kRsEmEntBytes, 0u);
        }
        if (cold)
        {
            {
                LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_light_prep_setup");
                vkCmdFillBuffer(cmd, mRegir.buffer, kRsEmBudgetOffset, kRsEmBudgetBytes, 0u);
            }
            {
                LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_light_prep_setup");
                vkCmdFillBuffer(cmd, mRegir.buffer, kRsEmXOffset, 16u * 8u, 0u);
            }
        }
        // Bounded counting sort: zero all cell counts, never grow the fixed pair array.
        {
            LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_light_prep_setup");
            vkCmdFillBuffer(cmd,mRegir.buffer,kRsEmLCellOffset,kRsEmLCellBytes,0u);
        }
        {
            LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_light_prep_setup");
            vkCmdFillBuffer(cmd,mRegir.buffer,kRsEmSigOffset+(VkDeviceSize)kRsEmNCellsAll*8u,(VkDeviceSize)kRsEmOvCells*8u,0u);
        }
        {
            LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_light_prep_setup");
            vkCmdFillBuffer(cmd,mRegir.buffer,kRsEmLongOffset,8u,0u);
        }
        {
            LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_light_prep_setup");
            vkCmdFillBuffer(cmd,mRegir.buffer,kRsEmFwdOffset,kRsEmFwdBytes,0xFFFFFFFFu);
        }
        {
            LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_light_prep_setup");
            vkCmdFillBuffer(cmd,mRegir.buffer,kRsEmCchFrameOffset,kRsEmCchFrameBytes,0u);
        }
        memBarrier(VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kRW);
        VKRtReflPush ep = rp;
        ep.rtr3[0] = (uint32_t)mRsEmInputGen.address;
        ep.rtr3[1] = (uint32_t)(mRsEmInputGen.address >> 32u);
        const uint32_t genRows = mInstanceCount;
        std::memcpy(&ep.rtr[0], &genRows, sizeof(genRows));
        ep.rtr3[2] = 13u; // fixed row discovery, no mesh triangles
        ep.rtr3[3] = par;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mRsEmPipe);
        vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ep), &ep);
        {
            GpuPassScope scope(*this, cmd, onAsync ? "c_rs_em_discover" : "g_rs_em_discover");
            vkCmdDispatch(cmd, 1u, 1u, 1u);
        }
        memBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kRW, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kRW);
        gpuSubMark(cmd, onAsync ? "c_rs_em_discover" : "g_rs_em_discover");
        ep.rtr3[2] = 17u;
        vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ep), &ep);
        {
            GpuPassScope scope(*this, cmd, onAsync ? "c_rs_em_admit" : "g_rs_em_admit");
            vkCmdDispatch(cmd, EM_BUDGET_OBJECTS / 64u, 1u, 1u);
        }
        memBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kRW, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kRW);
        gpuSubMark(cmd, onAsync ? "c_rs_em_admit" : "g_rs_em_admit");
        ep.rtr3[2] = 0u;   // mode 0: retained emitters only
        ep.rtr3[3] = par;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mRsEmPipe);
        vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ep), &ep);
        {
            GpuPassScope scope(*this, cmd, onAsync ? "c_rs_em_emitters" : "g_rs_em_emitters");
            vkCmdDispatch(cmd, 64u, 4u, 1u);
        }
        memBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kRW, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kRW);
        gpuSubMark(cmd, onAsync ? "c_rs_em_emitters" : "g_rs_em_emitters");   // (the clears + mode 0)
        ep.rtr3[2] = 1u;   // mode 1: the emitters' record blocks (one workgroup)
        vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ep), &ep);
        {
            GpuPassScope scope(*this, cmd, onAsync?"c_rs_em_blocks":"g_rs_em_blocks");
            vkCmdDispatch(cmd, 1u, 1u, 1u);
        }
        memBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kRW, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kRW);
        gpuSubMark(cmd,onAsync?"c_rs_em_blocks":"g_rs_em_blocks");
        ep.rtr3[2] = 2u;   // mode 2: the triangle records, one invocation per record (grid-stride)
        vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ep), &ep);
        {
            GpuPassScope scope(*this, cmd, onAsync?"c_rs_em_records":"g_rs_em_records");
            vkCmdDispatch(cmd, kRsEmCap / 64u, 1u, 1u);
        }
        memBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kRW, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kRW);
        gpuSubMark(cmd,onAsync?"c_rs_em_records":"g_rs_em_records");
        // Every stage has a count bound; all-level reach attempts <= 64 per record.
        const uint32_t modes[5]={3u,4u,5u,6u,7u};
        const uint32_t groups[5]={kRsEmCap,kRsEmNCellsAll/64u,1u,kRsEmCap,kRsEmNCellsAll/64u};
        static const char* const marks[2][5]={{"g_rs_em_count","g_rs_em_scan","g_rs_em_sums","g_rs_em_fill","g_rs_em_short"},
                                             {"c_rs_em_count","c_rs_em_scan","c_rs_em_sums","c_rs_em_fill","c_rs_em_short"}};
        for(uint32_t i=0u;i<5u;++i)
        {
            ep.rtr3[2]=modes[i];
            vkCmdPushConstants(cmd,mRtReflLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(ep),&ep);
            {
                GpuPassScope scope(*this, cmd, marks[onAsync?1:0][i]);
                vkCmdDispatch(cmd,groups[i],1u,1u);
            }
            memBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,kRW,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,kRW);
            gpuSubMark(cmd,marks[onAsync?1:0][i]);
        }
        ep.rtr3[2]=9u; // ONLY bounded long lists, never a record-group/whole-grid gather
        vkCmdPushConstants(cmd,mRtReflLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(ep),&ep);
        memBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,kRW,VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
        {
            GpuPassScope scope(*this, cmd, onAsync?"c_rs_em_long":"g_rs_em_long");
            vkCmdDispatchIndirect(cmd,mRegir.buffer,kRsEmCchArgsOffset);
        }
        memBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,kRW,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,kRW);
        gpuSubMark(cmd,onAsync?"c_rs_em_long":"g_rs_em_long");
        // (-> pass 15, the lit kernels, the GI: the prep's closing barrier)
    }
    if (occ)
    {
        // (#869 OCCLUSION CULL, clean_deferred_light.comp) the cache: zeroed when new / re-enabled / the
        // last light list's frame did not resolve it (its counts are carried by rs_remap, one list to the
        // next), its stats every frame, then THE RESOLVE (mode 8 of the emissive build kernel: one
        // invocation per patch) turns last frame's notes into this frame's counts -- before pass 15, so
        // pass 15 and the lit kernels read the same counts. Fixed region of mRegir (6.25 MiB).
        if (!mRsOccRemapOk) mRsOccClear = true;
        // (WAR -- last frame's resolve / pass 15 / lit kernels -> the fill: the prep's opening barrier)
        {
            LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_light_prep_setup");
            vkCmdFillBuffer(cmd, mRegir.buffer, kRegirOccOffset, mRsOccClear ? kRsOccBytes : kRsOccStatBytes, 0u);
        }
        if (mRsOccStatsNow) {
            LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_light_prep_setup");
            vkCmdFillBuffer(cmd, mRegir.buffer, kRegirLvcOffset, kRegirLvcBytes, 0u);
        }   // (LVC stage 0)
        memBarrier(VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kRW);
        mRsOccClear = false;
        mRsOccRan = true;
        {
            VKRtReflPush op = rp;
            op.rtr3[2] = 8u;   // mode 8: the occlusion resolve
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mRsEmPipe);
            vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(op), &op);
            {
                GpuPassScope scope(*this, cmd, onAsync ? "c_rs_occ" : "g_rs_occ");
                vkCmdDispatch(cmd, kRsOccCells / 64u, 1u, 1u);
            }
            gpuSubMark(cmd, onAsync ? "c_rs_occ" : "g_rs_occ");   // (-> pass 15 / the lit kernels: the closing barrier)
        }
    }
    if (chg)
    {
        // (#1039) the GI CHANGE TAGS' bits 0-2 (stage 9, clean_deferred_light.comp CHG-TAG): one lane per GI texel --
        // last frame's reservoir there (layer parity ^ 1: nothing writes it this frame) against this frame's changed
        // / hot lights (the GI header + the light table, front end) -> the texel's tag word (bit 3 clear; HIT-LIGHT /
        // SHADE set it for the fresh hit). REUSE reads them. Push: rtr2.xy = the GI extent, rtr2.w = its parity.
        const uint32_t gw = mGiDn->width(), gh = mGiDn->height();
        VKRtReflPush tp{};
        tp.d.view = viewIdx;
        tp.rtr2[0] = (float)gw;
        tp.rtr2[1] = (float)gh;
        tp.rtr2[3] = (float)mGiDn->parity();
        vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(tp), &tp);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mGiSplitPipe[GIS_CHGTAG]);
        {
            GpuPassScope scope(*this, cmd, onAsync ? "c_gi_chgtag" : "g_gi_chgtag");
            vkCmdDispatch(cmd, (gw + 7u) / 8u, (gh + 7u) / 8u, mViewCount);   // (#1608: z = the views)
        }
        gpuSubMark(cmd, onAsync ? "c_gi_chgtag" : "g_gi_chgtag");
    }
    // CLOSING BARRIER (one for C, E, O and the tags -- each had its own): their results -> pass 15 (SELECT /
    // SUNVIS: the counts, the cells, the certificates), the lit kernels, the GI hit lighting (+ HIT-LIGHT's /
    // SHADE's bit 3 onto the tag words) and REUSE, later on this queue.
    memBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
               kRW | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
}

// After the reflect passes (+ RT reflections), before the AO join / BASE / COMBINE; the split
// layout's sets + push are bound again when it returns (as recordRtReflect).
void LLCleanRendererVK::recordProjStoch(VkCommandBuffer cmd, const VKDeferPush& p, uint32_t viewOff, bool onAsync)
{
    if (!mPdnNow || !mPdn || !mPdnSamplePipe || !mRtReflLayout || !mRT) return;
    const VkDescriptorSet rtSet = mRT->consumerSet();
    if (rtSet == VK_NULL_HANDLE) return;
    // THE LIGHT PREP (C / E / O, the plan): recordWorld records it ahead of the lighting. Here only when that
    // point did not plan it (never expected: the same gate) -- inline, where it ran before the move.
    if (!mLightPrep.pdn)
    {
        LLVKHitch::count("lightPrepLate", 1u);
        lightPrepPlanShadow();
        if (!mLightPrep.pdn) return;   // (the same gate as above: cannot fail -- never a null pass-15 kernel)
        recordLightPrep(cmd, p.view, viewOff, onAsync, false);
    }
    const bool merged = mLightPrep.merged;
    // (#1608 STEREO) the TLAS on the lighting's queue whatever it is -- VR world frames light on the async queue -- as
    // the stereo RT shadow mask / pre-trace / mirror / VR GI take it (c7a688462d); a no-op when already taken
    if (stereoActive()) releaseTlas(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    // (No barrier of its own. Its only job was WAR on the denoiser's images -- last frame's lighting read the final
    //  image, its temporal pass the history: an execution dependency, which recordDeferLight's opening barrier
    //  (src COMPUTE, dst COMPUTE, ahead of every path into here) already gives against every earlier compute on
    //  this queue, and the fork's semaphore against last frame's graphics readers. Pass 15 reads the G-buffer /
    //  depth (graphics: the semaphore / that barrier), the light tables (front end) and the light prep's results
    //  (its closing barrier); nothing recorded before it in the chain -- RT mirror, lit-guard scan, SHADOW /
    //  CLASSIFY, REFLECT, the RT reflections -- reads what it writes (the selection, GEOM, the reservoirs, the job
    //  list) or writes what it reads (llcleanvisqueuevk.cpp THE FRAME WALK's dependency audit).)
    const uint32_t dw = mPdn->width(), dh = mPdn->height();
    VKRtReflPush rp{};
    rp.d = p;
    rp.rtr[0] = (float)dw;
    rp.rtr[1] = (float)dh;
    rp.rtr[2] = (float)(mPdnFrame & 0xFFFFFu);                   // (exact in a float)
    // K | the GEOM parity << 4 (K > 4: pdnCtlWord -- only on the split core, with the high-K SELECT compiled for this
    // frame's flavour; else 4 this frame. The fused kernels read bits 0-2 alone: min(K, 4).)
    mPdnKNow = (mPdnK > 4u && !(mLightPrep.split && mPdnSelectHkPipe[mLightPrep.merged ? 1 : 0])) ? 4u : mPdnK;
    rp.rtr[3] = (float)pdnCtlWord(mPdnKNow, mPdn->parity(), 0u);
    // (#869 SPLIT, clean_deferred_light.comp pdnSplitSelect) rtr3.z: bits 0-2 the split lights per
    // texel (only into images that carry the split layers), bit 3 last frame's split ids are valid
    // here (the hysteresis reads them)
    // Radiance exact-specular reuses one split slot for an unfiltered RGB term.
    // The RIS proposals/PDFs and diffuse are unchanged; ordinary radiance samples
    // zero only the chosen light's specular integrand. Ratio split is unchanged.
    const uint32_t splitK = mPdn->hasSplit() ? (mPdnRadiance ? (mPdnExactSpecular ? 1u : 0u) : mPdnSplitK) : 0u;
    rp.rtr3[2] = splitK | (splitK != 0u && mPdn->historyValid() ? 8u : 0u);
    // (RenderVKShadowMoverMask 2) bits 8-11: THE MOVER COUNTDOWN's length, max(RenderVKRTShadowMoverPersist,
    // RenderVKRTShadowDynamicMaxHistory) -- the sun channel's (clean_dn_temporal.comp), frames a texel stays mover-exact
    // and history-capped after its rays last met a mover (clean_deferred_light.comp pdnMoverCountdownLen)
    // ... and bit 12: last frame's MOT holds the countdowns (it ran the exact selection, the denoiser's history is valid --
    // a resized / fresh MOT holds nothing to reproject)
    if (mLightPrep.exact)
        rp.rtr3[2] |= (std::min<uint32_t>(moverCountdownLen(), 15u) << 8u)
                    | (mPdnExactPrev && mPdn->historyValid() ? 0x1000u : 0u);
    mPdnExactPrev = mLightPrep.exact;
    U32 slope = 2, bleed = 1, facing = 1, oneRay = 0, awayTrace = 1;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_slope(gSavedSettings, "RenderVKRTShadowSlopeBias", 2U);
    static LLCachedControl<U32> s_bleed(gSavedSettings, "RenderVKProjStochBleedFix", 1U);
    static LLCachedControl<U32> s_facing(gSavedSettings, "RenderVKProjStochFacingGate", 1U);
    static LLCachedControl<U32> s_one(gSavedSettings, "RenderVKShadowSunOneRay", 0U);
    static LLCachedControl<U32> s_away(gSavedSettings, "RenderVKRTShadowMergeAwayTrace", 1U);
    slope = std::min<U32>(s_slope, 3u);
    bleed = s_bleed; facing = s_facing; oneRay = s_one; awayTrace = s_away;
#endif
    const bool sunSkip = mLightPrep.sunSkip;   // (SUN-CHANNEL SKIP: lightPrepPlan)
    // clean_deferred_light.comp pass 15: rtr2.x the weak-sun weight (0 = no sun ray), rtr2.y the
    // view-slope origin offset (RenderVKRTShadowSlopeBias: 0, 1/8, 1/4, 1/2 of the render-pixel
    // footprint -- the RT shadow mask's rtSlopeBiasPx; the generic core gives it to the projectors
    // too), rtr2.z the PDN_OPT_* bits (exact integer).
    if (merged) rp.rtr2[0] = mShadowMergeSunW;
    rp.rtr2[1] = slope == 0u ? 0.f : 0.0625f * (float)(1u << slope);
    // (PDN_OPT_BLEED: the no-information flag, RenderVKProjStochBleedFix; PDN_OPT_FACING: the
    // turned-away receiver gate, RenderVKProjStochFacingGate -- clean_deferred_light.comp)
    // (#1031 PDN_OPT_OFIRST, RenderVKShadowOpaqueFirst: the generic core's walks try the opaque geometry
    //  alone first -- clean_shadow_core.glsl OPAQUE FIRST)
    bool ofirst = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<bool> s_ofirst(gSavedSettings, "RenderVKShadowOpaqueFirst", false);
    ofirst = s_ofirst;
#endif
    rp.rtr2[2] = (float)((bleed != 0u ? 1u : 0u) | (merged && oneRay != 0u ? 2u : 0u) | (sunSkip ? 4u : 0u)
                         | (facing != 0u ? 8u : 0u) | (mPdnDebug == 7u ? 16u : 0u)
                         | (merged && awayTrace != 0u ? 32u : 0u)   // (PDN_OPT_SUN_AWAY)
                         | (ofirst ? 64u : 0u)                      // (PDN_OPT_OFIRST)
                         // (PDN_OPT_MOVER, RenderVKShadowMoverMask: every stochastic shadow ray leaves the
                         //  movers out -- the RT shadow mask traces them, moverRouteDecide)
                         | (mMoverRouteNow ? 128u : 0u)
                         // (RenderVKShadowMoverMask 2: PDN_OPT_MOVER_ALL -- every LOCAL-light ray sees all casters again
                         //  (the mask traces no local mover rays); PDN_OPT_MOVER_EXACT -- SELECT's mover-exact selection
                         //  and its job list (the split core with the list allocated))
                         | (mMoverRouteNow && mMoverExactNow ? 256u : 0u)
                         | (mMoverRouteNow && mLightPrep.exact ? 512u : 0u)
                         // (RenderVKProjStochVisCost, debug: PDN_OPT_VIS_NOWALK -- VIS traces no ray, the samples
                         //  read lit; + PDN_OPT_VIS_NORECV -- VIS skips the receiver too. 0 = neither: today's frame)
                         | (mPdnVisCost >= 1u ? 1024u : 0u) | (mPdnVisCost >= 2u ? 2048u : 0u));
    // (#869 Phase 5b / 5c) rtr2.w: RS_RES_ON = the reservoirs are bound at this extent (pass 15 writes
    // this parity's), RS_RES_PREV = last frame's pass 15 wrote the other parity's at this extent
    uint32_t rsRes = 0u;
    if (mRsNow && mRsRes.buffer && mRsResW == dw && mRsResH == dh)
        rsRes = 1u | (mRsResPrevOk ? 2u : 0u);
    rp.rtr2[3] = (float)rsRes;
    mRsResPrevOk = rsRes != 0u;
    // pass 15's kernel and the split (lightPrepPlan: the generic core on a ReSTIR / split frame)
    VkPipeline pipe = mLightPrep.pipe;
    const bool split = mLightPrep.split;
    if (split) rp.rtr3[3] = mPdnVisSlots;   // (clean_deferred_light.comp pdnVisSlots)
    // (RenderVKProjStochasticPasses, debug) N > 1 only on the split core in radiance mode without the mover-exact list
    // (recordProjStochPasses has the rules); else this frame runs one pass, the refusal logged once per change
    {
        uint32_t why = 0u;
        if (mPdnPasses > 1u)
        {
            if (!split) why = 1u;
            else if (!mPdnRadiance) why = 2u;
            else if (mLightPrep.exact) why = 3u;
        }
        mPdnPassesNow = (mPdnPasses > 1u && why == 0u) ? mPdnPasses : 1u;
        const uint32_t key = mPdnPasses > 1u ? (mPdnPasses | (why << 8u)) : 1u;
        if (key != mPdnPassesWhy)
        {
            static const char* const kWhy[4] = {"", "the split shadow kernels are not running this frame (fused core)",
                                                "radiance mode is off (RenderVKReSTIRRadiance 0)",
                                                "the mover-exact list runs (RenderVKShadowMoverMask 2)"};
            if (!(mPdnPassesWhy == ~0u && key == 1u))
            {
                if (mPdnPasses <= 1u)
                    LL_INFOS("VKShadow") << "RenderVKProjStochasticPasses 1: one pass (off)" << LL_ENDL;
                else if (why == 0u)
                    LL_INFOS("VKShadow") << "RenderVKProjStochasticPasses " << mPdnPasses << ": running, " << mPdnPasses
                                         << " passes x " << mPdnK << " picks per texel (timers c_defer_pstochsel / "
                                            "pstochvis / pstoch -- shadow* when merged -- sum all passes)" << LL_ENDL;
                else
                    LL_INFOS("VKShadow") << "RenderVKProjStochasticPasses " << mPdnPasses << ": refused, " << kWhy[why]
                                         << " -- one pass" << LL_ENDL;
            }
            mPdnPassesWhy = key;
        }
    }
    // (RenderVKProjStochasticAdaptive) the adaptive extra passes: the same rules as the passes above, + the tile list
    // allocated beside no exact list (pdnSelFrame) and its mask kernel compiled; RenderVKProjStochasticPasses > 1 wins.
    // Else none -- not one command of this path is recorded (setting 0: exactly today's frame)
    {
        uint32_t why = 0u;
        if (mPdnAdapt > 0u)
        {
            if (mPdnPassesNow > 1u) why = 1u;
            else if (!split) why = 2u;
            else if (!mPdnRadiance) why = 3u;
            else if (mLightPrep.exact || mPdnVisExact) why = 4u;
            else if (!mPdnVisTiles) why = 5u;
            else if (!mPdnMaskPipe) why = 6u;
        }
        mPdnAdaptNow = (mPdnAdapt > 0u && why == 0u) ? mPdnAdapt : 0u;
        // (the stats ring goes with the setting: retired, never in use by a frame in flight once freed)
        if (mPdnAdapt == 0u && mPdnAdaptRb.buffer)
        {
            retireBuffer(mPdnAdaptRb);
            mPdnAdaptRb = GpuBuffer{};
            std::fill(std::begin(mPdnAdaptRbUsed), std::end(mPdnAdaptRbUsed), false);
        }
        const uint32_t key = mPdnAdapt | (why << 8u) | (mPdnAdaptCap << 12u) | (mPdnAdaptHist ? 1u << 20u : 0u)
                           | (mPdnAdaptRule << 21u) | (mPdnKx << 22u) | (mPdnK << 27u);
        if (key != mPdnAdaptWhy)
        {
            static const char* const kWhy[7] = {"", "RenderVKProjStochasticPasses is above 1 (it wins)",
                                                "the split shadow kernels are not running this frame (fused core)",
                                                "radiance mode is off (RenderVKReSTIRRadiance 0)",
                                                "the mover-exact list is allocated (RenderVKShadowMoverMask 2)",
                                                "the tile list is not allocated (refused, or not yet at this extent)",
                                                "the mask kernel is not compiled yet"};
            if (!(mPdnAdaptWhy == ~0u && mPdnAdapt == 0u))
            {
                U32 moverMode = 0;
#if defined(LL_HAS_CLEAN_PIPELINE)
                static LLCachedControl<U32> s_mover(gSavedSettings, "RenderVKShadowMoverMask", 0U);
                moverMode = s_mover;
#endif
                if (mPdnAdapt == 0u)
                    LL_INFOS("VKShadow") << "RenderVKProjStochasticAdaptive 0: off" << LL_ENDL;
                else if (why == 0u)
                    LL_INFOS("VKShadow") << "RenderVKProjStochasticAdaptive " << mPdnAdapt << ": running, " << mPdnAdapt
                                         << " extra passes x " << pdnAdaptK() << " picks on the flagged tiles (pass 0: "
                                         << mPdnK << " picks"
                                         << (pdnAdaptK() > 4u && !mPdnSelectHkPipe[0]
                                                 ? "; the high-K SELECT is not compiled yet: 4 meanwhile" : "")
                                         << "), cap "
                                         << (mPdnAdaptCap ? std::to_string(mPdnAdaptCap) + "%" : std::string("none"))
                                         << ", adaptive mover history " << (mPdnAdaptHist ? "on" : "off")
                                         << ", penumbra rule " << (mPdnAdaptRule ? "1 (mixed visibility)"
                                                                                 : "0 (picks disagree)")
                                         << ", RenderVKShadowMoverMask " << moverMode
                                         << (moverMode == 1u ? " (local rays leave the movers out: no moving-caster tiles)" : "")
                                         << " (timers c_defer_pstochmask, c_defer_pstochxsel / xvis / x)" << LL_ENDL;
                else
                    LL_INFOS("VKShadow") << "RenderVKProjStochasticAdaptive " << mPdnAdapt << ": refused, " << kWhy[why]
                                         << LL_ENDL;
            }
            mPdnAdaptWhy = key;
        }
    }
    // (#771 Phase 2) the SUN EXPOSURE CERTIFICATES' table (the light prep's maintenance; 0 = SXC off): pass 15
    // skips the sun sample where a certificate proves it blocked
    rp.rtr3[0] = (uint32_t)(mLightPrep.sxcAddr & 0xFFFFFFFFull);
    rp.rtr3[1] = (uint32_t)(mLightPrep.sxcAddr >> 32);
    const VkDescriptorSet sets[5] = {mDsetDraw0, mDsetDraw1, mDsetDefer, mDsetDeferSplit, rtSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mRtReflLayout, 0, 5, sets, 1, &viewOff);
    VK_PUSH_BUDGET_CHECK(rp);
    // (LVC stage 3, RenderVKShadowReachMap) the reach maps' maintenance right before pass 15, on its queue and TLAS
    if (mLvcNow && mRsNow && mLvcPipe != VK_NULL_HANDLE && mRegir.buffer) recordLvc(cmd, rp, onAsync);
    vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rp), &rp);
    VkPipeline visPipe = VK_NULL_HANDLE;   // (the split core's VIS: the extra passes take the same)
    if (split)
    {
        // (#1031) SELECT (the candidates, the reservoirs, the sample list + its gate -> the descriptors + GEOM);
        // SUNVIS (merged: the sun channel, whole) and VIS (the gated light samples' walks -> mPdnVis), both
        // reading the descriptors, independent of each other; then SHADE (the terms x the visibilities +
        // every other store). Compute -> compute RAW between the steps. VIS runs one thread per gated light
        // sample: SELECT appends them to mPdnVis's job list, whose header (count 0, (0, 1, 1) workgroups) THE LIGHT
        // PREP reset ahead of the lighting (recordLightPrep: no transfer drain pair here) is VIS's indirect
        // dispatch.
        constexpr VkAccessFlags2 kRW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        auto raw = [cmd]()
        {
            VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
            m.dstAccessMask = kRW | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
            VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
            vkCmdPipelineBarrier2(cmd, &d);
        };
        // (RenderVKShadowIsoLod: the walk kernels' isotropic alpha-test LOD -- the footprint ones meanwhile
        //  if those still compile)
        bool isoLod = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<bool> s_iso(gSavedSettings, "RenderVKShadowIsoLod", false);
        isoLod = s_iso;
#endif
        // (#1128: the hang beacon's twins while RenderVKRTHangBeacon is on)
        const VkPipeline sunVis = rtBcPipe(isoLod && mPdnSunVisIsoPipe ? mPdnSunVisIsoPipe : mPdnSunVisPipe);
        // (LVC stage 0, RenderVKReSTIROcclusionCull 3) the statistics variant while it is compiled (pass 0 and the extra
        //  passes alike: visPipe); otherwise the production kernels
        const VkPipeline vis = (mRsOccStatsNow && mPdnVisLvcPipe != VK_NULL_HANDLE) ? mPdnVisLvcPipe
                             : rtBcPipe(isoLod && mPdnVisIsoPipe ? mPdnVisIsoPipe : mPdnVisPipe);
        visPipe = vis;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          mPdnKNow > 4u ? mPdnSelectHkPipe[merged ? 1 : 0] : mPdnSelectPipe[merged ? 1 : 0]);
        {
            GpuPassScope scope(*this, cmd, onAsync ? (merged ? "c_defer_shadowsel" : "c_defer_pstochsel") : (merged ? "g_defer_shadowsel" : "g_defer_pstochsel"));
            vkCmdDispatch(cmd, (dw + 7u) / 8u, (dh + 7u) / 8u, mViewCount);   // (#1608: z = the views)
        }
        if (merged) gpuSubMark(cmd, onAsync ? "c_defer_shadowsel" : "g_defer_shadowsel");
        else        gpuSubMark(cmd, onAsync ? "c_defer_pstochsel" : "g_defer_pstochsel");
        raw();
        if (merged && !sunSkip)
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sunVis);
            {
                GpuPassScope scope(*this, cmd, onAsync ? "c_defer_shadowsun" : "g_defer_shadowsun");
                vkCmdDispatch(cmd, (dw + 7u) / 8u, (dh + 7u) / 8u, mViewCount);   // (#1608: z = the views)
            }
            gpuSubMark(cmd, onAsync ? "c_defer_shadowsun" : "g_defer_shadowsun");
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vis);
        {
            GpuPassScope scope(*this, cmd, onAsync ? (merged ? "c_defer_shadowvis" : "c_defer_pstochvis") : (merged ? "g_defer_shadowvis" : "g_defer_pstochvis"));
            vkCmdDispatchIndirect(cmd, mPdnVis.buffer, mPdnVisOff + 4u);   // (one thread per gated light sample, 64 per workgroup)
        }
        if (merged) gpuSubMark(cmd, onAsync ? "c_defer_shadowvis" : "g_defer_shadowvis");
        else        gpuSubMark(cmd, onAsync ? "c_defer_pstochvis" : "g_defer_pstochvis");
        if (mLightPrep.exact)
        {
            // (RenderVKShadowMoverMask 2) THE MOVER-EXACT JOBS: the same VIS kernel over SELECT's exact list (one thread
            // per texel x light near a mover: one all-caster ray each) -- push rtr3.w bit 31 says which list; SHADE
            // gets the plain word back. Its own sub-mark (c_defer_shadowvisx / c_defer_pstochvisx): the cost of mode 2.
            const uint32_t slotsX = mPdnVisSlots | 0x80000000u;
            const uint32_t off = (uint32_t)(offsetof(VKRtReflPush, rtr3) + 3u * sizeof(uint32_t));
            vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, off, sizeof(uint32_t), &slotsX);
            {
                GpuPassScope scope(*this, cmd, onAsync ? (merged ? "c_defer_shadowvisx" : "c_defer_pstochvisx") : (merged ? "g_defer_shadowvisx" : "g_defer_pstochvisx"));
                vkCmdDispatchIndirect(cmd, mPdnVis.buffer, mPdnVisOff + pdnExactWordOff() + 4u);
            }
            vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, off, sizeof(uint32_t), &rp.rtr3[3]);
            if (merged) gpuSubMark(cmd, onAsync ? "c_defer_shadowvisx" : "g_defer_shadowvisx");
            else        gpuSubMark(cmd, onAsync ? "c_defer_pstochvisx" : "g_defer_pstochvisx");
        }
        raw();
        pipe = mPdnShadePipe;
    }
    recordProjStochTail(cmd, p, viewOff, onAsync, pipe, merged, sunSkip, bleed, rsRes, oneRay, rp, visPipe);
}

// recordProjStoch from its last kernel on (SHADE / the fused pass, the ReSTIR counters, the denoiser, the split
// layout re-bound; the RT layout's sets and pass 15's push bound).
void LLCleanRendererVK::recordProjStochTail(VkCommandBuffer cmd, const VKDeferPush& p, uint32_t viewOff, bool onAsync,
                                            VkPipeline pipe, bool merged, bool sunSkip, uint32_t bleed, uint32_t rsRes,
                                            uint32_t oneRay, const VKRtReflPush& rp, VkPipeline visPipe)
{
    const uint32_t dw = mPdn->width(), dh = mPdn->height();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    {
        GpuPassScope scope(*this, cmd, onAsync ? (merged ? "c_defer_shadowmerge" : "c_defer_pstoch") : (merged ? "g_defer_shadowmerge" : "g_defer_pstoch"));
        vkCmdDispatch(cmd, (dw + 7u) / 8u, (dh + 7u) / 8u, mViewCount);   // (#1608: SHADE / the fused kernels z = the views)
    }
    if (merged) gpuSubMark(cmd, onAsync ? "c_defer_shadowmerge" : "g_defer_shadowmerge");
    else        gpuSubMark(cmd, onAsync ? "c_defer_pstoch" : "g_defer_pstoch");
    // (RenderVKProjStochasticPasses) passes 1..N-1 into the signal pass 0's SHADE just wrote, before anything reads it
    // (RenderVKProjStochasticAdaptive: passes 1..A on the needs-rays mask's tiles alone, the same fold)
    if ((mPdnPassesNow > 1u || mPdnAdaptNow != 0u) && pipe == mPdnShadePipe && visPipe != VK_NULL_HANDLE)
        recordProjStochPasses(cmd, rp, visPipe, merged, onAsync);
    if (mRsNow) rsStatsRecord(cmd);   // (#869: the ReSTIR counters of this frame)
    // (THE RT LIGHTING SCRATCH) the split core's last access to its ranges: the GI's records may follow over them
    if (pipe == mPdnShadePipe) rtScratchHandoff(cmd);

    LLCleanDenoiserVK::Params dp;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<F32> s_hd(gSavedSettings, "RenderVKDenoiseHistory", 4.f);
    static LLCachedControl<F32> s_hs(gSavedSettings, "RenderVKDenoiseSpecHistory", 24.f);
    static LLCachedControl<F32> s_hm(gSavedSettings, "RenderVKDenoiseSpecMinHistory", 4.f);
    static LLCachedControl<F32> s_gamma(gSavedSettings, "RenderVKDenoiseClamp", 1.5f);
    static LLCachedControl<F32> s_lphi(gSavedSettings, "RenderVKDenoiseLumaPhi", 4.f);
    static LLCachedControl<F32> s_wcut(gSavedSettings, "RenderVKDenoiseLightChange", 0.25f);
    dp.maxHistDiffuse = s_hd;
    dp.maxHistSpecular = s_hs;
    dp.minHistSpecular = s_hm;
    dp.clampGamma = s_gamma;
    dp.lumPhi = s_lphi;
    dp.wCut = s_wcut;
#endif
    dp.atrous = mPdnAtrous;
    dp.debug = mPdnDebug;
    dp.frame = mPdnFrame;
    dp.exactSpecular = mPdnRadiance && mPdnExactSpecular && mPdn->hasSplit();
    dp.sampleGridHistory = mPdnSampleGridHistory;
    dp.shadowPlaneReconstruction = mPdnPlaneReconstruction && dp.sampleGridHistory;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<bool> s_specGuard(gSavedSettings, "RenderVKShadowSpecularLobeGuard", true);
    dp.specularLobeGuard = s_specGuard;
    static LLCachedControl<S32> s_casterPolicy(gSavedSettings, "RenderVKShadowCasterHistory", 3);
    dp.casterHistoryPolicy = (uint32_t)llclamp((S32)s_casterPolicy, 0, 3);
#endif
    // MOVER SHADOW ROUTING (RenderVKShadowMoverMask): the stochastic histories hold STATIC occlusion only -- the
    // moving-caster policies (cap / reset / mover record) have nothing to react to and are bypassed (0); the
    // movers' shadows are the RT shadow mask's, per frame. (The modes stay for the A/B with the setting off.)
    if (mMoverRouteNow) dp.casterHistoryPolicy = 0u;
    // (RenderVKShadowMoverMask 2) THE EXACT-MOVER HISTORY RULE (clean_dn_temporal.comp): the sun channel's moving-caster
    // rule for the local radiance -- where pass 15's mover countdown runs (MOT w, a ray met a mover now or within it)
    // the history is capped at RenderVKRTShadowDynamicMaxHistory; nothing is cut on entering / leaving the exact set. The
    // a-trous takes only exact taps around an exact centre.
    dp.moverExact = mMoverRouteNow && mLightPrep.exact;
    if (dp.moverExact) dp.sunDynMax = (F32)moverDynMaxHistory();
    // (radiance, every mover mode: clean_dn_temporal.comp THE CONVERGENCE RULE) the sun's moving-caster rule for the local
    // radiance -- a texel whose rays met a moving caster within the countdown keeps RenderVKRTShadowDynamicMaxHistory frames
    dp.moverCdLen = moverCountdownLen();
    dp.moverCdCap = moverDynMaxHistory();
    // (radiance, RenderVKShadowFixMoverVariance A/B, default 0 = as before: clean_dn_temporal.comp THE COUNTDOWN'S NOISE)
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_moverVarFix(gSavedSettings, "RenderVKShadowFixMoverVariance", 0U);
    dp.moverVarFix = std::min<U32>(s_moverVarFix, 2u);
#endif
    // (RenderVKProjStochasticAdaptiveHistory A/B, default 0: clean_dn_temporal.comp THE ADAPTIVE MOVER HISTORY) the
    // adaptive passes ran: a countdown texel that took them keeps cap / (1 + A) frames
    dp.adaptPasses = (mPdnAdaptNow != 0u && mPdnAdaptHist) ? mPdnAdaptNow : 0u;
    dp.rasterJitter[0] = mTaaJitterOn ? mTaaJitter[0] : 0.f;
    dp.rasterJitter[1] = mTaaJitterOn ? mTaaJitter[1] : 0.f;
    // #870 raw routing (DLSS RR reconstructs): the lighting reads this frame's raw projector /
    // merged-sun samples; the denoiser passes them through (the a-trous loop still runs: its last
    // iteration writes the depth the consumer's upsample reads). (#869 SPLIT: the split lights'
    // one-ray visibility passes raw with the rest's ratio -- the projector signal is both; with
    // RenderVKProjStochasticSplitK 0 exactly the old routing.)
    // Radiance mode keeps the local projector integrator (rrRawSignal): its sampled gobo /
    // BRDF and reused light picks need filtering at full resolution too. The merged sun's
    // visibility has its own raw bit and denoiser channel.
    dp.raw = rrRawSignal(kRrRawProj, mPdn->width(), mPdn->height());
    dp.rawSun = rrRawSignal(kRrRawMergedSun, mPdn->width(), mPdn->height());
    // Once per routing/extent change: the saved scale can differ from the live allocation
    // (budget fallback), and "full" here is relative to DLSS's internal render extent.
    const bool sunActive = merged && !sunSkip;
    const uint32_t routeKey[13] = {p.width, p.height, dw, dh, mPdnK, mPdnRadiance ? 1u : 0u,
        mRsNow ? 1u : 0u, dlssRrFrame() ? 1u : 0u, mRrRawMask, dp.raw ? 1u : 0u, dp.rawSun ? 1u : 0u,
        (sunActive ? 1u : 0u) | (dp.exactSpecular ? 2u : 0u) | (dp.shadowPlaneReconstruction ? 4u : 0u), mPdnPassesNow};
    if (!std::equal(routeKey, routeKey + 13, mPdnRouteKey))
    {
        std::copy(routeKey, routeKey + 13, mPdnRouteKey);
        LL_INFOS("VKShadow") << "stochastic signal: render=" << p.width << "x" << p.height
            << " denoiser=" << dw << "x" << dh << " samples=" << mPdnK << " passes=" << mPdnPassesNow
            << " radiance=" << mPdnRadiance
            << " restir=" << mRsNow << " rr=" << dlssRrFrame() << " rawMask=" << mRrRawMask
            << " exactSpecular=" << dp.exactSpecular << " planeReconstruction=" << dp.shadowPlaneReconstruction
            << " projectorOwner=" << (dp.raw ? "RR" : "local")
            << " sunOwner=" << (!sunActive ? "idle" : dp.rawSun ? "RR" : "local") << LL_ENDL;
    }
    dp.info = bleed != 0u;   // (the producer flags no-information texels with PDN_OPT_BLEED)
    // (RenderVKReSTIRRadiance) RADIANCE SEMANTICS (the GI denoiser's): 0 is a real value -- a texel whose
    // samples carried no light is dark, not "unknown" -- so no no-information samples; the ratio-only rules
    // (the converged shortcut) are off below. W stays the light-set reference (its
    // anti-lag cut is as valid for radiance), the disocclusion rules stay.
    if (mPdnRadiance) dp.info = false;
    // ... and the temporal clamp's sigma floored at the history's per-sample spread (DN_F_RADIANCE): the reused
    // reservoirs make the raw 3x3 one correlated pick, whose collapsed box reset the history every frame (the
    // crawling blotches). Off: the ratio's clamp exactly as before.
    dp.radiance = mPdnRadiance;
    // (#1038) DISOCCLUSION: the GI denoiser's young-history rules for the local-light shadows too (an
    // avatar walking out from behind a wall restarted its ReSTIR DI / projector shadow ratio from one
    // ray, with only the plain a-trous): the history fix (RenderVKDenoiseDisoccFix frames), the young-
    // texel boost and the moving-surface depth tolerance. The split lights' own channel keeps its rules.
    {
        U32 dfix = 4;
        bool yboost = true, mztol = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<U32>  s_dfix(gSavedSettings, "RenderVKDenoiseDisoccFix", 4U);
        static LLCachedControl<bool> s_yboost(gSavedSettings, "RenderVKDenoiseYoungBoost", true);
        static LLCachedControl<bool> s_mztol(gSavedSettings, "RenderVKDenoiseMovingDepthTol", true);
        dfix = s_dfix; yboost = s_yboost; mztol = s_mztol;
#endif
        dp.histFix = std::min<U32>(dfix, 7u);
        dp.youngBoost = yboost;
        dp.moveZTol = mztol;
        bool receiverGuides = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<bool> s_receiver(gSavedSettings, "RenderVKDenoiseReceiverGuides", true);
        receiverGuides = s_receiver;
#endif
        // A soft refusal of previous-pose storage retains legacy validation;
        // never claim current-pose descriptor aliases are exact prior guides.
        const bool previousSkinReady = !mSkinVbo.buffer
            || (mPrevSkinPos.buffer && mPrevSkinCap == mSkinVertCap);
        dp.localReceiverGuides = receiverGuides && previousSkinReady;
    }
    // (#869 / #85) a ReSTIR-converged diffuse takes RenderVKReSTIRDenoiseIters a-trous iterations
    {
        U32 convIt = 1;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<U32> s_conv(gSavedSettings, "RenderVKReSTIRDenoiseIters", 1U);
        convIt = s_conv;
#endif
        // (RenderVKReSTIRRadiance: never -- a reused pick's M says the light SELECTION converged, and a
        //  one-pick radiance sample is noisy whatever the selection: the full a-trous, like the GI's)
        dp.convIters = (rsRes != 0u && convIt != 0u && !mPdnRadiance) ? std::min<U32>(convIt, 7u) : 0u;
        // (#869) ... but never on a surface that moved on its own (an animated avatar: a still
        // light's shadow slides over it -- the reservoir's M is no sign of a converged shadow)
        F32 surfMove = 0.5f;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<F32> s_smove(gSavedSettings, "RenderVKReSTIRSurfaceMoveVeto", 0.5f);
        surfMove = s_smove;
#endif
        dp.surfMoveTexels = dp.convIters != 0u ? std::clamp(surfMove, 0.f, 3.75f) : 0.f;
    }
    // (#869) the moving-light rule (RenderVKReSTIRMoveCut): the history of a texel lit by a light that
    // moved this frame -- only on a ReSTIR frame (pass 15 writes the moved share into MOT w then;
    // regirHeaderFill sets the producer's half from the same setting)
    {
        U32 moveCut = 1;
        F32 moveHist = 4.f;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<U32> s_mcut(gSavedSettings, "RenderVKReSTIRMoveCut", 1U);
        static LLCachedControl<F32> s_mhist(gSavedSettings, "RenderVKReSTIRMoveHistory", 4.f);
        moveCut = s_mcut; moveHist = s_mhist;
#endif
        // Radiance uses the same bounded history, without the ratio's extra sampling.
        dp.moveHist = (mRsNow && moveCut != 0u) ? (U32)std::clamp(moveHist + 0.5f, 1.f, 15.f) : 0u;
        dp.radianceMoveReset = mPdnRadianceMoveReset && dp.moveHist != 0u;
    }
    {
        bool recovery = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<bool> s_recovery(gSavedSettings, "RenderVKDenoiseSkinnedVarianceRecovery", true);
        recovery = s_recovery;
#endif
        dp.skinnedVarianceRecovery = recovery && dp.radiance && dp.localReceiverGuides;
        // Report the actual local-shadow route, not an inference from RR settings.
        // No scene walk/readback: constant-size state, at most one line per second.
        const uint32_t route = (dp.raw ? 1u : 0u) | (dp.radiance ? 2u : 0u)
            | (dp.radianceMoveReset ? 4u : 0u) | (dp.localReceiverGuides ? 8u : 0u)
            | (dp.skinnedVarianceRecovery ? 16u : 0u) | (dp.sampleGridHistory ? 32u : 0u)
            | (mTaaJitterOn ? 64u : 0u) | (dp.debug << 8u);
        static uint32_t lastRoute = ~0u, lastWidth = 0u, lastHeight = 0u;
        static double lastLog = -2.0;
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if ((route != lastRoute || dw != lastWidth || dh != lastHeight) && now - lastLog >= 1.0)
        {
            LL_INFOS("Vulkan") << "Local shadow receiver route: raw=" << dp.raw
                << " radiance=" << dp.radiance << " colourReset=" << dp.radianceMoveReset
                << " priorGuides=" << dp.localReceiverGuides
                << " skinnedVariance=" << dp.skinnedVarianceRecovery
                << " sampleGrid=" << dp.sampleGridHistory << " jitter=" << mTaaJitterOn << " debug=" << dp.debug
                << " extent=" << dw << "x" << dh << " render=" << p.width << "x" << p.height
                << LL_ENDL;
            lastRoute = route; lastWidth = dw; lastHeight = dh; lastLog = now;
        }
    }
    if (merged && !sunSkip)
    {
        // THE SUN / MOON CHANNEL: the RT shadow mask's own denoiser settings (its history, the
        // moving-caster rules, the spatial radius cap, the exact reprojection) -- the sun keeps
        // the mask's quality on the denoiser's guides (llcleandenoisevk.h SUN / MOON CHANNEL).
        F32 hist = 32.f, dyn = 2.f, spatial = 12.f, gamma = 1.5f, soft = 0.01f;
        U32 dmax = 4, persist = 4;
        bool exact = true, rrHistory = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<F32> s_shist(gSavedSettings, "RenderVKRTShadowHistory", 32.f);
        static LLCachedControl<F32> s_sdyn(gSavedSettings, "RenderVKRTShadowDynamicHistory", 2.f);
        static LLCachedControl<U32> s_sdmax(gSavedSettings, "RenderVKRTShadowDynamicMaxHistory", 4U);
        static LLCachedControl<F32> s_ssp(gSavedSettings, "RenderVKRTShadowSpatial", 12.f);
        static LLCachedControl<F32> s_sgamma(gSavedSettings, "RenderVKRTShadowClamp", 1.5f);
        static LLCachedControl<U32> s_spersist(gSavedSettings, "RenderVKRTShadowMoverPersist", 4U);
        static LLCachedControl<bool> s_sexact(gSavedSettings, "RenderVKRTMaskExactReproj", true);
        static LLCachedControl<bool> s_srrHistory(gSavedSettings, "RenderVKRTShadowRrHistory", true);
        static LLCachedControl<F32> s_ssoft(gSavedSettings, "RenderVKRTShadowSoftness", 0.01f);
        hist = s_shist; dyn = s_sdyn; dmax = s_sdmax; spatial = s_ssp; gamma = s_sgamma;
        persist = s_spersist; exact = s_sexact; soft = s_ssoft; rrHistory = s_srrHistory;
#endif
        dp.sun = true;
        dp.sunCut = mShadowMergeSunCut;
        dp.sunHard = soft <= 0.f;
        dp.sunExact = exact;
        // Same current-frame route as the separate mask, not the previous frame's RR result.
        dp.sunNoResolve = rrHistory && dlssRrFrame() && mTaaJitterOn && !mOffscreen;
        dp.sunMaxHist = std::clamp(hist, 1.f, 64.f);
        dp.sunDynHist = std::clamp(dyn, 1.f, 15.f);
        dp.sunDynMax = (F32)std::min<U32>(dmax, 15u);
        dp.sunMove = mShadowMergeSunMove;
        // (the mask's cap is in mask texels; the denoiser's extent is the same 1 or 1/2 of render)
        dp.sunSpatialCap = std::clamp(spatial, 0.f, 16.f);
        dp.sunClamp = std::clamp(gamma, 0.5f, 8.f);
        dp.sunPersist = std::min<U32>(persist, 7u);
        dp.sunRays = oneRay != 0u ? 1.f : 0.f;   // (RenderVKShadowSunOneRay: one disc ray per texel)
    }
    // (RenderVKNoiseStats) the noise probe: pass 15's RAW radiance (IN layers 0 + 1: diffuse + specular) before the
    //  temporal pass, the FINAL denoised one after the a-trous -- each texel against itself last frame (still camera)
    bool noisePdn = false;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<bool> s_noisePdn(gSavedSettings, "RenderVKNoiseStats", false);
    noisePdn = s_noisePdn && !dp.raw;
#endif
    if (noisePdn)
    {
        mPdn->noiseBegin(cmd);
        mPdn->recordNoise(cmd, 0u, false, dp.atrous, 3u, mNoiseCamStill, this, onAsync ? "c_noise" : "g_noise");
        gpuSubMark(cmd, onAsync ? "c_noise" : "g_noise");
    }
    else mPdn->noiseRelease();
    mPdn->recordTemporal(cmd, mDsetDraw0, p.width, p.height, dp, this, onAsync ? "c_dn_tmp" : "g_dn_tmp");
    gpuSubMark(cmd, onAsync ? "c_dn_tmp" : "g_dn_tmp");
    // (the light-drop fade) the temporal pass wrote this frame's drop marks into MOT w (radiance, the caster policy's
    // evidence test, a real history: clean_dn_temporal.comp THE DROP MARK) -- DLSS RR's responsivity guide reads them
    mPdnDropNow = dp.radiance && !dp.raw && dp.casterHistoryPolicy >= 2u && dp.debug != 11u;
    mPdn->recordSpatial(cmd, p.width, p.height, dp, this, onAsync ? "c_dn_sp" : "g_dn_sp");
    gpuSubMark(cmd, onAsync ? "c_dn_sp" : "g_dn_sp");
    if (noisePdn)
    {
        mPdn->recordNoise(cmd, 1u, true, dp.atrous, 3u, mNoiseCamStill, this, onAsync ? "c_noise" : "g_noise");
        mPdn->noiseEnd(cmd);
        gpuSubMark(cmd, onAsync ? "c_noise" : "g_noise");
    }
    // Back to the split layout (incompatible push range: every set re-bound) for the passes after.
    const VkDescriptorSet ssets[4] = {mDsetDraw0, mDsetDraw1, mDsetDefer, mDsetDeferSplit};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mDeferSplitLayout, 0, 4, ssets, 1, &viewOff);
    vkCmdPushConstants(cmd, mDeferSplitLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
}

// ======================= MORE RAYS: RenderVKProjStochasticPasses (debug, 1-8, nonpersistent) =======================
// What N x K rays per texel would buy, without widening the K <= 4 layout (the control word's 3 bits, the descriptors'
// 4 picks, the visibility slots / job word): after pass 0's SHADE the split core's SELECT -> VIS -> SHADE runs N - 1
// more times, each pass p a full K-pick estimate whose SHADE folds into the signal pass 0 wrote (IN layers 0 / 1 rgb,
// in place -- no accumulation target: 0 B extra VRAM, nothing to free), before the counters and the denoiser.
//   * INDEPENDENCE: every random input of pass 15 derives from the push's frame word rtr.z -- the PCG seeds of the RIS
//     walks (grid window + picks, emitters, cluster walk), the probe, the dense sub-window and the reuse taps
//     (m.x / m.y / frame), the disc / IGN noise phase rt_noise_shift (R2 of the frame), rsPrimaryLookupCell's jitter (R2),
//     the emitter points' R3 (frame & 0xFFFF), occKeyTexel's jitter. Pass p pushes frame + p x kPassSeedStride (odd:
//     never 0 mod 0x10000; chosen so that for every seed A m.x + B m.y + C frame, p x stride x C stays farther from 0
//     mod 2^32 than A W + B H at an 8K denoiser extent -- no texel's seed of pass p is another texel's of pass 0; the
//     sum stays < 2^24, exact in the float; scripts/check pstoch-passes re-derives it from the shader). The VIS rays'
//     disc phase takes an explicit golden-ratio offset per pass on top (pdnVisRecv): float(frame) x R2 has only 1/32 ..
//     1/4 resolution at those frame words, so the extra passes' phases alone would collide. (Pass 0's own phase coarsens
//     the same way as mPdnFrame grows -- 1/16 at 2^20 sampling frames, before the 20-bit wrap; as before, not changed
//     here.) The ReGIR lookup jitter keeps the frame word's coarser phase (a proposal only). Pass 0 pushes
//     today's word: N = 1 records exactly what it did before, and with N > 1 pass 0 is still today's pass.
//   * PASS 0 OWNS THE FRAME: the reservoirs (read and stored), the GEOM guide, MOT (motion, the moved share, the
//     caster evidence + mover report -- they describe pass 0's K samples), the split lights' and the exact-specular
//     slot's own samples, the occlusion probe + notes, the ReSTIR / emitter / occlusion stats. An extra pass (push rtr3.z
//     bits 16-18, clean_deferred_light.comp pdnPassIdx) pushes no reservoirs (rtr2.w 0) and stores none, traces only
//     the rest's picks, and writes only its descriptors, jobs, visibilities and the fold.
//   * THE RESERVOIR, ONCE: re-resampling last frame's reservoirs N times would hand every pass nearly the same pick (one
//     shared tap pool) -- N correlated copies weighed as N independent estimates. Instead the N passes combine as ONE
//     list of c + N K - 1 samples with pdnSampleLight's confidence weights: the reservoir's pick (pass 0) keeps its
//     confidence c once, against pass 0's K - 1 and the extra passes' (N - 1) K fresh picks: c / (c + N K - 1) of the
//     estimate (c as SELECT capped it, min(M, K - 1): not re-derived against N K). Without a reused reservoir every pass
//     weighs the same (the plain mean). Convex, sample-independent weights of unbiased estimates: unbiased
//     (clean_deferred_light.comp pdnPassBaseWeight / pdnPassFold; scripts/check pstoch-passes replays it).
//   * THE COST IS PAID: each extra pass is a real SELECT (the candidate walk, K streaming reservoirs), VIS (its K
//     rays per texel, with the moved-light extra rays) and SHADE. GPU timers: the same marks as pass 0 --
//     c_defer_pstochsel / c_defer_pstochvis / c_defer_pstoch (merged frames: c_defer_shadowsel / shadowvis /
//     shadowmerge; g_ on graphics) -- sum every pass (the harvest adds marks by name); the job header reset between
//     passes lands in the SELECT line. Compare those lines at N = 1 and N.
//   * WHERE IT RUNS: the split core in radiance mode without the mover-exact list (recordProjStoch: else one pass,
//     logged once) -- a ratio's mean is not the ratio of the means, and the exact list's atomics / histogram would run
//     N times.
void LLCleanRendererVK::recordProjStochPasses(VkCommandBuffer cmd, const VKRtReflPush& rp, VkPipeline visPipe,
                                              bool merged, bool onAsync)
{
    if (!mPdnVis.buffer || !mPdnSelectPipe[0] || !mPdnShadePipe || !visPipe) return;
    const uint32_t dw = mPdn->width(), dh = mPdn->height();
    constexpr uint32_t kPassSeedStride = 601671u;
    constexpr VkAccessFlags2 kRW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    auto memBarrier = [cmd](VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da)
    {
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = ss; b.srcAccessMask = sa; b.dstStageMask = ds; b.dstAccessMask = da;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1; di.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &di);
    };
    const uint32_t frame0 = mPdnFrame & 0xFFFFFu;   // (rp.rtr[2])
    const uint32_t opts0 = (uint32_t)(rp.rtr2[2] + 0.5f);
    static const uint32_t kHdr[4] = {0u, 0u, 1u, 1u};   // (the job list's header: count 0, (0, 1, 1) workgroups)
    // (RenderVKProjStochasticAdaptive) THE ADAPTIVE PASSES: the needs-rays mask lists the tiles first, then SELECT and
    // SHADE of every extra pass run over that list (indirect: one workgroup per listed tile; VIS follows SELECT's job
    // list as always) -- the same seeds, the same fold, its own GPU marks
    const bool tiles = mPdnAdaptNow != 0u;
    if (tiles) recordProjStochMask(cmd, rp, onAsync);
    const uint32_t passes = tiles ? 1u + std::min<uint32_t>(mPdnAdaptNow, 7u) : std::min<uint32_t>(mPdnPassesNow, 8u);
    // (RenderVKProjStochasticAdaptiveSamples) the extra passes' K: the adaptive passes' own (pdnAdaptK), else pass 0's.
    // Above 4 only with the plain high-K SELECT compiled (else 4). A K other than pass 0's carries pass 0's in bits
    // 13-17 (pdnPassK0: the fold's W0 of an exact / dense pass-0 list); the same K pushes the word as before.
    uint32_t kx = tiles ? pdnAdaptK() : mPdnKNow;
    if (kx > 4u && !mPdnSelectHkPipe[0]) kx = 4u;
    const uint32_t ctlX = pdnCtlWord(kx, ((uint32_t)(rp.rtr[3] + 0.5f) >> 4) & 1u, kx != mPdnKNow ? mPdnKNow : 0u);
    const VkPipeline selX = kx > 4u ? mPdnSelectHkPipe[0] : mPdnSelectPipe[0];
    for (uint32_t pass = 1u; pass < passes; ++pass)
    {
        // the last pass's kernels -> this one: WAR on the job header (SELECT's atomics, VIS's indirect read) before its
        // reset, WAR on the descriptors / visibilities (VIS, SHADE read them), RAW on IN (SHADE wrote, this SHADE folds)
        memBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT | kRW);
        {
            LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_light_prep_setup");
            vkCmdUpdateBuffer(cmd, mPdnVis.buffer, mPdnVisOff, sizeof(kHdr), kHdr);
        }
        memBarrier(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                   kRW | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
        VKRtReflPush xp = rp;
        xp.rtr[2] = (float)(frame0 + pass * kPassSeedStride);   // (every seed and noise phase: independent passes)
        xp.rtr2[0] = 0.f;                                        // (no sun: the plain SELECT, no SUNVIS)
        xp.rtr2[2] = (float)(opts0 & ~512u);                     // (never PDN_OPT_MOVER_EXACT: the exact list is pass 0's)
        xp.rtr2[3] = 0.f;                                        // (no reservoirs: read or stored -- pass 0's alone)
        xp.rtr3[2] = rp.rtr3[2] | (pass << 16u);                 // (pdnPassIdx)
        if (tiles) xp.rtr3[2] |= 0x80000u;                       // (PDN_PASS_TILES: over the mask's tile list)
        // (2026-11-16, PDN_PASS_MULTIRAY) RenderVKProjStochasticAdaptiveSamples > 0: an exact list with budget to spare
        // (nCand < K) traces K rays over its lights, each light's mean visibility (clean_deferred_light.comp pdnNiRay's
        // block) -- within the K slots already sized (pdnVisSlotsWanted / pdnSelBytesWanted: no new memory)
        if (tiles && mPdnKx != 0u) xp.rtr3[2] |= 0x800000u;
        xp.rtr[3] = (float)ctlX;                                 // (this pass's K: pdnSamplesK / pdnPassK0)
        vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(xp), &xp);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, selX);
        if (tiles)
        {
            {
                GpuPassScope scope(*this, cmd, onAsync ? "c_defer_pstochxsel" : "g_defer_pstochxsel");
                vkCmdDispatchIndirect(cmd, mPdnVis.buffer, mPdnVisOff + pdnExactWordOff() + 4u);
            }
            gpuSubMark(cmd, onAsync ? "c_defer_pstochxsel" : "g_defer_pstochxsel");
        }
        else
        {
            {
                GpuPassScope scope(*this, cmd, onAsync ? (merged ? "c_defer_shadowsel" : "c_defer_pstochsel") : (merged ? "g_defer_shadowsel" : "g_defer_pstochsel"));
                vkCmdDispatch(cmd, (dw + 7u) / 8u, (dh + 7u) / 8u, mViewCount);   // (#1608: z = the views)
            }
            if (merged) gpuSubMark(cmd, onAsync ? "c_defer_shadowsel" : "g_defer_shadowsel");
            else        gpuSubMark(cmd, onAsync ? "c_defer_pstochsel" : "g_defer_pstochsel");
        }
        memBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                   kRW | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, visPipe);
        {
            GpuPassScope scope(*this, cmd, tiles ? (onAsync ? "c_defer_pstochxvis" : "g_defer_pstochxvis") : (onAsync ? (merged ? "c_defer_shadowvis" : "c_defer_pstochvis") : (merged ? "g_defer_shadowvis" : "g_defer_pstochvis")));
            vkCmdDispatchIndirect(cmd, mPdnVis.buffer, mPdnVisOff + 4u);
        }
        if (tiles)       gpuSubMark(cmd, onAsync ? "c_defer_pstochxvis" : "g_defer_pstochxvis");
        else if (merged) gpuSubMark(cmd, onAsync ? "c_defer_shadowvis" : "g_defer_shadowvis");
        else             gpuSubMark(cmd, onAsync ? "c_defer_pstochvis" : "g_defer_pstochvis");
        memBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kRW);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPdnShadePipe);
        if (tiles)
        {
            {
                GpuPassScope scope(*this, cmd, onAsync ? "c_defer_pstochx" : "g_defer_pstochx");
                vkCmdDispatchIndirect(cmd, mPdnVis.buffer, mPdnVisOff + pdnExactWordOff() + 4u);   // (the tile list)
            }
            gpuSubMark(cmd, onAsync ? "c_defer_pstochx" : "g_defer_pstochx");
        }
        else
        {
            {
                GpuPassScope scope(*this, cmd, onAsync ? (merged ? "c_defer_shadowmerge" : "c_defer_pstoch") : (merged ? "g_defer_shadowmerge" : "g_defer_pstoch"));
                vkCmdDispatch(cmd, (dw + 7u) / 8u, (dh + 7u) / 8u, mViewCount);   // (#1608: z = the views)
            }
            if (merged) gpuSubMark(cmd, onAsync ? "c_defer_shadowmerge" : "g_defer_shadowmerge");
            else        gpuSubMark(cmd, onAsync ? "c_defer_pstoch" : "g_defer_pstoch");
        }
    }
    // (LVC noise metric, RenderVKShadowNoiseStats) the MASK kernel's NOISE phase over the final estimate: per tile, the
    //  luminance change of every surface texel against last frame's (still camera) by the tile's class / admission / depth
    {
        bool want = false;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<bool> s_noise(gSavedSettings, "RenderVKShadowNoiseStats", false);
        want = s_noise;
#endif
        mLvcNoiseNow = want && tiles && mRegir.buffer && mPdnMaskPipe && mRsNow;
        const bool sameExtent = mLvcNoiseW == dw && mLvcNoiseH == dh;
        if (!mLvcNoiseNow || !sameExtent) mLvcNoisePrev = false;
        if (mLvcNoiseNow)
        {
            if (!mRsOccStatsNow)
            {   // (the LVC counters are zeroed with the occlusion statistics only: here without them)
                memBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kRW, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT);
                {
                    LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_light_prep_setup");
                    vkCmdFillBuffer(cmd, mRegir.buffer, kRegirLvcOffset, kRegirLvcBytes, 0u);
                }
            }
            memBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kRW);
            VKRtReflPush np = rp;
            np.rtr3[2] = (rp.rtr3[2] & ~0x01F00000u) | 0x1000000u | (mLvcNoisePrev ? 0x2000000u : 0u);
            vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(np), &np);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPdnMaskPipe);
            {
                GpuPassScope scope(*this, cmd, onAsync ? "c_lvc_noise" : "g_lvc_noise");
                vkCmdDispatch(cmd, (dw + 7u) / 8u, (dh + 7u) / 8u, mViewCount);   // (#1608: z = the views)
            }
            memBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, kRW | VK_ACCESS_2_TRANSFER_READ_BIT);
            gpuSubMark(cmd, onAsync ? "c_lvc_noise" : "g_lvc_noise");
            mLvcNoisePrev = true;
            mLvcNoiseW = dw; mLvcNoiseH = dh;
        }
    }
    if (tiles) recordProjStochAdaptStats(cmd);   // (the tile list's header -> the stats ring: before rtScratchHandoff)
    // (pass 0's push again for what follows on this layout: the counters' copies, the scratch handoff)
    vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rp), &rp);
}

// ======================= ADAPTIVE EXTRA RAYS: RenderVKProjStochasticAdaptive (0-7, nonpersistent) =======================
// Where the K-pick estimate is noisy -- penumbrae, moving casters' shadows, young histories: a small share of the screen --
// the extra passes of RenderVKProjStochasticPasses run on those 8 x 8 tiles alone, at that cost x the flagged share.
//   * THE MASK (clean_deferred_light.comp SH_STAGE_MASK, after pass 0's SHADE): every tile's class from words pass 0 and
//     last frame's denoiser already wrote (MOT: the moving-caster hit, the picks' visibility evidence, the moved lights;
//     GEOM of the other parity at the camera-only reprojection: the mover carry, the visibility prior, a pending caster
//     change, the depth that says the history is there at all) -- 3 moving caster, 2 young history, 1 penumbra. Its
//     second phase admits the classes from 3 down within the budget (RenderVKProjStochasticAdaptiveCap) and appends the
//     admitted tiles + their indirect dispatch. GPU-only: the list is consumed by vkCmdDispatchIndirect, never read by
//     the CPU in the frame.
//   * THE PASSES (recordProjStochPasses): A extra passes, each SELECT / SHADE dispatched indirect over the list (one 8 x 8
//     workgroup per tile: pdnAdaptTexel), VIS over SELECT's job list as always -- so the rays follow the flagged texels.
//     Every flagged texel takes all A passes: the fold and pdnPassBaseWeight hold unchanged (Adaptive A = Passes A + 1 on
//     those tiles); the rest keep pass 0's estimate exactly.
//   * THE HISTORY (RenderVKProjStochasticAdaptiveHistory A/B): pass 1's SHADE marks the texels (MOT w PDN_MOT_EXTRA_RAYS);
//     the temporal pass caps a marked countdown texel at cap / (1 + A) frames (clean_dn_temporal.comp).
//   * MEMORY: the tile list in the exact list's place of mPdnVis (64 B + 8 B per tile; pdnSelFrame), the stats ring 192 B.
//     Nothing carries across frames in the scratch (the host resets the header before every mask).
//   * TIMERS: c_defer_pstochmask (reset + mask + list), c_defer_pstochxsel / c_defer_pstochxvis / c_defer_pstochx (the
//     extra passes, summed by name; g_ on graphics). Compare with RenderVKProjStochasticPasses' pass-0 marks.
void LLCleanRendererVK::recordProjStochMask(VkCommandBuffer cmd, const VKRtReflPush& rp, bool onAsync)
{
    const uint32_t dw = mPdn->width(), dh = mPdn->height();
    // (#1608) every view's tiles (clean_deferred_light.comp pdnAdaptTile): the MASK z = the views, LIST over all of them,
    // ONE budget for both eyes (the cap's share of every view's tiles: the eyes settle at the same noise)
    const uint32_t tx = (dw + 7u) / 8u, ty = (dh + 7u) / 8u, tiles = tx * ty * mViewCount;
    constexpr VkAccessFlags2 kRW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    auto memBarrier = [cmd](VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da)
    {
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = ss; b.srcAccessMask = sa; b.dstStageMask = ds; b.dstAccessMask = da;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1; di.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &di);
    };
    // pass 0's SHADE (MOT / IN) and VIS (the job header's indirect read) -> the header's reset and the mask's reads
    memBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_TRANSFER_WRITE_BIT | kRW);
    // (the tile list's header: count 0, dispatch (0, 0, 1), the class counts and the admission counter 0)
    static const uint32_t kTileHdr[16] = {0u, 0u, 0u, 1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    {
        LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_light_prep_setup");
        vkCmdUpdateBuffer(cmd, mPdnVis.buffer, mPdnVisOff + pdnExactWordOff(), sizeof(kTileHdr), kTileHdr);
    }
    // (LVC stage 1) the depth admission's words past the tiles' classes (clean_deferred_light.comp pdnAdaptZBase)
    {
        LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_light_prep_setup");
        vkCmdFillBuffer(cmd, mPdnVis.buffer, mPdnVisOff + pdnExactWordOff() + (16u + 2ull * tiles) * 4u, kPdnAdZWords * 4u, 0u);
    }
    memBarrier(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kRW);
    VKRtReflPush xp = rp;
    // rtr3.z bit 21: the denoiser's history is valid (else every texel is young); rtr2.w the budget in tiles
    xp.rtr3[2] = rp.rtr3[2] | (mPdn->historyValid() ? 0x200000u : 0u);
    // rtr3.z bit 22 (PDN_AD_OLD_RULE): RenderVKProjStochasticAdaptiveRule 1 -- the penumbra class by the mixed aggregate
    // visibility (421222cb3e) instead of this frame's same-light picks disagreeing
    if (mPdnAdaptRule != 0u) xp.rtr3[2] |= 0x400000u;
    const uint32_t budget = mPdnAdaptCap == 0u ? tiles
                          : std::max<uint32_t>(1u, (uint32_t)(((uint64_t)tiles * mPdnAdaptCap) / 100u));
    xp.rtr2[3] = (float)budget;   // (exact: < 2^24 tiles)
    vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(xp), &xp);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPdnMaskPipe);
    {
        GpuPassScope scope(*this, cmd, onAsync ? "c_defer_pstochmask" : "g_defer_pstochmask");
        vkCmdDispatch(cmd, tx, ty, mViewCount);   // (MASK: one 8 x 8 workgroup per tile; #1608: z = the views)
    }
    memBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kRW);
    xp.rtr3[2] |= 0x100000u;   // (LIST)
    vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(xp), &xp);
    {
        GpuPassScope scope(*this, cmd, onAsync ? "c_defer_pstochmask" : "g_defer_pstochmask");
        vkCmdDispatch(cmd, (tiles + 63u) / 64u, 1, 1);   // (one thread per tile)
    }
    gpuSubMark(cmd, onAsync ? "c_defer_pstochmask" : "g_defer_pstochmask");
    // the list + its dispatch -> the extra passes' SELECT / SHADE (storage + indirect); the loop's own barrier follows
    memBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
               kRW | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
}

// THE ADAPTIVE LINE: this frame's tile-list header (the listed count + the class counts) copied into a 3-slot host ring,
// the slot written 3 frames ago harvested first (the rsStatsRecord pattern: never a wait), one VKShadow line a second.
// Before rtScratchHandoff (the GI's records overwrite the range later in the frame).
void LLCleanRendererVK::recordProjStochAdaptStats(VkCommandBuffer cmd)
{
    VkMemScope memcat(VKMEM_READBACK);
    if (!mCtx || !mPdn) return;
    constexpr VkDeviceSize kSlot = 64u;   // (the header's 16 words)
    const uint32_t slot = mPdnAdaptRbSlot;
    mPdnAdaptRbSlot = (mPdnAdaptRbSlot + 1u) % 3u;
    if (mPdnAdaptRbUsed[slot] && mPdnAdaptRb.mapped)
    {
        const uint32_t* w = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mPdnAdaptRb.mapped)
                                                               + (size_t)slot * kSlot);
        const uint32_t tiles = mPdnAdaptRbTiles[slot];
        const uint32_t listed = std::min(w[0], tiles);
        const uint64_t flagged = (uint64_t)w[5] + w[6] + w[7];
        mPdnAdaptAcc[0] += 1u;
        mPdnAdaptAcc[1] += tiles;
        mPdnAdaptAcc[2] += listed;
        mPdnAdaptAcc[3] += w[5];
        mPdnAdaptAcc[4] += w[6];
        mPdnAdaptAcc[5] += w[7];
        // (the mask's diagnostic words 9-11: penumbra tiles under the old mixed-visibility rule / the picks-disagree
        //  rule / the old rule's spread test alone -- whichever rule drives the list)
        mPdnAdaptAcc[6] += std::min(w[9], tiles);
        mPdnAdaptAcc[7] += std::min(w[10], tiles);
        mPdnAdaptAcc[8] += std::min(w[11], tiles);
        // (2026-11-16, words 12-14: SELECT of the extra passes, summed over them -- RenderVKProjStochasticAdaptiveSamples
        //  > 0 only) exact lists, multi-ray exact lists, their traced rays past one per light
        mPdnAdaptAcc[9] += w[12];
        mPdnAdaptAcc[10] += w[13];
        mPdnAdaptAcc[11] += w[14];
        if (listed < flagged) ++mPdnAdaptCapHits;
    }
    mPdnAdaptRbUsed[slot] = false;
    const double now = pdnClock();
    if (mPdnAdaptAcc[0] != 0u && mPdnAdaptAcc[1] != 0u && now >= mPdnAdaptLogAt)
    {
        const double t = (double)mPdnAdaptAcc[1];
        std::ostringstream os;
        os << std::fixed << std::setprecision(1) << "adaptive extra rays (RenderVKProjStochasticAdaptive " << mPdnAdaptNow
           << "): " << 100.0 * (double)mPdnAdaptAcc[2] / t << "% of " << mPdnAdaptAcc[1] / mPdnAdaptAcc[0]
           << " tiles listed; flagged -- moving caster " << 100.0 * (double)mPdnAdaptAcc[5] / t << "%, young history "
           << 100.0 * (double)mPdnAdaptAcc[4] / t << "%, penumbra " << 100.0 * (double)mPdnAdaptAcc[3] / t
           << "% (rule " << (mPdnAdaptRule ? "1 mixed visibility" : "0 picks disagree") << "; penumbra by rule -- "
           << "old mixed visibility " << 100.0 * (double)mPdnAdaptAcc[6] / t << "% (spread alone "
           << 100.0 * (double)mPdnAdaptAcc[8] / t << "%), picks disagree " << 100.0 * (double)mPdnAdaptAcc[7] / t
           << "%); picks " << mPdnKNow << " pass 0 / " << pdnAdaptK() << " extra";
        if (mPdnKx != 0u)   // (the multi-ray exact lists: AdaptiveSamples > 0 only -- per extra pass, per texel)
        {
            const double xl = (double)std::max<uint64_t>(mPdnAdaptAcc[9], 1u);
            os << " (exact lists " << (double)mPdnAdaptAcc[9] / (double)mPdnAdaptAcc[0] << "/frame, multi-ray "
               << 100.0 * (double)mPdnAdaptAcc[10] / xl << "% of them, +"
               << (double)mPdnAdaptAcc[11] / (double)std::max<uint64_t>(mPdnAdaptAcc[10], 1u)
               << " rays past one per light each)";
        }
        os << "; cap "
           << (mPdnAdaptCap ? std::to_string(mPdnAdaptCap) + "%" : std::string("none")) << " cut " << mPdnAdaptCapHits
           << " of " << mPdnAdaptAcc[0] << " frames (" << mPdnAdaptAcc[0] << " frames, 3 frames late, no wait)";
        LL_INFOS("VKShadow") << os.str() << LL_ENDL;
        std::fill(std::begin(mPdnAdaptAcc), std::end(mPdnAdaptAcc), 0u);
        mPdnAdaptCapHits = 0u;
        mPdnAdaptLogAt = now + 60.0;   // (log volume: a once-a-minute summary)
    }
    if (!mPdnAdaptRb.buffer)
    {
        if (now < mPdnAdaptRbRetryAt) return;
        VkDeviceContext::SoftAllocScope soft(mCtx);
        mPdnAdaptRb = mCtx->createBuffer(3u * kSlot, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!mPdnAdaptRb.buffer)
        {
            mPdnAdaptRbRetryAt = now + 30.0;   // (#697: the line pauses; the adaptive passes run on)
            return;
        }
    }
    if (!mPdnAdaptRb.mapped) return;
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; m.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &d);
    const VkBufferCopy ac{mPdnVisOff + pdnExactWordOff(), (VkDeviceSize)slot * kSlot, kSlot};
    {
        LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_light_prep_setup");
        vkCmdCopyBuffer(cmd, mPdnVis.buffer, mPdnAdaptRb.buffer, 1u, &ac);
    }
    mPdnAdaptRbUsed[slot] = true;
    mPdnAdaptRbTiles[slot] = ((mPdn->width() + 7u) / 8u) * ((mPdn->height() + 7u) / 8u) * mViewCount;   // (#1608: every view's)
}

// ======================= ReSTIR DI RESERVOIRS (#869 Phase 5b / 5c) =======================
// A sampling frame (pdnFrame, the denoiser allocated at its extent): the reservoir buffer at that
// extent -- created once per extent (behind a device wait: set 3 may be bound by frames in flight),
// freed with the denoiser's images; a refused allocation (#697) retries after 30 s, meanwhile pass
// 15 runs on this frame's grid candidates alone (no temporal / spatial reuse: noisier, same cost).
void LLCleanRendererVK::rsResFrame()
{
    if (!mCtx || !mPdn || !mPdn->allocated()) return;
    const uint32_t w = mPdn->width(), h = mPdn->height();
    if (mRsRes.buffer && mRsResW == w && mRsResH == h) return;
    const double now = pdnClock();
    if (now < mRsResRetryAt) return;
    // (#1608: every view's texels -- clean_deferred_light.comp rsResIndex; the view count is fixed for the session)
    const VkDeviceSize bytes = (VkDeviceSize)w * h * mViewCount * 2u * 8u;
    vkDeviceWaitIdle(mCtx->device());
    if (mRsRes.buffer) { mCtx->destroyBuffer(mRsRes); mRsRes = GpuBuffer{}; }
    mRsResW = mRsResH = 0;
    mRsResPrevOk = false;
    {
        VkMemScope memcat(VKMEM_SCENE);   // #407 (with the lights / grid)
        VkDeviceContext::SoftAllocScope soft(mCtx);
        mRsRes = mCtx->createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    if (!mRsRes.buffer)
    {
        mRsResRetryAt = now + 30.0;
        LL_WARNS("Vulkan") << "ReSTIR DI reservoirs: " << ((bytes + (1u << 19)) >> 20) << " MB refused (VRAM); no"
                              " temporal / spatial reuse (grid candidates only) -- retry in 30 s" << LL_ENDL;
        writeRsResBinding();
        return;
    }
    mCtx->setObjectName(VK_OBJECT_TYPE_BUFFER, (uint64_t)mRsRes.buffer, "restirReservoirs");
    mRsResW = w; mRsResH = h;
    writeRsResBinding();
    LL_INFOS("Vulkan") << "ReSTIR DI reservoirs: " << w << "x" << h << (mViewCount > 1u ? " x " + std::to_string(mViewCount)
                                                                                           + " views" : std::string())
                       << " x 2 parities, " << ((bytes + (1u << 19)) >> 20) << " MB" << LL_ENDL;
}

void LLCleanRendererVK::writeRsResBinding()
{
    if (!mCtx || !mDsetDeferSplit) return;
    const VkBuffer b = mRsRes.buffer ? mRsRes.buffer : mRsResDummy.buffer;
    if (!b) return;
    const VkDescriptorBufferInfo bi{b, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = mDsetDeferSplit; w.dstBinding = kDsBindingRsRes; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(mCtx->device(), 1, &w, 0, nullptr);
}

// ======================= THE SHADOW CORE SPLIT (#1031) =======================
// RenderVKShadowSplitKernels: the generic core (pass 15 -DCLEAN_SHADOW_CORE) as SELECT -> SUNVIS (merged)
// + VIS -> SHADE over a 64 B selection descriptor per denoiser texel (mPdnSel, set 3 binding 26) and the
// light samples' visibilities (mPdnVis, binding 27: pdnVisSlotsWanted() x 4 B per texel). Allocated on a
// sampling frame at the denoiser's extent (rsResFrame's rule: syncFromStoresPrestep, behind a device
// wait), freed with the denoiser's images or the setting; a refusal (#697) runs the fused core, retry in
// 30 s; a settings change that lengthens the sample list runs the fused core until the next prestep.
bool LLCleanRendererVK::pdnSplitWanted() const
{
    bool on = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKShadowSplitKernels", true);
    on = s_on;
#endif
    return on && mPdnSelectPipe[0] && mPdnSelectPipe[1] && mPdnSunVisPipe && mPdnVisPipe && mPdnShadePipe;
}

// The longest light-sample list a texel can have under the settings (clean_deferred_light.comp: nProj +
// spN + the probe): the K picks or the exact list (<= K), or the dense window (RenderVKReSTIRMoveLights,
// the header's rs_cfg.w bits 8-11) + the emitter stratum; + the split lights; + the occlusion probe.
uint32_t LLCleanRendererVK::pdnVisSlotsWanted() const
{
    U32 moveCut = 1, moveLights = 4;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_mcut(gSavedSettings, "RenderVKReSTIRMoveCut", 1U);
    static LLCachedControl<U32> s_mlights(gSavedSettings, "RenderVKReSTIRMoveLights", 4U);
    moveCut = s_mcut; moveLights = s_mlights;
#endif
    const uint32_t dense = moveCut != 0u ? std::min<uint32_t>(moveLights, kRegirResMax - 1u) + 1u : 0u;
    // (K <= 16, dense <= kRegirResMax: <= 16 + 4 + 1 = 21 slots -- PDN_JOB_SLOT_BITS 5 and the gate's 28 bits hold them)
    return std::max<uint32_t>(std::clamp<U32>(pdnKMax(), 1u, kPdnKMax), dense) + std::min<U32>(std::max(mPdnSplitK, mPdnExactSpecular ? 1u : 0u), 4u) + 1u;
}

// (RenderVKProjStochasticAdaptiveSamples) the adaptive extra passes' K: their own, else pass 0's
uint32_t LLCleanRendererVK::pdnAdaptK() const
{
    return std::clamp<uint32_t>(mPdnKx != 0u ? mPdnKx : mPdnK, 1u, kPdnKMax);
}

// The largest K of any dispatch the settings ask for: pass 0's, and the adaptive passes' while they are wanted (the
// descriptors' extensions and the visibility slots are sized for it)
uint32_t LLCleanRendererVK::pdnKMax() const
{
    return std::max<uint32_t>(mPdnK, mPdnAdapt > 0u ? pdnAdaptK() : 0u);
}

// clean_deferred_light.comp pdnSamplesK / pdnPassK0: bits 0-2 min(K, 4) (every reader that stops at 4), bit 4 the GEOM
// parity, bits 8-12 K when above 4 (the split core with the second extension), bits 13-17 pass 0's K (an extra pass
// whose K differs; 0 = the same). K <= 4 with k0 0: `K | parity << 4`, the word as it always was. < 2^18: exact in a float.
uint32_t LLCleanRendererVK::pdnCtlWord(uint32_t k, uint32_t parity, uint32_t k0)
{
    k = std::clamp<uint32_t>(k, 1u, kPdnKMax);
    return std::min<uint32_t>(k, 4u) | ((parity & 1u) << 4) | (k > 4u ? (k << 8) : 0u)
         | (std::min<uint32_t>(k0, kPdnKMax) << 13);
}

// The selection descriptors (clean_deferred_light.comp THE SELECTION DESCRIPTORS): 4 uvec4 per texel, + one uvec4 per
// texel past them (pdnSelExtIndex: picks 2-3 and their targets, or the exact list's 3rd / 4th candidate) while K > 2,
// + (K > 4) 5 uvec4 per texel past those (pdnSelExt2Index: picks 4..15 / candidates 5..16).
// K <= 2 keeps the 64 B layout as it was. SELECT reads the binding's length: a buffer without the extension runs K = 2,
// one without the second extension K <= 4.
VkDeviceSize LLCleanRendererVK::pdnSelBytesWanted(uint32_t w, uint32_t h) const
{
    const uint32_t k = pdnKMax();
    // (#1608: every view's texels -- clean_deferred_light.comp pdnTexels)
    return (VkDeviceSize)w * h * mViewCount * (k > 4u ? 160u : (k > 2u ? 80u : 64u));
}

void LLCleanRendererVK::pdnSelFree()
{
    // (the scratch keeps the GI's records alone -- re-laid out, so the shadow core's share is freed)
    if (mPdnSel.buffer || mPdnVis.buffer) rtScratchLayout(mRtScrGiBytes, mRtScrGiKeep, 0u, 0u);
    mPdnSelW = mPdnSelH = mPdnVisSlots = 0;
    mPdnVisExact = false;
    mPdnVisTiles = false;
    writePdnSelBinding();
}

// (RenderVKShadowMoverMask 2) the exact job list's want: the setting's mode 2 in radiance mode (the routing itself is
// decided per frame -- the allocation does not follow TLAS / merge flaps)
bool LLCleanRendererVK::pdnExactWanted() const
{
    U32 mode = 1;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_mover(gSavedSettings, "RenderVKShadowMoverMask", 0U);
    mode = s_mover;
#endif
    return mode == 2u && mPdnRadiance && pdnSplitWanted();
}

// clean_deferred_light.comp pdnExactBase: the exact list's header word = 4 + 2 N S (past the light-sample jobs and their
// visibilities), in bytes from mPdnVisOff
VkDeviceSize LLCleanRendererVK::pdnExactWordOff() const
{
    return 16u + (VkDeviceSize)mPdnSelW * mPdnSelH * mViewCount * mPdnVisSlots * 8u;   // (#1608: every view's texels)
}

// (RenderVKProjStochasticAdaptive) the adaptive tile list's want: the setting in radiance mode, never beside the exact list
// (it takes the exact list's place -- clean_deferred_light.comp pdnAdaptBase)
bool LLCleanRendererVK::pdnTilesWanted() const
{
    return mPdnAdapt > 0u && mPdnRadiance && !pdnExactWanted();
}

// clean_deferred_light.comp THE ADAPTIVE TILE LIST: 16 header words + the list + the tiles' classes, a word per 8 x 8 tile each
VkDeviceSize LLCleanRendererVK::pdnTilesBytes(uint32_t w, uint32_t h) const
{
    const VkDeviceSize tiles = (VkDeviceSize)((w + 7u) / 8u) * ((h + 7u) / 8u) * mViewCount;   // (#1608: every view's)
    return (16u + 2u * tiles + kPdnAdZWords) * 4u;   // (+ LVC stage 1: the depth admission's histogram, PDN_AD_ZWORDS)
}

// THE MOVER COUNTDOWN's length and the exact texels' history cap (RenderVKShadowMoverMask 2): the sun channel's own
// moving-caster settings (recordProjStochTail's sun block reads the same two)
uint32_t LLCleanRendererVK::moverDynMaxHistory() const
{
    U32 dmax = 4;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_sdmax(gSavedSettings, "RenderVKRTShadowDynamicMaxHistory", 4U);
    dmax = s_sdmax;
#endif
    return std::min<U32>(dmax, 15u);
}
uint32_t LLCleanRendererVK::moverCountdownLen() const
{
    U32 persist = 4;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_spersist(gSavedSettings, "RenderVKRTShadowMoverPersist", 4U);
    persist = s_spersist;
#endif
    return std::max<uint32_t>(std::max<uint32_t>(std::min<U32>(persist, 7u), moverDynMaxHistory()), 1u);
}

// ======================= THE RT LIGHTING SCRATCH =======================
// llcleanrenderervk.h has the contract. WHY THE SHARING IS SAFE (every recording order):
//   * the chain (async q_defer_light or graphics): the light prep -> pass 15 (merged first, or after the RT
//     reflections) -> recordGI; the M6 frame walk records the GI's TRACE in the walk, after recordProjStoch /
//     recordShadowMergeFirst; the one-frame-latency GI (q_gi_lag) is forked after the combine, which the head (pass
//     15) was joined before; the GI early trace stops the chain after recordGI. Pass 15 is the only client whose
//     ranges are written before the GI's first record, and rtScratchHandoff orders its last access before them.
//   * across frames: last frame's GI accesses -> this frame's light prep (the job-list header): the light prep's
//     opening barrier on its queue, the fork / join semaphores between queues (as for every buffer they share).
//   * what lives ACROSS pass 15 in the GI's range -- the list headers (light prep), the change tags (light prep's
//     CHG-TAG, read by REUSE) -- is the GI's KEPT PREFIX (clean_deferred_light.comp giChgTagWord / giRecIndex): the
//     shadow core's ranges start after it. Nothing in either client's range carries over to the next frame.
//   * VR / stereo (#1608): both clients run per view in one recording -- the same order (pass 15 first, then the
//     GI's records), every range sized for every view's texels; the kept prefix / handoff rules unchanged.
bool LLCleanRendererVK::rtScratchLayout(VkDeviceSize giBytes, VkDeviceSize giKeep, VkDeviceSize selBytes,
                                        VkDeviceSize visBytes)
{
    if (!mCtx) return false;
    const bool pdn = selBytes != 0u && visBytes != 0u;
    if (!pdn) selBytes = visBytes = 0u;
    if (giBytes == 0u) giKeep = 0u;
    VkDeviceSize selOff = 0u, visOff = 0u;
    const VkDeviceSize total = rtScratchPlace(giBytes, giKeep, selBytes, visBytes, &selOff, &visOff);
    const bool same = mRtScrGiBytes == giBytes && mRtScrGiKeep == giKeep && mPdnSelBytes == selBytes
                   && mPdnVisBytes == visBytes && (total == 0u || mRtScratch.buffer != VK_NULL_HANDLE);
    if (same) return total != 0u;
    vkDeviceWaitIdle(mCtx->device());   // (set 3 b25-27 + the buffer may be used by frames in flight)
    const VkDeviceSize oldBytes = mRtScratch.buffer ? mRtScratch.size : 0u;
    // (under texture pressure a smaller layout keeps the buffer -- only the views / bindings move: no allocation while
    //  pressure is set, and a GI re-admission at the old total re-uses it; the GI's share of it is its kept prefix +
    //  whatever its records exceed the shadow core by, so a pressure free of the GI would release only that)
    const bool keepBig = mTexPressure && total != 0u && mRtScratch.buffer && total <= mRtScratch.size;
    if (mRtScratch.buffer && mRtScratch.size != total && !keepBig) { mCtx->destroyBuffer(mRtScratch); mRtScratch = GpuBuffer{}; }
    if (total != 0u && !mRtScratch.buffer)
    {
        VkMemScope memcat(VKMEM_DENOISE);
        VkDeviceContext::SoftAllocScope soft(mCtx);
        // (the GI: storage + its lists' indirect dispatches + the light prep's header updates; pass 15: the same +
        //  the ReSTIR counters' copy of the job count -- transfer source)
        mRtScratch = mCtx->createBuffer(total, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
                                                   | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mRtScratch.buffer) mCtx->setObjectName(VK_OBJECT_TYPE_BUFFER, (uint64_t)mRtScratch.buffer, "rtLightingScratch");
    }
    const bool ok = total == 0u || mRtScratch.buffer != VK_NULL_HANDLE;
    if (!ok)
    {
        // #697: never a crash -- both clients take their fused kernels, each retries in 30 s
        LL_WARNS("Vulkan") << "RT lighting scratch: " << ((total + (1u << 19)) >> 20) << " MB (GI records "
                           << ((giBytes + (1u << 19)) >> 20) << " MB, split shadow core "
                           << ((selBytes + visBytes + (1u << 19)) >> 20) << " MB) refused (VRAM); the fused kernels, retry in 30 s"
                           << LL_ENDL;
        const double retry = pdnClock() + 30.0;   // (giClock: the same steady clock)
        if (giBytes) mGiHitRetryAt = retry;
        if (pdn) mPdnSelRetryAt = retry;
        giBytes = giKeep = selBytes = visBytes = 0u;
        mGiHitW = mGiHitH = 0;
        mPdnSelW = mPdnSelH = mPdnVisSlots = 0;
        mPdnVisExact = false;
        mPdnVisTiles = false;
    }
    mRtScrGiBytes = giBytes;
    mRtScrGiKeep = giKeep;
    mPdnSelBytes = selBytes;
    mPdnVisBytes = visBytes;
    mPdnSelOff = ok ? selOff : 0u;
    mPdnVisOff = ok ? visOff : 0u;
    mGiHit = giBytes ? mRtScratch : GpuBuffer{};
    mPdnSel = selBytes ? mRtScratch : GpuBuffer{};
    mPdnVis = visBytes ? mRtScratch : GpuBuffer{};
    if (giBytes == 0u) mGiHitW = mGiHitH = 0;
    if (selBytes == 0u) { mPdnSelW = mPdnSelH = mPdnVisSlots = 0; mPdnVisExact = false; mPdnVisTiles = false; }
    if (total == 0u && mRtScratch.buffer) { mCtx->destroyBuffer(mRtScratch); mRtScratch = GpuBuffer{}; }
    writeGiBindings();
    writePdnSelBinding();
    if (ok && total != oldBytes)
        LL_INFOS("Vulkan") << "RT lighting scratch: " << ((total + (1u << 19)) >> 20) << " MB = max(GI records "
                           << ((giBytes + (1u << 19)) >> 20) << " MB, split shadow core "
                           << (((pdn ? visOff + visBytes : 0u) + (1u << 19)) >> 20) << " MB incl. the GI's kept prefix "
                           << ((selOff + (1u << 19)) >> 20) << " MB) -- separately "
                           << ((giBytes + selBytes + visBytes + (1u << 19)) >> 20) << " MB" << LL_ENDL;
    return ok && total != 0u;
}

void LLCleanRendererVK::rtScratchHandoff(VkCommandBuffer cmd)
{
    if (!mRtScrGiBytes || !mPdnSel.buffer) return;
    // pass 15's SELECT / VIS / SHADE (storage) and the job count's copy (transfer read) -> the GI's TRACE / list /
    // record writes over the same range, later on this queue (WAR + WAW)
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT
                   | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                    | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &d);
}

void LLCleanRendererVK::pdnSelFrame()
{
    if (!mCtx || !mPdn || !mPdn->allocated()) return;
    const uint32_t w = mPdn->width(), h = mPdn->height();
    if (!pdnSplitWanted())
    {
        if (mPdnSel.buffer || mPdnVis.buffer)
        {
            vkDeviceWaitIdle(mCtx->device());
            pdnSelFree();
        }
        return;
    }
    const uint32_t slots = pdnVisSlotsWanted();
    const double now = pdnClock();
    // (RenderVKShadowMoverMask 2) THE MOVER-EXACT JOB LIST past the visibilities: wanted with mode 2 in radiance mode,
    // released otherwise; a refused one waits 30 s (#697) while the split core keeps running without it
    const bool exactWant = pdnExactWanted() && (mPdnVisExact || now >= mPdnExactRetryAt);
    // (RenderVKProjStochasticAdaptive) THE ADAPTIVE TILE LIST in the exact list's place: never both (pdnTilesWanted)
    const bool tilesWant = !exactWant && pdnTilesWanted() && (mPdnVisTiles || now >= mPdnTilesRetryAt);
    const VkDeviceSize selBytes = pdnSelBytesWanted(w, h);
    if (mPdnSel.buffer && mPdnVis.buffer && mPdnSelW == w && mPdnSelH == h && mPdnVisSlots == slots
        && mPdnVisExact == exactWant && mPdnVisTiles == tilesWant && mPdnSelBytes == selBytes) return;
    if (now < mPdnSelRetryAt) return;
    // (the job list header, the jobs, the visibilities; + the exact list: header, then a job word and a visibility per
    //  denoiser texel -- the render extent's w x h, never grown; #1608: every view's texels, clean_deferred_light.comp
    //  pdnTexels)
    const VkDeviceSize texels = (VkDeviceSize)w * h * mViewCount;
    const VkDeviceSize exactBytes = 16u + texels * 8u;
    const VkDeviceSize visBytes = 16u + texels * slots * 8u;
    // (THE RT LIGHTING SCRATCH: on the GI records' range -- rtScratchLayout waits for the device)
    bool exact = exactWant;
    // (the GI client's share as it stands: a refused layout drops it -- the fallback below asks for it again)
    const VkDeviceSize giBytes = mRtScrGiBytes, giKeep = mRtScrGiKeep;
    const double giRetry = mGiHitRetryAt;
    const uint32_t giW = mGiHitW, giH = mGiHitH;
    bool tiles = tilesWant && !exact;
    const VkDeviceSize tilesBytes = pdnTilesBytes(w, h);
    bool ok = rtScratchLayout(giBytes, giKeep, selBytes, visBytes + (exact ? exactBytes : 0u) + (tiles ? tilesBytes : 0u))
           && mPdnSel.buffer;
    if (!ok && exact)
    {
        // (#697: never a crash -- the exact list is dropped, mode 2 acts as 1, retry in 30 s; the split core alone next,
        //  the GI's records with it as before: the exact list's refusal costs neither client its range)
        LL_WARNS("Vulkan") << "RenderVKShadowMoverMask 2: the mover-exact job list (" << ((exactBytes + (1u << 19)) >> 20)
                           << " MB) was refused (VRAM); mode 2 acts as 1 -- retry in 30 s" << LL_ENDL;
        mPdnExactRetryAt = now + 30.0;
        mPdnSelRetryAt = 0.0;
        mGiHitRetryAt = giRetry;
        exact = false;
        ok = rtScratchLayout(giBytes, giKeep, selBytes, visBytes) && mPdnSel.buffer;
        if (ok && giBytes != 0u && mRtScrGiBytes == giBytes) { mGiHitW = giW; mGiHitH = giH; }
    }
    if (!ok && tiles)
    {
        // (#697: the adaptive passes are dropped -- the split core without them, retry in 30 s)
        LL_WARNS("Vulkan") << "RenderVKProjStochasticAdaptive: the adaptive tile list (" << ((tilesBytes + 1023u) >> 10)
                           << " KB) was refused (VRAM); no adaptive passes -- retry in 30 s" << LL_ENDL;
        mPdnTilesRetryAt = now + 30.0;
        mPdnSelRetryAt = 0.0;
        mGiHitRetryAt = giRetry;
        tiles = false;
        ok = rtScratchLayout(giBytes, giKeep, selBytes, visBytes) && mPdnSel.buffer;
        if (ok && giBytes != 0u && mRtScrGiBytes == giBytes) { mGiHitW = giW; mGiHitH = giH; }
    }
    if (!ok)
    {
        mPdnSelW = mPdnSelH = mPdnVisSlots = 0;
        mPdnVisExact = false;
        mPdnSelRetryAt = now + 30.0;
        LL_WARNS("Vulkan") << "split shadow core: the selection descriptors + visibilities ("
                           << ((selBytes + visBytes + (1u << 19)) >> 20)
                           << " MB) were refused (VRAM); the fused core instead -- retry in 30 s" << LL_ENDL;
        return;
    }
    mPdnSelW = w; mPdnSelH = h;
    mPdnVisSlots = slots;
    mPdnVisExact = exact;
    mPdnVisTiles = tiles;
    if (tiles)
        LL_INFOS("Vulkan") << "RenderVKProjStochasticAdaptive: adaptive tile list " << ((w + 7u) / 8u) << "x" << ((h + 7u) / 8u)
                           << " tiles, " << ((tilesBytes + 1023u) >> 10) << " KB (in the split core's visibility range)"
                           << LL_ENDL;
    if (exact)
        LL_INFOS("Vulkan") << "RenderVKShadowMoverMask 2: mover-exact job list " << w << "x" << h << " jobs, "
                           << ((exactBytes + (1u << 19)) >> 20) << " MB (in the split core's visibility range)" << LL_ENDL;
    LL_INFOS("Vulkan") << "split shadow core (RenderVKShadowSplitKernels): " << w << "x" << h
                       << (mViewCount > 1u ? " x " + std::to_string(mViewCount) + " views" : std::string()) << ", descriptors "
                       << ((selBytes + (1u << 19)) >> 20) << " MB + light-sample jobs / visibilities (" << slots << " slots) "
                       << ((visBytes + (1u << 19)) >> 20) << " MB"
                       << (mRtScrGiBytes ? " (on the GI records' range of the RT lighting scratch)" : "") << LL_ENDL;
}

void LLCleanRendererVK::writePdnSelBinding()
{
    if (!mCtx || !mDsetDeferSplit) return;
    const VkBuffer s = mPdnSel.buffer ? mPdnSel.buffer : mRsResDummy.buffer;
    const VkBuffer v = mPdnVis.buffer ? mPdnVis.buffer : mRsResDummy.buffer;
    if (!s || !v) return;
    // (their ranges of the RT lighting scratch; the placeholder whole)
    const VkDescriptorBufferInfo bi[2] = {
        {s, mPdnSel.buffer ? mPdnSelOff : 0u, mPdnSel.buffer ? mPdnSelBytes : VK_WHOLE_SIZE},
        {v, mPdnVis.buffer ? mPdnVisOff : 0u, mPdnVis.buffer ? mPdnVisBytes : VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[2]{};
    for (uint32_t i = 0; i < 2; ++i)
    {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = mDsetDeferSplit; w[i].dstBinding = i == 0 ? kDsBindingPdnSel : kDsBindingPdnVis;
        w[i].descriptorCount = 1; w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(mCtx->device(), 2, w, 0, nullptr);
}

// ======================= POINT LIGHT SHADOW MAPS (#439, the non-RT fallback) =======================
// On the RT tier point lights are shadowed by ReSTIR DI (RenderVKReSTIRLights 1: every point light, a
// soft disc, the own-fixture rule). Cards without ray queries get CUBE SHADOW MAPS for the most
// important few. LANDED HERE: the per-frame SELECTION and its VRAM BUDGET (RenderVKPointShadowMaps 1,
// default 0) -- what the render path will consume -- logged on change. NOT LANDED (the plan; it
// shares the projector atlas's machinery, which #567 is reworking -- every change there additive):
//   1. STORAGE: one D32 cube-map array per resolution tier (512 / 256 / 128, by apparent size), a
//      static layer + a dynamic overlay per light (the projector atlas's static-cache split, #567:
//      kShadowSplitStatic / kShadowSplitDynamic), allocated on first use under the VRAM ladder's
//      STEP_PROJ_SHADOWS rung (+ its own rung below it), freed after a grace period unused.
//   2. CULL: ONE compute cull for every face of every selected light (clean_cull.comp's caster mode
//      with a sphere of the light's radius -- <= 20 m in SL -- instead of a frustum, writing 6 face
//      bits per instance), the casters' own light excluded (the instance box holding the light and
//      under RenderVKReSTIRFixtureSize: the RT path's own-fixture rule, as data).
//   3. RENDER: one layered multi-draw-indirect per tier (gl_Layer = light x 6 + face, the shadow
//      pipelines' depth-only VS + a linear-distance FS), static layer only when something static in
//      range changed (gVKChangedSpheres, the atlas's rule), the dynamic overlay on a budget
//      (RenderVKPointShadowDynamicInterval) -- a per-frame cap on face renders.
//   4. LOOKUP: localLight's point branch (the lit FS / deferred kernels) samples the cube by
//      direction with normal-offset + slope bias and a jittered 4-tap PCF; record index in the light's
//      tex.z like a projector's (a point light's record = its tier + layer), so the same "no record =
//      unshadowed" rule holds and the ReSTIR set (rsLightSet) keeps excluding mapped lights.
// Selection: brightness x apparent size (the projectors' score), 1.5 x hysteresis for last frame's,
// attached lights only with RenderVKReSTIRAttachedLights, inert / degenerate records never; only
// while the RT tier does not shadow point lights already.
// (LVC stage 3b) a cube face's shadow-view key (never 0) and its world -> VK clip matrix (column-major, depth 0..1: the
// projector maps' convention, llappviewer.cpp shadow_vp): a 90-degree view from the light along face f's axis
// (f = axis x 2 + negative: +x -x +y -y +z -z), any up (the lookup uses the record's own matrix)
uint64_t LLCleanRendererVK::psmFaceKey(uint64_t lightKey, uint32_t face)
{
    const uint64_t k = (lightKey * 0x9E3779B97F4A7C15ull) ^ (0xC0BE000000000000ull | (uint64_t)(face + 1u));
    return k != 0u ? k : 1u;
}
void LLCleanRendererVK::psmFaceViewProj(const float* pos, uint32_t face, float nearD, float farD, float* out)
{
    const uint32_t ax = face >> 1u;
    const float sgn = (face & 1u) ? -1.f : 1.f;
    float f[3] = {0.f, 0.f, 0.f};
    f[ax] = sgn;
    float up[3] = {0.f, 0.f, 1.f};
    if (ax == 2u) { up[0] = 0.f; up[1] = 1.f; up[2] = 0.f; }
    // s = f x up, u = s x f (right-handed look-at, the eye looking down -Z)
    float sx[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
    const float sl = std::sqrt(sx[0] * sx[0] + sx[1] * sx[1] + sx[2] * sx[2]);
    for (float& c : sx) c /= sl;
    const float u[3] = {sx[1] * f[2] - sx[2] * f[1], sx[2] * f[0] - sx[0] * f[2], sx[0] * f[1] - sx[1] * f[0]};
    float V[16] = {sx[0], u[0], -f[0], 0.f,
                   sx[1], u[1], -f[1], 0.f,
                   sx[2], u[2], -f[2], 0.f,
                   -(sx[0] * pos[0] + sx[1] * pos[1] + sx[2] * pos[2]),
                   -(u[0] * pos[0] + u[1] * pos[1] + u[2] * pos[2]),
                   (f[0] * pos[0] + f[1] * pos[1] + f[2] * pos[2]), 1.f};
    // the GL perspective (fov 90, aspect 1) then gl_to_vk (y flipped, z to 0..1): column-major
    float P[16] = {};
    P[0] = 1.f; P[5] = 1.f;
    P[10] = -(farD + nearD) / (farD - nearD); P[11] = -1.f;
    P[14] = -(2.f * farD * nearD) / (farD - nearD);
    float G[16] = {1.f, 0.f, 0.f, 0.f,  0.f, -1.f, 0.f, 0.f,  0.f, 0.f, 0.5f, 0.f,  0.f, 0.f, 0.5f, 1.f};
    auto mul = [](const float* a, const float* b, float* o)   // o = a * b (column-major 4 x 4)
    {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
            {
                float v = 0.f;
                for (int k = 0; k < 4; ++k) v += a[k * 4 + r] * b[c * 4 + k];
                o[c * 4 + r] = v;
            }
    };
    float GP[16];
    mul(G, P, GP);
    mul(GP, V, out);
}

void LLCleanRendererVK::pointShadowMapSelect(const LLVKWorldFrame& wf)
{
    U32 mode = 0, maxN = 4;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_mode(gSavedSettings, "RenderVKPointShadowMaps", 0U);
    static LLCachedControl<U32> s_max(gSavedSettings, "RenderVKPointShadowMapMax", 4U);
    mode = s_mode; maxN = s_max;
#endif
    mPsmSel.clear();
    mPsmBytes = 0;
    const bool rtCovers = mProjMode == 2u && mRsExt;   // (ReSTIR shadows them)
    if (mode != 0u && !rtCovers && maxN > 0u)
    {
        const float* cam = wf.cam_pos;
        const size_t n = std::min<size_t>(mLightsCPU.size(), std::min<size_t>(wf.lights.size(), 256));
        for (size_t i = 0; i < n; ++i)
        {
            const VKLightGPU& g = mLightsCPU[i];
            if (g.proj_origin[3] >= 0.5f || !(g.pos_radius[3] > 0.f) || g.pos_radius[2] < -1.0e8f) continue;
            if (wf.lights[i].proj_axis[1] >= 0.5f && !mRsAttached) continue;   // (attached: facelights)
            const float dx = g.pos_radius[0] - cam[0], dy = g.pos_radius[1] - cam[1], dz = g.pos_radius[2] - cam[2];
            const float dist = std::max(std::sqrt(dx * dx + dy * dy + dz * dz), 0.1f);
            const float lum = 0.2126f * g.color_falloff[0] + 0.7152f * g.color_falloff[1] + 0.0722f * g.color_falloff[2];
            const float size = g.pos_radius[3] / dist;
            float score = lum * std::min(size, 8.f);
            if (std::binary_search(mPsmKeys.begin(), mPsmKeys.end(), wf.lights[i].key)) score *= 1.5f;
            if (!std::isfinite(score) || !(score > 0.f)) continue;
            const uint32_t res = size > 1.f ? 512u : (size > 0.25f ? 256u : 128u);
            mPsmSel.push_back({(uint32_t)i, wf.lights[i].key, res, score});
        }
        std::sort(mPsmSel.begin(), mPsmSel.end(), [](const PsmSel& a, const PsmSel& b) {
            return a.score != b.score ? a.score > b.score : a.light < b.light; });
        if (mPsmSel.size() > maxN) mPsmSel.resize(maxN);
        for (const PsmSel& s : mPsmSel) mPsmBytes += 2ull * 6ull * s.res * s.res * 4ull;   // static + dynamic, D32
    }
    // (LVC stage 3b, #439 completion) THE CUBE FACES as #567 shadow views: this frame's records for the faces queued last
    // frame (selectProjShadows consumed them above) tag each light whose six faces all hold a tile -- tex.z = PSM_CUBE |
    // six 5-bit record indices (clean_draw_common.glsl pointCubeVisibility); a light short of a face stays unshadowed this
    // frame. Then the six faces of this frame's selection are queued for the next selectProjShadows: 90-degree views from
    // the light, near = its own prim's sphere (>= 5 cm: the fixture's inside never shadows it), far = its radius, scored
    // like a projector so faces and projectors share RenderVKProjShadowMax by importance. The atlas brings the rest
    // (static cache + dynamic overlay, change-sphere / mover / avatar invalidation, the re-render budget).
    static_assert(kMaxProjShadows <= 32u, "PSM_CUBE packs six 5-bit record indices");
    mPsmMapped = 0u;
    mPsmCubes = 0u;
    if (mode != 0u && !rtCovers && mProjMode == 1u)
    {
        const size_t nl = std::min<size_t>(mLightsCPU.size(), wf.lights.size());
        for (const PsmSel& s : mPsmSel)
        {
            if (s.light >= nl || wf.lights[s.light].key != s.key) continue;
            uint32_t word = 0x80000000u;
            bool all = true;
            for (uint32_t f = 0; f < 6u && all; ++f)
            {
                const int32_t r = shadowViewRecord(psmFaceKey(s.key, f));
                if (r < 0 || r >= (int32_t)kMaxProjShadows) all = false;
                else word |= (uint32_t)r << (5u * f);
            }
            if (all) { mLightsCPU[s.light].tex[2] = word; ++mPsmMapped; }
        }
        // (a light needs all six faces: queue only as many cubes as the atlas's tiles can hold -- RenderVKProjShadowMax,
        //  at most the atlas's cells -- the highest-scored first; a face's score steps down by a hair per face so one
        //  light's six faces sort together and the tiles complete whole cubes instead of interleaving lights)
        U32 tileMax = 16;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<U32> s_tiles(gSavedSettings, "RenderVKProjShadowMax", 16U);
        tileMax = s_tiles;
#endif
        const uint32_t tiles = std::min<uint32_t>(tileMax, mProjCapacity != 0u ? mProjCapacity : 16u);
        mPsmCubes = std::min<uint32_t>((uint32_t)mPsmSel.size(), tiles / 6u);
        for (uint32_t ci = 0; ci < mPsmCubes; ++ci)
        {
            const PsmSel& s = mPsmSel[ci];
            const VKLightGPU& g = mLightsCPU[s.light];
            const float near_d = std::max(std::min(g.proj_axis[0], 0.5f * g.pos_radius[3]), 0.05f);
            const float far_d = std::max(g.pos_radius[3], near_d + 0.1f);
            for (uint32_t f = 0; f < 6u; ++f)
            {
                ShadowViewReq r;
                r.key = psmFaceKey(s.key, f);
                psmFaceViewProj(g.pos_radius, f, near_d, far_d, r.vp);
                r.tan[0] = r.tan[1] = 1.f;
                r.apex[0] = g.pos_radius[0]; r.apex[1] = g.pos_radius[1]; r.apex[2] = g.pos_radius[2];
                r.radius = g.pos_radius[3];
                r.near_d = near_d;
                r.score = s.score * (1.f - 1.0e-5f * (float)f);
                queueShadowView(r);
            }
        }
    }
    mPsmKeys.clear();
    for (const PsmSel& s : mPsmSel) mPsmKeys.push_back(s.key);
    std::sort(mPsmKeys.begin(), mPsmKeys.end());
    if (mode != 0u && !rtCovers && !mOffscreen)
    {   // (a once-a-minute summary: how many selected lights hold all six faces)
        static double s_psmLogAt = 0.0;
        const double now = pdnClock();
        if (now >= s_psmLogAt)
        {
            s_psmLogAt = now + 60.0;
            LL_INFOS("VKShadow") << "point light shadow maps (#439, RenderVKPointShadowMaps): " << mPsmMapped << " of "
                                 << mPsmSel.size() << " selected light(s) shadowed by their 6 cube faces this frame ("
                                 << mPsmCubes << " cube(s) queued: 6 atlas tiles each within RenderVKProjShadowMax / the "
                                 << "atlas's cells, shared with the shadow-mapped projectors)"
                                 << (mProjMode == 1u ? "" : " (projector shadows are not on shadow maps: none)") << LL_ENDL;
        }
    }
    const uint32_t key = mode == 0u ? 0u : (rtCovers ? 1u : 2u + (uint32_t)mPsmSel.size() * 4u + (uint32_t)(mPsmBytes >> 20) * 1024u);
    if (key != mPsmLogKey && !mOffscreen)
    {
        mPsmLogKey = key;
        if (mode != 0u)
            LL_INFOS("VKShadow") << "point light shadow maps (RenderVKPointShadowMaps 1, #439 non-RT fallback): "
                                 << (rtCovers ? "not needed -- ray traced through ReSTIR DI (RenderVKReSTIRLights 1)"
                                              : std::to_string(mPsmSel.size()) + " light(s) selected (RenderVKPointShadowMapMax "
                                                    + std::to_string(maxN) + "), their 6 cube faces each as projector-atlas "
                                                      "shadow views (#567, competing with the projectors under "
                                                      "RenderVKProjShadowMax); a light is shadowed once all 6 faces hold a tile")
                                 << LL_ENDL;
    }
}

// ======================= ReSTIR DI STATS (#869 Phase 5) =======================
// After pass 15 (a ReSTIR frame): harvest this ring slot's copy from 3 frames ago, then copy this
// frame's counters (the grid header's rs_stat: the build's + pass 15's) into it. GPU timers on (the line
// is logged with them), or the emissive build on: MODE/CAP/BUDGET transitions read the same words
// (never a wait: the copy is 3 frames old). Bounded: 3 slots of a few hundred bytes.
void LLCleanRendererVK::rsStatsRecord(VkCommandBuffer cmd)
{
    VkMemScope memcat(VKMEM_READBACK);   // #407
    if (!mRegir.buffer || !mCtx) return;
    if (!mTsOn && !mRsEmNow)
    {   // (nothing to read: the slots' copies would be stale when the ring restarts)
        mRsStatUsed[0] = mRsStatUsed[1] = mRsStatUsed[2] = false;
        std::fill(std::begin(mRsEmDiagUsed), std::end(mRsEmDiagUsed), false);
        return;
    }
    const uint32_t slot = mRsStatSlot;
    mRsStatSlot = (mRsStatSlot + 1u) % 3u;
    // (rs_stat + rs_em: contiguous in the header; + #91 rs_em_stat and rs_em_take_cfg and #986 rs_em_cache,
    // a second copy region)
    constexpr VkDeviceSize kBytesA = sizeof(RegirHeader::rs_stat) + sizeof(RegirHeader::rs_em);
    constexpr VkDeviceSize kBytesB = sizeof(RegirHeader::rs_em_stat) + sizeof(RegirHeader::rs_em_take_cfg)
                                   + sizeof(RegirHeader::rs_em_cache);
    constexpr VkDeviceSize kBytesC = 8u;   // (#986 LONG CELLS: their count, the longest list -- kRsEmLongOffset)
    constexpr VkDeviceSize kBytesD = kRsOccStatBytes;   // (#869 OCCLUSION CULL: OCC_STAT_WORDS -- kRegirOccOffset)
    constexpr VkDeviceSize kBytesE = 32u;   // (#1006 INCREMENTAL CELLS: EM_CCH_STAT, 4 x 8 B -- kRsEmCchStatOffset)
    constexpr VkDeviceSize kBytesF = 40u;   // (#1009 CLUSTER STATICS + SKIP DIAGNOSTIC + EM_CCH_OUT: CC_HDR [41..45] -- kRsEmCchClusterOffset)
    constexpr VkDeviceSize kBytesG = 64u; // bounded producer CC_HDR [48..55], same GPU snapshot
    constexpr VkDeviceSize kBytesH = 16u;   // (#1031 split core: mPdnVis's job-list header -- the VIS job count, word 0)
    constexpr VkDeviceSize kOffG = kBytesA + kBytesB + kBytesC + kBytesD + kBytesE + kBytesF;
    constexpr VkDeviceSize kOffH = kOffG + kBytesG;
    constexpr VkDeviceSize kBytes = kOffH + kBytesH;
    static_assert(offsetof(RegirHeader, rs_em) == offsetof(RegirHeader, rs_stat) + sizeof(RegirHeader::rs_stat),
                  "rs_stat / rs_em contiguous");
    static_assert(offsetof(RegirHeader, rs_em_take_cfg) == offsetof(RegirHeader, rs_em_stat) + sizeof(RegirHeader::rs_em_stat),
                  "rs_em_stat / rs_em_take_cfg contiguous");
    static_assert(offsetof(RegirHeader, rs_em_cache) == offsetof(RegirHeader, rs_em_take_cfg) + sizeof(RegirHeader::rs_em_take_cfg),
                  "rs_em_take_cfg / rs_em_cache contiguous");
    if (mRsStatUsed[slot] && mRsStatRb.mapped)
    {   // Every counter and mode below is from this one retired GPU snapshot, never this frame's settings.
        const uint32_t* s = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mRsStatRb.mapped)
                                                               + (size_t)slot * kBytes);
        const bool emBuilt = s[8 + 2] != 0u;   // (rs_em.z: the triangle cap, written while the build runs)
        const uint32_t* x = s + (kBytesA + kBytesB + kBytesC + kBytesD) / 4u;
        const bool lists = mRsStatLists[slot];
        const uint32_t* budget = s + (kBytesA + kBytesB + kBytesC + kBytesD + kBytesE + kBytesF) / 4u;
        const uint32_t mode = emBuilt ? 5u : 0u;
        const uint32_t live = std::min(s[8], s[10]);
        mRsEmTriPeak = std::max(mRsEmTriPeak, live);
        const uint32_t state = mode | (emBuilt && s[9] != 0u ? 16u : 0u)
                            | (emBuilt && budget[5] != 0u ? 32u : 0u)
                            | (emBuilt && budget[12] != 0u ? 64u : 0u)
                            | (emBuilt && budget[13] != 0u ? 128u : 0u)
                            | (emBuilt && budget[15] != 0u ? 256u : 0u)
                            | (emBuilt && budget[6] >= budget[7] ? 512u : 0u);
        uint32_t snap[26] = {mRsStatFrame[slot], state, s[11] >> 1u, live, s[10], s[9], s[22],
                             s[12], lists ? x[0] : 0u, lists ? x[1] : 0u};
        if (emBuilt) std::copy(budget,budget+16,snap+10);
        if (state != mRsEmTransitionState)
        {
            std::copy(std::begin(mRsEmObserved), std::end(mRsEmObserved), mRsEmChangeFrom);
            std::copy(std::begin(snap), std::end(snap), mRsEmChangeTo);
            mRsEmTransitionState = state;
            ++mRsEmTransitionPending;
        }
        std::copy(std::begin(snap), std::end(snap), mRsEmObserved);
        const double now = pdnClock();
        if (mRsEmTransitionPending != 0u && now >= mRsEmTransitionNext)
        {
            auto describe = [](const uint32_t* q) {
                static const char* const names[] = {"off", "incremental", "full", "gather9", "gather7", "bounded"};
                if (q[0] == 0u) return std::string("no prior GPU snapshot");
                std::ostringstream os;
                os << "gpu_frame=" << q[0] << " mode=" << names[std::min(q[1] & 15u, 5u)]
                   << " state=" << q[1] << " emitters=" << q[2] << " live_tris=" << q[3]
                   << "/" << q[4] << " subset_dropped_in_cohort=" << q[5] << " pairs=" << q[6]
                   << " cells_with_candidates=" << q[7] << " changed/regather=" << q[8] << "/" << q[9]
                   << " discovery_rows/offers=" << q[10] << "/" << q[11] << " evictions=" << q[12]
                   << " discovery_sweep_complete=" << q[13] << " active/deferred_faces=" << q[14] << "/" << q[15]
                   << " pairs/cap=" << q[16] << "/" << q[17] << " record_attempt_ceiling=" << q[18] << " long_cells=" << q[19]
                   << " radius_clamped_records=" << q[22] << " centroid_support_records=" << q[23]
                   << " attempted_cells=" << q[24] << " invariant_failure=" << q[25];
                return os.str();
            };
            LL_INFOS("VKEmissive") << "build MODE/CAP/BUDGET transition: [" << describe(mRsEmChangeFrom)
                                  << "] -> [" << describe(mRsEmChangeTo) << "] tri_peak=" << mRsEmTriPeak
                                  << " pairs_cap=" << kRsEmPairCap << " transitions=" << mRsEmTransitionPending
                                  << " reason=" << ((state & 256u) ? "PAIR_INVARIANT_FAILURE_UNAVAILABLE" : (state & 512u) ? "PAIR_BUDGET_SATURATED" : (state & 32u) ? "FACE_ADMISSION_SATURATED" : (state & 128u) ? "CENTROID_SUPPORT_APPROXIMATION"
                                                       : (state & 16u) ? "TRI_SUBSET_OR_DROP" : (state & 64u) ? "RADIUS_BUDGET_CLAMP" : "MODE_OR_RECOVERY")
                                  << " harvested_at_gpu_frame=" << mRsStatFrameNow << " async_lag="
                                  << (mRsStatFrameNow - mRsStatFrame[slot]) << " grid_header_frames; no wait" << LL_ENDL;
            mRsEmTransitionPending = 0u;
            mRsEmTransitionNext = now + 2.0;
        }
        if (mRsEmDiagUsed[slot]) rsEmDiagRead(slot, s);
        mRsEmDiagUsed[slot] = false;
        // (clean_regir.glsl SUBSET STRIDE) the rest's stride (rs_em_take_cfg.w): a STEP re-subsets every emitter at
        // once (records rebuilt, emissive reservoirs rejected, cells forced) -- noted, at most a line per
        // kRsEmStrideLogSecs with the steps since the last one
        const uint32_t stride = emBuilt ? s[16 + 3] : 0u;
        if (stride != 0u)
        {
            if (mRsEmStrideLast != 0u && stride != mRsEmStrideLast)
            {
                ++mRsEmStrideSteps;
                const double now = pdnClock();
                if (now - mRsEmStrideLogT >= kRsEmStrideLogSecs)
                {
                    LL_INFOS("Vulkan") << "emissive light build: subset stride " << mRsEmStrideLast << " -> " << stride
                                       << " (" << mRsEmStrideSteps << " step(s) since the last note): triangle records "
                                       << std::min(s[8], s[8 + 2]) << " / cap " << s[8 + 2] << ", skipped or dropped "
                                       << s[8 + 1] << ", emitters " << (s[8 + 3] >> 1) << LL_ENDL;
                    mRsEmStrideLogT = now;
                    mRsEmStrideSteps = 0;
                }
            }
            mRsEmStrideLast = stride;
        }
    }
    if (mRsStatUsed[slot] && mRsStatRb.mapped && mRsStatTs[slot] && mTsOn)
    {
        const uint32_t* s = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mRsStatRb.mapped)
                                                               + (size_t)slot * kBytes);
        for (int k = 0; k < 8; ++k) mRsStatAcc.stat[k] += s[k];
        if (s[8 + 2] != 0u)   // (#91: the emissive records' cap: built that frame)
        {
            const double em = (double)std::min(s[8], s[8 + 2]);
            mRsStatAcc.em += em;
            mRsStatAcc.emDrop += s[8 + 1];
            mRsStatAcc.emPeak = std::max(mRsStatAcc.emPeak, em);
            mRsStatAcc.emInst += (double)(s[8 + 3] >> 1);   // (rs_em.w: parity | emitters << 1)
            mRsStatAcc.emCap = std::max(mRsStatAcc.emCap, (double)s[8 + 2]);   // (RenderVKReSTIREmissiveTriangles)
            ++mRsStatAcc.emFrames;
            for (int k = 0; k < 4; ++k) mRsStatAcc.emStat[k] += s[12 + k];
            // (LIGHT TAKEOVER: rs_em_take_cfg -- this frame's table lights, taken, refused)
            mRsStatAcc.emTake[0] += mRsStatTakeN[slot];
            mRsStatAcc.emTake[1] += s[16 + 1];
            mRsStatAcc.emTake[2] += s[16 + 2];
            // Current records built, actual bounded pairs; the legacy GATHER bit stays zero.
            mRsStatAcc.emCopied += s[20 + 0];
            mRsStatAcc.emBuilt += s[20 + 1];
            mRsStatAcc.emPairs += s[20 + 2];
            mRsStatAcc.emPairsPeak = std::max(mRsStatAcc.emPairsPeak, (double)s[20 + 2]);
            const uint32_t* budget = s + (kBytesA + kBytesB + kBytesC + kBytesD + kBytesE + kBytesF) / 4u;
            for (uint32_t k=0;k<16u;++k) mRsStatAcc.emBudget[k] += budget[k];
            if ((s[20 + 3] & 1u) != 0u) ++mRsStatAcc.emGather;
            if (mRsStatLists[slot])   // (the region is cleared and copied with the cell lists only)
            {
                mRsStatAcc.emLong += s[24 + 0];
                mRsStatAcc.emLongest = std::max(mRsStatAcc.emLongest, (double)s[24 + 1]);
            }
            if (mRsStatInc[slot])   // (#1006: EM_CCH_STAT, after the occlusion words -- see rsStatsText)
            {
                const uint32_t* x = s + (kBytesA + kBytesB + kBytesC + kBytesD) / 4u;
                mRsStatAcc.emDirty += x[0];
                mRsStatAcc.emRegather += x[1];
                mRsStatAcc.emForced += x[2];
                mRsStatAcc.emStaleMax = std::max(mRsStatAcc.emStaleMax, (double)x[3]);
                mRsStatAcc.emWeak += x[4];
                mRsStatAcc.emMiss += x[5];
                mRsStatAcc.emKept += x[6];
                mRsStatAcc.emFull += x[7];
                mRsStatAcc.emClInst += x[8];   // (#1009: CC_HDR [41..44], right after EM_CCH_STAT -- kBytesF)
                mRsStatAcc.emClTri += x[9];
                mRsStatAcc.emOutInst += x[16];   // (CC_HDR [45], after the skip words [42..44])
                mRsStatAcc.emOutTri += x[17];
                if (mRsStatSkip[slot])
                {
                    for (int k = 0; k < 5; ++k) mRsStatAcc.emSkip[k] += x[10 + k];
                    ++mRsStatAcc.emSkipFrames;
                }
                ++mRsStatAcc.emIncFrames;
            }
        }
        if (mRsStatOcc[slot])   // (#869 OCCLUSION CULL: words 0-12, see clean_deferred_light.comp STATS)
        {
            const uint32_t* o = s + (kBytesA + kBytesB + kBytesC) / 4u;
            for (int k = 0; k < 13; ++k) mRsStatAcc.occ[k] += o[k];
            ++mRsStatAcc.occFrames;
        }
        if (mRsStatVis[slot])   // (#1031 split core: VIS's jobs = the gated light samples SELECT listed that frame)
        {
            const double jobs = (double)s[kOffH / 4u];
            mRsStatAcc.visJobs += jobs;
            mRsStatAcc.visJobsPeak = std::max(mRsStatAcc.visJobsPeak, jobs);
            mRsStatAcc.visTex += (double)s[3] + (double)s[4];   // (rs_stat 3 / 4: the pass-15 surface texels)
            ++mRsStatAcc.visFrames;
        }
        mRsStatAcc.setN += mRsStatSetN[slot];
        mRsStatAcc.pointN += mRsStatPointN[slot];
        mRsStatAcc.movedN += mRsStatMovedN[slot];
        mRsStatAcc.flickN += mRsStatFlickN[slot];
        ++mRsStatAcc.frames;
    }
    mRsStatUsed[slot] = false;
    if (!mRsStatRb.buffer)
        mRsStatRb = mCtx->createBuffer(3u * kBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!mRsStatRb.buffer) return;
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; m.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &d);
    const VkBufferCopy c[7] = {{offsetof(RegirHeader, rs_stat), (VkDeviceSize)slot * kBytes, kBytesA},
                               {offsetof(RegirHeader, rs_em_stat), (VkDeviceSize)slot * kBytes + kBytesA, kBytesB},
                               {kRegirOccOffset, (VkDeviceSize)slot * kBytes + kBytesA + kBytesB + kBytesC, kBytesD},
                               {kRsEmLongOffset, (VkDeviceSize)slot * kBytes + kBytesA + kBytesB, kBytesC},
                               {kRsEmCchStatOffset, (VkDeviceSize)slot * kBytes + kBytesA + kBytesB + kBytesC + kBytesD,
                                kBytesE},
                               {kRsEmCchClusterOffset,
                                (VkDeviceSize)slot * kBytes + kBytesA + kBytesB + kBytesC + kBytesD + kBytesE, kBytesF},
                               {kRsEmXOffset + 48u*8u, (VkDeviceSize)slot*kBytes + kOffG, kBytesG}};
    // (the regions present this frame, packed: A, B, + D with the occlusion cache, + C and (#1006) E, (#1009) F
    //  with the cell lists -- the per-build words they hold are cleared with the lists)
    VkBufferCopy cc[7];
    uint32_t nc = 0;
    cc[nc++] = c[0];
    cc[nc++] = c[1];
    if (mRsOccNow) cc[nc++] = c[2];
    if (mRsEmListsNow) cc[nc++] = c[3];
    if (mRsEmListsNow) cc[nc++] = c[4];
    if (mRsEmListsNow) cc[nc++] = c[5];
    if (mRsEmListsNow) cc[nc++] = c[6];
    {
        LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_light_prep_setup");
        vkCmdCopyBuffer(cmd, mRegir.buffer, mRsStatRb.buffer, nc, cc);
    }
    lvcStatsRecord(cmd, slot);   // (LVC stage 0: its own ring, the same slot and barrier)
    // (#1031 split core) VIS's job count: SELECT's atomics, finished before this barrier (it ran ahead of VIS / SHADE on
    // this queue); next frame's header reset (recordLightPrep) is ordered after this read by its opening barrier
    // (src ALL_TRANSFER), as the regir copies above
    const bool visCount = mLightPrep.split && mPdnVis.buffer != VK_NULL_HANDLE;
    if (visCount)
    {
        // (the job list's header at its range of the RT lighting scratch; the GI's records overwrite it later in the
        //  frame: rtScratchHandoff orders this read first)
        const VkBufferCopy vc{mPdnVisOff, (VkDeviceSize)slot * kBytes + kOffH, kBytesH};
        {
            LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_light_prep_setup");
            vkCmdCopyBuffer(cmd, mPdnVis.buffer, mRsStatRb.buffer, 1u, &vc);
        }
    }
    // A snapshot only every 5 seconds, piggybacked on the existing retired statistics slot.
    // No fence/query/readback wait and no additional per-frame CPU/GPU round trip.
    mRsEmDiagUsed[slot] = false;
    const double now = pdnClock();
    if (mRsEmDiagLog && now >= mRsEmDiagNext && now >= mRsEmDiagRetry)
    {
        constexpr VkDeviceSize bytes = (VkDeviceSize)kRsEmInstCap * kRsEmEntBytes;
        if (!mRsEmDiagRb.buffer)
        {
            VkDeviceContext::SoftAllocScope soft(mCtx);
            mRsEmDiagRb = mCtx->createBuffer(3u * bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (!mRsEmDiagRb.buffer)
            {
                mRsEmDiagRetry = now + 30.0;
                LL_WARNS("VKEmissive") << "diagnostic readback refused: " << 3u * bytes
                                      << " bytes; retry in 30s, lighting retained" << LL_ENDL;
            }
        }
        if (mRsEmDiagRb.mapped)
        {
            gpuSubMark(cmd, mAsyncNow.on ? "c_rs_em_diag_begin" : "g_rs_em_diag_begin");
            const uint32_t par = mRsEmParity ^ 1u; // recordLightPrep toggled NEXT parity
            const VkBufferCopy dc{kRsEmEntOffset + par * bytes, slot * bytes, bytes};
            {
                GpuPassScope scope(*this, cmd, mAsyncNow.on ? "c_rs_em_diag_copy" : "g_rs_em_diag_copy");
                vkCmdCopyBuffer(cmd, mRegir.buffer, mRsEmDiagRb.buffer, 1u, &dc);
            }
            mRsEmDiagUsed[slot] = true;
            mRsEmDiagFrame[slot] = mRsStatFrameNow;
#if defined(LL_HAS_CLEAN_PIPELINE)
            mRsEmDiagNewCount[slot] = LLCleanInstanceStore::instance().newInstanceCount();
#endif
            mRsEmDiagNext = now + 5.0;
            gpuSubMark(cmd, mAsyncNow.on ? "c_rs_em_diag_copy" : "g_rs_em_diag_copy");
        }
    }
    mRsStatFrame[slot] = mRsStatFrameNow;
    mRsStatLists[slot] = mRsEmListsNow;
    mRsStatLong[slot] = mRsEmLongNow;
    mRsStatTs[slot] = mTsOn;
    mRsStatInc[slot] = mRsEmListsNow;
    mRsStatSkip[slot] = mRsEmListsNow && mRsEmSkipNow;
    mRsStatOcc[slot] = mRsOccNow;
    mRsStatVis[slot] = visCount;
    mRsStatSetN[slot] = mRsSetN;
    mRsStatPointN[slot] = mRsPointN;
    mRsStatMovedN[slot] = mRsMovedN;
    mRsStatFlickN[slot] = mRsFlickN;
    mRsStatTakeN[slot] = mRsEmTakeNow != 0u ? mRsEmTakeLights : 0u;
    mRsStatUsed[slot] = true;
}

// Inspect only the GPU's bounded emitter-entry snapshot, once per five seconds. No scene/face scan.
void LLCleanRendererVK::rsEmDiagRead(uint32_t slot, const uint32_t* stats)
{
    if (!mRsEmDiagRb.mapped) return;
    constexpr uint32_t words = kRsEmEntBytes / 4u;
    const auto* data = reinterpret_cast<const uint32_t*>(mRsEmDiagRb.mapped) + (size_t)slot * kRsEmInstCap * words;
    const uint32_t n = kRsEmInstCap; // fixed cohort/table slots; frame stamp filters empty entries
    std::array<uint32_t, kRsEmInstCap> order{};
    size_t count = 0;
    for (uint32_t e = 0; e < n; ++e)
        if (data[e * words + 41u] == mRsEmDiagFrame[slot] && data[e * words + 1u] != 0u) order[count++] = e;
    std::partial_sort(order.begin(), order.begin() + std::min<size_t>(15u, count), order.begin() + count,
        [&](uint32_t a, uint32_t b) { return data[a * words + 1u] != data[b * words + 1u]
            ? data[a * words + 1u] > data[b * words + 1u] : data[a * words] < data[b * words]; });
    LL_INFOS("VKEmissive") << "TOP15 GPU emitter faces by source triangles: gpu_frame=" << mRsEmDiagFrame[slot]
                          << " harvested_at_gpu_frame=" << mRsStatFrameNow << " async_lag="
                          << mRsStatFrameNow - mRsEmDiagFrame[slot] << " grid_header_frames; entries=" << n
                          << "/" << kRsEmInstCap << " source_tris=kept+subset_dropped; cells=actual all-level COUNT target hits"
                          << " (NOT unique cells; bounded effective reach); retained cohort only, not scene total; metadata=live lookup at harvest; names async"
                          << "; attachments/worn/rigged excluded by existing producer, not this diagnostic" << LL_ENDL;
    auto asFloat = [](uint32_t u) { float f; std::memcpy(&f, &u, sizeof(f)); return f; };
#if defined(LL_HAS_CLEAN_PIPELINE)
    const double now = pdnClock();
    const bool identityStable = mRsEmDiagNewCount[slot] == LLCleanInstanceStore::instance().newInstanceCount();
    for (uint32_t i = 0; i < s_emDiagNameCount;)
        if (s_emDiagNames[i].until < now) s_emDiagNames[i] = s_emDiagNames[--s_emDiagNameCount]; else ++i;
#endif
    for (size_t rank = 0; rank < std::min<size_t>(15u, count); ++rank)
    {
        const uint32_t* w = data + order[rank] * words;
        const uint32_t mf = w[32], fl = w[33], motion = w[27];
        std::ostringstream identity;
        identity << "row=" << w[0];
#if defined(LL_HAS_CLEAN_PIPELINE)
        LLUUID id; S32 face = -1;
        if (identityStable && LLCleanInstanceStore::instance().keyForId(w[0], id, face))
        {
            LLViewerObject* o = gObjectList.findObject(id);
            if (o && (o->isAvatar() || o->isAttachment() || o->getAvatar()))
                identity << " attachment on avatar attachpoint=" << (uint32_t)ATTACHMENT_ID_FROM_STATE(o->getAttachmentState())
                         << " (name/identity omitted; live row may have changed since snapshot)";
            else if (o)
            {
                identity << " local_id=" << o->getLocalID() << " uuid=" << id << " face=" << face;
                LLSelectNode* node = LLSelectMgr::getInstance()->getSelection()->findNode(o);
                if (node && node->mValid) identity << " name=\"" << emDiagSafeName(node->mName) << "\"";
                else
                {
                    identity << " name=<pending/unavailable>";
                    const bool pending = std::any_of(s_emDiagNames.begin(), s_emDiagNames.begin() + s_emDiagNameCount,
                        [&](const EmDiagName& v) { return v.id == id; });
                    if (!pending && s_emDiagNameCount < s_emDiagNames.size() && o->getRegion())
                    {
                        s_emDiagNames[s_emDiagNameCount++] = {id, mRsEmDiagFrame[slot], o->getLocalID(), now + 10.0};
                        LLSelectMgr::getInstance()->requestObjectPropertiesFamily(o); // read-only, <=15 / 5s
                    }
                }
            }
            else identity << " identity=<object departed>";
        }
        else identity << " identity=<row retired or allocation epoch changed; no potentially recycled identity>";
#endif
        LL_INFOS("VKEmissive") << "#" << rank + 1u << " " << identity.str()
            << " gpu_frame=" << mRsEmDiagFrame[slot] << " triangles=" << w[1] << " kept=" << w[3]
            << " subset_dropped=" << w[1] - std::min(w[1], w[3]) << " stride=" << w[4]
            << " why=" << ((mf & 64u) ? ((mf & 8u) ? "PBR_FACTOR_TIMES_TEX" : "PBR_FACTOR") : "FULLBRIGHT_AND_GLOW_TIMES_BASE_TEX_IF_PRESENT")
            << " material_flags=" << mf << " instance_flags=" << fl << " fullbright=" << ((fl & 4096u) != 0u)
            << " glow_byte=" << ((mf >> 8u) & 255u) << " tex=" << w[9]
            << " scaled_factor_rgb=" << asFloat(w[6]) << "," << asFloat(w[7]) << "," << asFloat(w[8])
            << " factor_luma=" << asFloat(w[39]) << " coarse_tex_luma_result=" << asFloat(w[34])
            << " min=" << asFloat(w[35]) << " alpha_mode=" << ((fl & 2u) ? "BLEND" : (fl & 4u) ? "MASK" : "OPAQUE")
            << " alpha_factor=" << asFloat(w[38]) << " world_aabb_extent=" << asFloat(w[29]) << "," << asFloat(w[30]) << "," << asFloat(w[31])
            << " sampled_area_estimate=" << asFloat(w[28]) << " face_range_estimate=" << asFloat(w[10])
            << " moved_this_frame=" << ((motion & 1u) ? ((motion & 2u) ? "yes" : "no") : "unknown/new")
            << " geometry_key_changed=" << ((motion & 1u) ? ((motion & 4u) ? "yes" : "no") : "unknown/new")
            << " triangle_requested_range_max=" << asFloat(w[42]) << " triangle_effective_range_max=" << asFloat(w[43])
            << " radius_clamped_records=" << w[24] << " centroid_support_records=" << w[25]
            << " actual_cell_triangle_pairs=" << (mRsStatLists[slot] ? std::to_string(w[36]) : "unavailable(lists off)")
            << " unique_cells_upper_bound=" << (mRsStatLists[slot] ? std::to_string(std::min(w[36], kRsEmNCellsAll)) : "unavailable")
            << " dark_or_degenerate_records=" << w[37] << " triangle_luma_cutoff=" << 0.02f * asFloat(w[35])
            << "; motion/signature is a cause of invalidation, NOT a per-object cell-regather counter" << LL_ENDL;
    }
}

// ======================= LVC STAGE 3: THE REACH MAPS (RenderVKShadowReachMap) =======================
// research/LVC_DESIGN_2026-11-16.md 3.1 / 3.3b; clean_deferred_light.comp CLEAN_LVC_BUILD (the five modes) and lvcBlocked
// (pass 15's skip). Recorded right before pass 15 on its queue so the builds trace the same TLAS as its shadow rays:
// ASSIGN (1 workgroup) -> DIRTY (one lane per instance record, kLvcInstCap) -> PLAN (1 workgroup) -> BUILD (kLvcBuildMax
// lights x 6144 texels x 8 rays) -> FINALIZE (the dilated half-float maps) -> pass 15 reads PLAN's usable flags. Fixed
// work every frame, no readback, no per-light CPU work; the region (kRegirLvcMapBytes) is zeroed once when the maps come
// on (mLvcClear). GPU timer c_lvc / g_lvc.
void LLCleanRendererVK::recordLvc(VkCommandBuffer cmd, const VKRtReflPush& rp, bool onAsync)
{
    auto cc = [cmd](VkPipelineStageFlags2 src, VkAccessFlags2 sa)
    {
        VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        m.srcStageMask = src; m.srcAccessMask = sa;
        m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
        vkCmdPipelineBarrier2(cmd, &d);
    };
    if (mLvcClear)
    {
        VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        m.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT; m.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
        vkCmdPipelineBarrier2(cmd, &d);
        {
            LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_light_prep_setup");
            vkCmdFillBuffer(cmd, mRegir.buffer, kRegirLvcMapOffset, kRegirLvcMapBytes, 0u);
        }
        cc(VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        mLvcClear = false;
        mLvcFrame = 16u;   // (past LVC_STILL: a new slot's "still since" stamp never wraps below 0)
    }
    else cc(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    mLvcFrame = (mLvcFrame + 1u) & 0xFFFFFFu;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLvcPipe);
    VKRtReflPush xp = rp;
    xp.rtr3[3] = (float)mLvcFrame;   // (exact below 2^24)
    static LLCachedControl<bool> s_stageTimers(gSavedSettings, "RenderVKPerfLvcStages", false);
    const bool stageTimers = s_stageTimers;
    static const char* const kStageMark[2][5] = {
        {"g_lvc_assign", "g_lvc_dirty", "g_lvc_plan", "g_lvc_build", "g_lvc_finalize"},
        {"c_lvc_assign", "c_lvc_dirty", "c_lvc_plan", "c_lvc_build", "c_lvc_finalize"}};
    if (stageTimers) gpuSubMark(cmd, onAsync ? "c_lvc_setup" : "g_lvc_setup");
    const uint32_t groups[5] = {1u, kLvcInstCap / 64u, 1u, kLvcBuildMax * kLvcTexels / 64u, kLvcBuildMax * kLvcTexels / 128u};
    for (uint32_t mode = 0; mode < 5u; ++mode)
    {
        xp.rtr3[2] = (float)mode;
        vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(xp), &xp);
        {
            GpuPassScope scope(*this, cmd, stageTimers ? kStageMark[onAsync ? 1u : 0u][mode] : (onAsync ? "c_lvc" : "g_lvc"));
            vkCmdDispatch(cmd, groups[mode], 1u, 1u);
        }
        cc(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        if (stageTimers) gpuSubMark(cmd, kStageMark[onAsync ? 1u : 0u][mode]);
    }
    gpuSubMark(cmd, onAsync ? "c_lvc" : "g_lvc");
}

// ======================= LVC STAGE 0: THE LIGHT-VISIBILITY STATISTICS (RenderVKReSTIROcclusionCull 3) =======================
// research/LVC_DESIGN_2026-11-16.md section 5: the counters clean_deferred_light.comp's LVC STATS fill in pass 15 (VIS's
// statistics variant, SELECT, the adaptive MASK) -- copied with the ReSTIR ring (its slot, harvested 3 frames late, never
// a wait), summed per light TAG (the list is re-sorted every frame: this frame's index -> tag is saved with the slot), one
// VKGpu line with the ReSTIR DI line (GPU timers on). Bounded: 3 slots x 9.25 KiB host-visible, <= 256 tags.
void LLCleanRendererVK::lvcStatsRecord(VkCommandBuffer cmd, uint32_t slot)
{
    constexpr VkDeviceSize kTagBytes = 256u * 4u, kPosBytes = 256u * 16u;
    constexpr VkDeviceSize kSlot = kRegirLvcBytes + kTagBytes + kPosBytes;
    if (mLvcStatUsed[slot] && mLvcStatRb.mapped)
    {
        const uint8_t* base = static_cast<const uint8_t*>(mLvcStatRb.mapped) + (size_t)slot * kSlot;
        const uint32_t* w = reinterpret_cast<const uint32_t*>(base);
        const uint32_t* tags = reinterpret_cast<const uint32_t*>(base + kRegirLvcBytes);
        const float* pos = reinterpret_cast<const float*>(base + kRegirLvcBytes + kTagBytes);
        for (uint32_t k = 0; k < 84u; ++k)
        {
            if (k == 51u) mLvcAcc.s[k] = (double)((uint32_t)mLvcAcc.s[k] | w[k]);      // (changed flag bits: ORed)
            else if (k == 53u) mLvcAcc.s[k] = std::max(mLvcAcc.s[k], (double)w[k]);  // (the breaks' edge closeness: max)
            else mLvcAcc.s[k] += w[k];
        }
        // ASTRA admission reasons' static-blocked subset; 84..147 are churn records.
        for (uint32_t k = 148u; k < 158u; ++k) mLvcAcc.s[k] += w[k];
        ++mLvcAcc.frames;
        {   // (static churn samples: the first 16 changed instances of that frame, summed by id)
            const uint32_t ns = std::min<uint32_t>(w[83], 16u);
            for (uint32_t k = 0; k < ns; ++k)
            {
                const uint32_t* e = w + 84u + k * 4u;
                const uint32_t id = e[0] & 0x0FFFFFFFu;
                auto it = mLvcChurn.find(id);
                if (it == mLvcChurn.end())
                {
                    if (mLvcChurn.size() >= 64u) continue;
                    it = mLvcChurn.emplace(id, LvcChurn{}).first;
                }
                ++it->second.n;
                it->second.kinds |= 1u << (e[0] >> 28);
                if ((e[0] >> 28) == 1u)
                {   // (a transform change: word 1 is the rotation / scale columns' largest element change)
                    float rd; std::memcpy(&rd, &e[1], sizeof(rd));
                    it->second.rot = std::max(it->second.rot, rd);
                }
                else it->second.flagsOld = e[1];
                it->second.flagsNew = e[2];
                float mv; std::memcpy(&mv, &e[3], sizeof(mv));
                it->second.move = std::max(it->second.move, mv);
            }
        }
        for (uint32_t li = 0; li < 256u; ++li)
        {
            const uint32_t* c = w + kLvcStatWords + li * kLvcLightWords;
            if (c[0] == 0u && c[3] == 0u && c[4] == 0u && c[5] == 0u) continue;
            const uint32_t tag = tags[li];
            auto it = std::find_if(mLvcAcc.lights.begin(), mLvcAcc.lights.end(),
                                   [tag](const LvcLightAcc& e) { return e.tag == tag; });
            if (it == mLvcAcc.lights.end())
            {
                if (mLvcAcc.lights.size() >= 256u) continue;   // (bounded: a new tag past 256 is not summed)
                mLvcAcc.lights.push_back(LvcLightAcc{});
                it = mLvcAcc.lights.end() - 1;
                it->tag = tag;
            }
            for (uint32_t k = 0; k < 6u; ++k) it->c[k] += c[k];
            float ex;
            std::memcpy(&ex, &c[6], sizeof(ex));
            it->excess = std::max(it->excess, ex);   // (stage 3 verify: the largest distance past map + margin)
            it->age = std::max(it->age, c[7]);
            for (uint32_t k = 0; k < 3u; ++k) it->pos[k] = pos[li * 4u + k];
            it->radius = std::fabs(pos[li * 4u + 3u]);
            it->proj = pos[li * 4u + 3u] < 0.f;
        }
    }
    mLvcStatUsed[slot] = false;
    if (!mRsOccStatsNow && !mLvcNoiseNow) return;
    mLvcAcc.occ = mLvcAcc.occ || mRsOccStatsNow;
    mLvcAcc.noise = mLvcAcc.noise || mLvcNoiseNow;
    if (!mLvcStatRb.buffer)
    {
        VkDeviceContext::SoftAllocScope soft(mCtx);
        mLvcStatRb = mCtx->createBuffer(3u * kSlot, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    if (!mLvcStatRb.mapped) return;   // (refused: no statistics this frame, retried the next)
    const VkBufferCopy c{kRegirLvcOffset, (VkDeviceSize)slot * kSlot, kRegirLvcBytes};
    {
        LLCleanRendererVK::GpuPassScope scope(*this, cmd, "c_light_prep_setup");
        vkCmdCopyBuffer(cmd, mRegir.buffer, mLvcStatRb.buffer, 1u, &c);
    }
    uint8_t* base = static_cast<uint8_t*>(mLvcStatRb.mapped) + (size_t)slot * kSlot;
    uint32_t* tags = reinterpret_cast<uint32_t*>(base + kRegirLvcBytes);
    float* pos = reinterpret_cast<float*>(base + kRegirLvcBytes + kTagBytes);
    const size_t n = std::min<size_t>(mLightsCPU.size(), 256u);
    for (size_t li = 0; li < 256u; ++li)
    {
        if (li < n)
        {
            const VKLightGPU& g = mLightsCPU[li];
            tags[li] = g.tex[3];
            pos[li * 4u + 0u] = g.pos_radius[0]; pos[li * 4u + 1u] = g.pos_radius[1]; pos[li * 4u + 2u] = g.pos_radius[2];
            // (the radius, negative for a projector)
            pos[li * 4u + 3u] = g.proj_origin[3] >= 0.5f ? -g.pos_radius[3] : g.pos_radius[3];
        }
        else { tags[li] = 0u; pos[li * 4u + 0u] = pos[li * 4u + 1u] = pos[li * 4u + 2u] = pos[li * 4u + 3u] = 0.f; }
    }
    mLvcStatUsed[slot] = true;
}

std::string LLCleanRendererVK::lightCensusText()
{
    const size_t n = std::min<size_t>(mLightsCPU.size(), 256u);
    uint64_t h = 1469598103934665603ull;
    uint32_t points = 0, projs = 0;
    auto mix = [&h](uint32_t v) { h = (h ^ v) * 1099511628211ull; };
    for (size_t i = 0; i < n; ++i)
    {
        const VKLightGPU& g = mLightsCPU[i];
        const bool proj = g.proj_origin[3] >= 0.5f;
        (proj ? projs : points)++;
        mix(g.tex[3]);
        for (int k = 0; k < 4; ++k) mix((uint32_t)std::lround(g.pos_radius[k] * 10.f));   // (10 cm / 0.1 m steps)
    }
    std::ostringstream os;
    os << std::fixed << std::setprecision(1) << "light census: " << n << " lights (" << points << " point, " << projs
       << " projector), set hash " << std::hex << h << std::dec;
    if (h != mLightCensusHash)
    {
        os << (mLightCensusHash == 0 ? " (first census)" : " (CHANGED since the last census)") << ":";
        for (size_t i = 0; i < n; ++i)
        {
            const VKLightGPU& g = mLightsCPU[i];
            const float lum = 0.2126f * g.color_falloff[0] + 0.7152f * g.color_falloff[1] + 0.0722f * g.color_falloff[2];
            os << " [" << g.tex[3] << (g.proj_origin[3] >= 0.5f ? " proj" : " pt") << " r " << g.pos_radius[3] << " ("
               << g.pos_radius[0] << ", " << g.pos_radius[1] << ", " << g.pos_radius[2] << ") L " << std::setprecision(2)
               << lum << std::setprecision(1) << "]";
        }
        mLightCensusHash = h;
    }
    return os.str();
}

// (RenderVKGpuTimers) SCENE FINGERPRINT, every 5 s beside the light census: what an A/B pair must share before its
// numbers are compared -- the scene's size (instances), who is in it (avatars), what is still streaming (texture
// fetches, mesh requests), the lights (count + the census hash), the environment (the sun direction + a hash over the
// frame's atmosphere block) and the camera. "settled" counts the consecutive windows with nothing streaming and the
// instance count, light set and environment unchanged: a measurement window should start at settled >= 2.
std::string LLCleanRendererVK::sceneFingerprintText()
{
    uint64_t env = 1469598103934665603ull;
    auto mix = [&env](uint32_t v) { env = (env ^ v) * 1099511628211ull; };
    for (int i = 0; i < 64; ++i) mix((uint32_t)std::lround(mAtmos[i] * 1000.f));   // (1e-3 steps: no float noise)
    uint32_t inst = 0, cpuInst = 0, avatars = 0;
    S32 texFetch = 0, meshHdr = 0, meshLod = 0;
#if defined(LL_HAS_CLEAN_PIPELINE)
    inst = LLCleanInstanceStore::instance().liveCount();
    cpuInst = LLCleanInstanceStore::instance().cpuInstanceCount();
    avatars = (uint32_t)LLCharacter::sInstances.size();
    if (LLAppViewer::getTextureFetch()) texFetch = LLAppViewer::getTextureFetch()->getNumRequests();
    meshHdr = LLMeshRepoThread::sActiveHeaderRequests.load();
    meshLod = LLMeshRepoThread::sActiveLODRequests.load();
#endif
    const bool same = inst == mFpInst && mLightCensusHash == mFpLights && env == mFpEnv;
    // (a bench replay -- no login -- streams nothing: its missing textures wait on a network that never answers)
    bool live = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
    live = LLStartUp::getStartupState() >= STATE_STARTED;
#endif
    const bool streaming = live && (texFetch != 0 || meshHdr != 0 || meshLod != 0);
    mFpSettled = (same && !streaming) ? mFpSettled + 1u : 0u;
    mFpInst = inst; mFpLights = mLightCensusHash; mFpEnv = env;
    std::ostringstream os;
    os << std::fixed << std::setprecision(3) << "scene fingerprint: instances " << inst << " (cpu " << cpuInst
       << "), avatars " << avatars << (live ? "" : " (replay)") << ", streaming: textures " << texFetch << " meshes " << meshHdr << "/" << meshLod
       << ", lights " << std::min<size_t>(mLightsCPU.size(), 256u) << " set hash " << std::hex << mLightCensusHash
       << ", env hash " << env << std::dec << ", sun (" << mFpSunDir[0] << ", " << mFpSunDir[1] << ", " << mFpSunDir[2]
       << "), camera (" << std::setprecision(2) << mTaaCam[0] << ", " << mTaaCam[1] << ", " << mTaaCam[2] << ") fwd ("
       << std::setprecision(3) << mTaaFwd[0] << ", " << mTaaFwd[1] << ", " << mTaaFwd[2] << "), settled " << mFpSettled;
    return os.str();
}

std::string LLCleanRendererVK::lvcStatsText()
{
    LvcAcc& a = mLvcAcc;
    if (!a.frames) { a.lights.clear(); mLvcChurn.clear(); return std::string(); }
    const double f = 1.0 / a.frames;
    const double* s = a.s;
    std::ostringstream os;
    std::ostringstream ns;   // (the noise metric and the churn samples: their own tail)
    if (a.noise)
    {   // TRAD = sum |L - L_prev| / sum L over the group's surface texels (still camera: the estimate's temporal noise)
        static const char* const kGrp[6] = {"all", "unflagged tiles", "listed tiles", "flagged, cut by the cap",
                                            "flagged far (>= 16 m)", "flagged near (< 4 m)"};
        ns << std::fixed << std::setprecision(4) << "; NOISE (RenderVKShadowNoiseStats; temporal |dL| / L, still camera):";
        for (int g = 0; g < 6; ++g)
        {
            const double* q = s + 64 + 3 * g;
            ns << " " << kGrp[g] << " " << (q[2] > 0.0 ? q[1] / q[2] : 0.0) << " (" << std::setprecision(0) << q[0] * f
               << " texels)" << std::setprecision(4) << (g < 5 ? "," : "");
        }
    }
    if (!mLvcChurn.empty())
    {   // (static churn: the instances DIRTY saw change, what they are)
        std::vector<std::pair<uint32_t, LvcChurn>> v(mLvcChurn.begin(), mLvcChurn.end());
        std::sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return x.second.n > y.second.n; });
        ns << std::fixed << std::setprecision(3) << "; static churn samples (" << v.size() << " instances):";
        for (size_t i = 0; i < std::min<size_t>(v.size(), 12u); ++i)
        {
            const LvcChurn& c = v[i].second;
            ns << " [id " << v[i].first << " x" << c.n << ((c.kinds & 2u) ? " xform" : "") << ((c.kinds & 4u) ? " flags" : "")
               << ((c.kinds & 8u) ? " geom" : "") << " flags 0x" << std::hex << c.flagsNew << " xor 0x"
               << (c.flagsOld ^ c.flagsNew) << std::dec << " move " << c.move << " rot " << std::scientific
               << std::setprecision(1) << c.rot << std::fixed << std::setprecision(3);
#if defined(LL_HAS_CLEAN_PIPELINE)
            LLUUID oid;
            S32 face = -1;
            if (LLCleanInstanceStore::instance().keyForId(v[i].first, oid, face))
            {
                LLViewerObject* obj = gObjectList.findObject(oid);
                ns << " obj " << oid.asString().substr(0, 8) << " face " << face;
                if (!obj) ns << " gone";
                else
                {
                    LLVOVolume* vo = obj->getPCode() == LL_PCODE_VOLUME ? (LLVOVolume*)obj : nullptr;
                    ns << " pc" << (U32)obj->getPCode() << (obj->isMesh() ? " mesh" : "")
                       << (obj->isAttachment() ? " att" : "") << (obj->getAvatarAncestor() ? " on-avatar" : "")
                       << (obj->mDrawable.notNull() && obj->mDrawable->isActive() ? " ACTIVE" : " static")
                       << (vo && vo->isAnimatedObject() ? " animesh" : "") << (vo && vo->isFlexible() ? " flexi" : "")
                       << (obj->flagUsePhysics() ? " physical" : "") << (obj->isRoot() ? " root" : " child")
                       << (obj->getAngularVelocity().isExactlyZero() ? "" : " angvel")
                       << (obj->getVelocity().isExactlyZero() ? "" : " vel");
                }
            }
            else ns << " (no store key)";
#endif
            ns << "]";
        }
        mLvcChurn.clear();
    }
    if (!a.occ)
    {
        os << std::fixed << std::setprecision(0) << "LVC stats (noise only) over " << a.frames << " frames" << ns.str();
        a = LvcAcc{};
        return os.str();
    }
    os << std::fixed << std::setprecision(0) << "LVC stats (RenderVKReSTIROcclusionCull 3) over " << a.frames
       << " frames, light samples/frame";
    static const char* const grp[3] = {"pass 0 slot 0 (reservoir pick)", "pass 0 other", "extra passes"};
    for (int g = 0; g < 3; ++g)
    {
        const double* q = s + g * 6;
        const double tr = std::max(q[0], 1.0);
        os << "; " << grp[g] << ": traced " << q[0] * f << std::setprecision(1) << " (reached " << 100.0 * q[1] / tr
           << "%, static-blocked " << 100.0 * q[2] / tr << "%, static-blocked near " << 100.0 * q[3] / tr
           << "%, mover-blocked " << 100.0 * q[4] / tr << "%), untraced " << std::setprecision(0) << q[5] * f;
    }
    const double mv = std::max(s[18], 1.0), mb = std::max(s[4] + s[10], 1.0);
    os << std::setprecision(1) << "; pass 0 mover-flagged samples " << std::setprecision(0) << s[18] * f
       << std::setprecision(1) << " (static walk also blocked " << 100.0 * s[19] / mv << "%; of the mover-blocked "
       << 100.0 * s[20] / mb << "%; kinds rigged " << 100.0 * s[21] / mv << "%, attachment " << 100.0 * s[22] / mv
       << "%, spinner " << 100.0 * s[23] / mv << "%, row changed " << 100.0 * s[24] / mv << "%)";
    const double c3 = std::max(s[25], 1.0);
    os << "; moving-caster texels/frame: pass-0 rays " << std::setprecision(0) << s[25] * f << ", carry alone " << s[26] * f
       << std::setprecision(1) << ", pass-0 texels with only redundant mover flags " << 100.0 * s[27] / c3 << "%";
    os << "; VIS jobs/texel pass 0 " << std::setprecision(2) << (s[30] > 0.0 ? s[28] / s[30] : 0.0) << " ("
       << std::setprecision(0) << s[28] * f << " jobs, " << s[30] * f << " texels), extra passes " << std::setprecision(2)
       << (s[31] > 0.0 ? s[29] / s[31] : 0.0) << " (" << std::setprecision(0) << s[29] * f << " jobs, " << s[31] * f
       << " texels)";
    {   // pass 0's traced samples by list kind (exact: every candidate / pick: RIS / dense window), and their static-blocked
        const double tl = std::max(s[32] + s[33] + s[34], 1.0);
        os << "; pass 0 by list: exact " << std::setprecision(1) << 100.0 * s[32] / tl << "% (static-blocked "
           << 100.0 * s[35] / std::max(s[32], 1.0) << "%), pick " << 100.0 * s[33] / tl << "% (static-blocked "
           << 100.0 * s[36] / std::max(s[33], 1.0) << "%), dense " << 100.0 * s[34] / tl << "%";
    }
    if (s[41] > 0.0 || s[44] > 0.0)
    {   // (LVC stage 3, RenderVKShadowReachMap) the reach maps: slots, usable, dirty; builds; SELECT's skips; VIS's verify
        os << "; reach maps (RenderVKShadowReachMap): lights with a slot " << std::setprecision(1) << s[41] * f
           << ", usable " << s[42] * f << ", built but dirty " << s[43] * f << ", builds/frame " << std::setprecision(2)
           << s[44] * f << " (rays " << std::setprecision(0) << s[45] * f << ", finished " << std::setprecision(2) << s[46] * f
           << "), static instance changes/frame " << std::setprecision(1) << s[47] * f << ", candidates skipped/frame "
           << std::setprecision(0) << s[40] * f << ", traced samples the maps call blocked/frame " << s[38] * f
           << ", of them REACHED (invariant, must be 0) " << s[39] * f << " (total " << s[39] << ")"
           << ", instances reclassified dynamic (thrash) " << s[48] << " (changes by part: transform / box "
           << std::setprecision(1) << s[49] * f << ", flags only " << s[50] * f << " (bits 0x" << std::hex
           << (uint32_t)s[51] << std::dec << "), LOD group / geometry only " << s[52] * f << ")";
        if (s[39] > 0.0) os << "; the breaks' nearest face-edge distance (texels) " << (15.0 - s[53]);
    }
    {
        static const char* const reasons[10] = {"off", "projector", "attached", "no-slot", "unusable",
                                                "near", "wide-disc", "open-map", "margin", "map-blocked"};
        os << "; ASTRA reach admission pass0 jobs/frame (all/static-blocked-including-near):";
        for (uint32_t k = 0; k < 10u; ++k)
            os << " " << reasons[k] << "=" << std::setprecision(0) << s[54u + k] * f << "/" << s[148u + k] * f;
    }
    std::vector<LvcLightAcc>& L = a.lights;
    if (s[39] > 0.0)
    {   // (stage 3 verify) the lights whose reach map was wrong: violations, the largest excess, the map's age then
        std::sort(L.begin(), L.end(), [](const LvcLightAcc& x, const LvcLightAcc& y) { return x.c[5] > y.c[5]; });
        os << "; INVARIANT BREAKS by light:";
        for (size_t i = 0; i < std::min<size_t>(L.size(), 4u) && L[i].c[5] > 0.0; ++i)
            os << " [tag " << L[i].tag << " r " << std::setprecision(1) << L[i].radius << " at (" << L[i].pos[0] << ", "
               << L[i].pos[1] << ", " << L[i].pos[2] << ") breaks " << std::setprecision(0) << L[i].c[5] << " of "
               << L[i].c[4] << " map-blocked, max excess " << std::setprecision(3) << L[i].excess << " m, map age "
               << L[i].age << " frames]";
    }
    // the top 8 lights by statically blocked pass-0 rays (tag, kind, radius, position; traced / reached / blocked / mover)
    std::sort(L.begin(), L.end(), [](const LvcLightAcc& x, const LvcLightAcc& y) { return x.c[2] > y.c[2]; });
    double totBlk = 0.0;
    for (const LvcLightAcc& e : L) totBlk += e.c[2];
    os << "; top lights by static-blocked rays (of " << std::setprecision(0) << totBlk * f << "/frame):";
    for (size_t i = 0; i < std::min<size_t>(L.size(), 8u); ++i)
    {
        const LvcLightAcc& e = L[i];
        const double tr = std::max(e.c[0], 1.0);
        os << " [tag " << e.tag << (e.proj ? " proj" : " point") << " r " << std::setprecision(1) << e.radius << " at ("
           << e.pos[0] << ", " << e.pos[1] << ", " << e.pos[2] << ") traced " << std::setprecision(0) << e.c[0] * f
           << std::setprecision(1) << " reached " << 100.0 * e.c[1] / tr << "% static " << 100.0 * e.c[2] / tr
           << "% mover " << 100.0 * e.c[3] / tr << "%]";
    }
    os << ns.str();
    a = LvcAcc{};
    return os.str();
}

std::string LLCleanRendererVK::rsStatsText()
{
    RsStatAcc& a = mRsStatAcc;
    if (!a.frames) return std::string();
    const double k = 1.0 / a.frames;
    const double* s = a.stat;
    const double tex = s[3] + s[4];
    std::ostringstream os;
    os << std::fixed << std::setprecision(1) << "ReSTIR DI over " << a.frames << " frames: light set "
       << a.setN * k << " lights (" << a.pointN * k << " point; "
       << a.movedN * k << " moving, " << a.flickN * k << " flickering); grid cells with candidates "
       << std::setprecision(0) << s[0] * k << ", candidates/cell " << std::setprecision(2)
       << (s[0] > 0.0 ? s[1] / s[0] : 0.0) << " (max " << std::setprecision(0) << s[2] * k << ")"
       << "; pass-15 texels from the grid " << std::setprecision(3) << (tex > 0.0 ? s[3] / tex : 0.0)
       << " (cluster walk outside it " << (tex > 0.0 ? s[4] / tex : 0.0) << ")"
       << "; reservoir M/texel " << std::setprecision(2) << (s[3] > 0.0 ? s[5] / s[3] : 0.0)
       << ", temporal kept " << std::setprecision(3) << (s[3] > 0.0 ? s[6] / s[3] : 0.0)
       << ", spatial taps kept/texel " << (s[3] > 0.0 ? s[7] / s[3] : 0.0);
    if (a.visFrames)
    {   // (#1031 split core: c_defer_shadowvis / this = the cost of one light sample's walk(s))
        const double vf = 1.0 / a.visFrames;
        os << "; shadow VIS light samples/frame " << std::setprecision(0) << a.visJobs * vf << " (peak "
           << a.visJobsPeak << "), per surface texel " << std::setprecision(2)
           << (a.visTex > 0.0 ? a.visJobs / a.visTex : 0.0);
    }
    if (a.emFrames)
    {
        const double ef = 1.0 / a.emFrames;
        const double* e = a.emStat;
        os << "; emissive (RenderVKReSTIREmissive " << mRsEmMode << ") emitters " << std::setprecision(1)
           << a.emInst * ef << ", triangles live " << std::setprecision(0) << a.em * ef
           << " / cap " << a.emCap << " (peak " << a.emPeak << ", skipped or dropped/frame " << std::setprecision(1)
           << a.emDrop * ef << "), grid cells with emitting triangles (#1006: of the cells re-gathered that frame) " << std::setprecision(0) << e[0] * ef
           << ", candidate triangles/cell " << std::setprecision(2) << (e[0] > 0.0 ? e[1] / e[0] : 0.0)
           << "; pass-15 texels with emitter candidates " << std::setprecision(3) << (tex > 0.0 ? e[2] / tex : 0.0)
           << ", emitter samples traced/frame " << std::setprecision(0) << e[3] * ef
           << "; light takeover (RenderVKReSTIREmissiveLightTakeover): point lights with emitting faces "
           << std::setprecision(1) << a.emTake[0] * ef << ", taken over " << a.emTake[1] * ef
           << ", refused (a block dropped) " << a.emTake[2] * ef
           << "; records reused / rebuilt per frame " << a.emCopied * ef
           << " / " << a.emBuilt * ef << "; bounded pairs " << std::setprecision(0) << a.emPairs*ef << "/" << kRsEmPairCap
           << " (peak " << a.emPairsPeak << ", no gather fallback)"
           << "; discovery rows/offers " << a.emBudget[0]*ef << "/" << a.emBudget[1]*ef
           << ", cohort evictions " << a.emBudget[2]*ef << ", active/deferred faces "
           << a.emBudget[4]*ef << "/" << a.emBudget[5]*ef << ", attempted cells " << a.emBudget[14]*ef
           << "/" << a.emBudget[8]*ef << ", radius-clamped/centroid-support records "
           << a.emBudget[12]*ef << "/" << a.emBudget[13]*ef << ", invariant failures " << a.emBudget[15]
           << "; long cells " << a.emLong*ef << "/frame, longest list " << a.emLongest;
        if (a.emIncFrames)
        {   // (#1006 INCREMENTAL CELLS + WEAK OVERLAY, clean_regir.glsl: EM_CCH_STAT per lists build; the cells
            // with emitting triangles above count only the cells re-gathered that frame)
            const double xf = 1.0 / a.emIncFrames;
            os << std::setprecision(1) << "; bounded cell rebuild: legacy_changed/frame "
               << a.emDirty * xf << ", re-gathered/frame " << a.emRegather * xf << " (forced or refreshed "
               << a.emForced * xf << ", forward-map misses among them " << a.emMiss * xf << "), kept/frame "
               << a.emKept * xf << ", max staleness " << std::setprecision(0) << a.emStaleMax << " header frames, full invalidations "
               << a.emFull << " / " << a.emIncFrames << "; weak emitters on the overlay "
               << "(RenderVKReSTIREmissiveWeakPower)/frame " << std::setprecision(1) << a.emWeak * xf
               // (#1009 CLUSTER STATICS, clean_regir.glsl: statics traced on their cluster RT cut, emitting through
               //  their legacy row -- of the emitters above, and their triangles before the cap)
               << "; cluster-LOD statics among them (RenderVKReSTIREmissiveClusterStatic): emitters "
               << a.emClInst * xf << ", their triangles " << std::setprecision(0) << a.emClTri * xf
               // (mode 0: emitters whose reach misses the grid's outermost level -- not listed, not in the cap)
               << "; legacy outside-grid counters (no admission camera cull)/frame " << std::setprecision(1)
               << a.emOutInst * xf << ", their triangles " << std::setprecision(0) << a.emOutTri * xf;
            if (a.emSkipFrames)
            {   // (#1009 SKIP DIAGNOSTIC, RenderVKReSTIREmissiveSkipStats: emitting instances left out, by why)
                const double sf = 1.0 / a.emSkipFrames;
                const double* q = a.emSkip;
                os << std::setprecision(1) << "; emitting instances left out/frame (RenderVKReSTIREmissiveSkipStats): "
                   << "rigged legacy " << q[0] * sf << ", rigged cluster " << q[1] * sf
                   << ", cluster static without a legacy row " << q[2] * sf << ", worn / own attachment " << q[3] * sf
                   << ", not in the TLAS " << q[4] * sf;
            }
        }
    }
    if (a.occFrames)
    {   // (#869 OCCLUSION CULL, RenderVKReSTIROcclusionCull: clean_deferred_light.comp STATS)
        const double of = 1.0 / a.occFrames;
        const double* o = a.occ;
        os << std::fixed << std::setprecision(1)
           << "; occlusion cull: patches live " << o[0] * of << " / " << kRsOccCells << ", culling " << o[1] * of
           << ", culled lights/culling patch " << (o[1] > 0.0 ? o[2] / o[1] : 0.0)
           << " (patches with 1-3 / 4-15 / 16-63 / 64+: " << o[3] * of << " / " << o[4] * of << " / " << o[5] * of
           << " / " << o[6] * of << "), new culls/frame " << o[8] * of << ", re-admissions/frame " << o[7] * of
           << ", probes/frame " << std::setprecision(0) << o[9] * of << std::setprecision(1)
           << ", patch claims refused (chain full)/frame " << o[10] * of << ", patches freed/frame " << o[11] * of
           << ", counts dropped with their light/frame " << o[12] * of;
    }
    mRsStatAcc = RsStatAcc{};
    return os.str();
}

// (#91) RenderVKReSTIREmissive 1 with ReSTIR DI on and the emissive kernel built: the emitters may
// light this frame, so ReSTIR DI (and pass 15) run even without a light in the set.
bool LLCleanRendererVK::rsEmLightWanted() const
{
    U32 em = 0;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_em(gSavedSettings, "RenderVKReSTIREmissive", 1U);
    em = s_em;
#endif
    return em == 1u && mRsEmPipe != VK_NULL_HANDLE;
}

// (#91) LIGHT TAKEOVER (clean_regir.glsl LIGHT TAKEOVER): this frame's table from the world frame's
// (instance, light) pairs -- the point lights' emitting faces. Rows grouped by light in light order (a
// light's rows are its emitter entries [first, first + rows): mode 1 reads them as one range), at most
// kRsEmTakeN (a light that does not fit whole is left out: it keeps its point light); then the rows again
// sorted by instance for mode 0's binary search. Bounded: (256 + kRsEmTakeN) x 2 words.
void LLCleanRendererVK::rsEmTakeTable(const LLVKWorldFrame& wf)
{
    mRsEmTakeRows = mRsEmTakeLights = 0;
    if (wf.light_takeover.empty() || wf.light_takeover.size() > kRsEmTakeN)
    {
        mRsEmTake.clear();
        return;
    }
    const size_t nL = std::min<size_t>(std::min<size_t>(mLightsCPU.size(), wf.lights.size()), 256);
    static std::vector<std::pair<uint32_t, uint32_t>> byLight;   // (light, instance)
    byLight.clear();
    for (const auto& p : wf.light_takeover)
    {
        const uint32_t li = p.second;
        if (li >= nL) continue;
        const VKLightGPU& g = mLightsCPU[li];
        // (a point light with a live record; the GPU checks the rest: the grid, the radius, the colour)
        if (g.proj_origin[3] >= 0.5f || !(g.pos_radius[3] > 0.f) || g.pos_radius[2] < -1.0e8f) continue;
        byLight.emplace_back(li, p.first);
    }
    std::sort(byLight.begin(), byLight.end());
    byLight.erase(std::unique(byLight.begin(), byLight.end()), byLight.end());
    {   // (an instance belongs to one prim, so to one light: a repeated instance keeps its first light)
        static std::unordered_set<uint32_t> seen;
        seen.clear();
        byLight.erase(std::remove_if(byLight.begin(), byLight.end(),
                                     [](const std::pair<uint32_t, uint32_t>& p) { return !seen.insert(p.second).second; }),
                      byLight.end());
    }
    mRsEmTake.assign((size_t)(256u + kRsEmTakeN) * 2u, 0u);
    static std::vector<std::pair<uint32_t, uint32_t>> byInst;   // (instance, row | light << 16)
    byInst.clear();
    uint32_t rows = 0;
    for (size_t i = 0; i < byLight.size();)
    {
        size_t j = i;
        while (j < byLight.size() && byLight[j].first == byLight[i].first) ++j;
        const uint32_t n = (uint32_t)(j - i);
        if (rows + n > kRsEmTakeN) break;   // (lights are in importance order: the rest keep their light)
        const uint32_t li = byLight[i].first;
        mRsEmTake[li * 2u] = rows;
        mRsEmTake[li * 2u + 1u] = n;
        for (size_t k = i; k < j; ++k) byInst.emplace_back(byLight[k].second, (rows + (uint32_t)(k - i)) | (li << 16));
        rows += n;
        ++mRsEmTakeLights;
        i = j;
    }
    std::sort(byInst.begin(), byInst.end());   // (unique instances: one row each)
    for (size_t k = 0; k < byInst.size(); ++k)
    {
        mRsEmTake[(256u + k) * 2u] = byInst[k].first;
        mRsEmTake[(256u + k) * 2u + 1u] = byInst[k].second;
    }
    mRsEmTakeRows = rows;   // (= byInst.size(): the rows ARE the reserved emitter entries [0, rows))
}

// ======================= MERGED RT SHADOWS (#195 + #85, RenderVKRTShadowMerge) =======================

// The merge's preconditions this frame: the setting, the stochastic projector path and the split
// deferred lighting asked for, on screen, the sun / moon ray traced THIS frame (selectLitVariant's
// mSunRtNow: RT warm, TLAS valid, not over the RT budget) AND ray-traced projector records, the
// kernels and a valid TLAS. needImages: the denoiser's sun channel is allocated too (selectLitVariant's
// prediction -- pdnFrame allocates it on frames that want it, so a first frame runs today's path)
// and no lit debug twin is wanted (those frames light with the uber kernel / forward).
bool LLCleanRendererVK::shadowMergeWanted(bool needImages) const
{
    U32 merge = 1, stoch = 0, split = 1;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32>  s_merge(gSavedSettings, "RenderVKRTShadowMerge", 1U);
    static LLCachedControl<U32>  s_stoch(gSavedSettings, "RenderVKProjStochastic", 1U);
    merge = s_merge; stoch = s_stoch;
#endif
    split = deferSplitMode();   // (#824: RenderVKDeferredSplit 2 = auto per GPU class)
    // (#1608: a layered stereo split frame merges as the desktop's does -- unless SlipstreamVRPointShadows picked the
    //  layered RT shadow mask: vrStochShadows)
    if (merge == 0u || stoch == 0u || split == 0u || mOffscreen || !vrStochShadows()) return false;
    if (!mSunRtNow || mProjMode != 2u || (mProjRecords == 0u && mRsSetN == 0u && !rsEmLightWanted()))
        return false;
    if (!mPdn || !mPdnMergePipe || !mPdnSamplePipe || !mRtReflLayout || !mPdn->pipelinesReady() || !mDeferSplitOk
        || !mDsetDraw0)
        return false;
    if (!mRT || !mRT->tlasValid() || mRT->consumerSet() == VK_NULL_HANDLE) return false;
    if (needImages && (!mPdn->hasSun() || litDebugWanted())) return false;
    return true;
}

// (#1608) THE STOCHASTIC LOCAL-LIGHT SHADOWS IN STEREO (SlipstreamVRPointShadows): 1 (the default) -- pass 15 / ReSTIR DI,
// the merged RT shadows and the denoiser per view, as on the desktop; 0 / 2 / 3 -- the layered RT shadow mask shadows
// the projectors (0: no point-light shadows; 2 / 3: the mask's VR point lights, the cheaper option). The desktop: always.
bool LLCleanRendererVK::vrStochShadows() const
{
    if (!stereoActive()) return true;
    U32 pts = 1;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_vrPts(gSavedSettings, "SlipstreamVRPointShadows", 1U);
    pts = s_vrPts;
#endif
    return pts == 1u;
}

// Once per frame (selectLitVariant): the WEAK-SUN weight and the light's motion for the merged
// pass's sun / moon channel.
//   * WEIGHT: the active light's colour (frame info atmos lane 2: the sun's, or the moon's at night)
//     luminance x the sun scale (lane 12.z), against RenderVKRTShadowMergeSunMin = t: 0 below t (no
//     sun ray at all -- night, a dim moon cost nothing), a smooth ramp to 1 at 2t (the shadow fades
//     toward lit: never a pop), 1 above. 0 = off (always traced).
//   * MOTION: the light direction's step since last frame in disc radii (the RT shadow mask's
//     precise atan2 measure: rtMaskPushFill), which caps the sun history (dnSunMoveCap); a sun <->
//     moon switch (lane 9.w) or a jump past 10 degrees (time of day set, a teleport to another
//     sky) restarts it.
void LLCleanRendererVK::shadowMergeSunState()
{
    F32 tmin = 0.02f, soft = 0.01f;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<F32> s_min(gSavedSettings, "RenderVKRTShadowMergeSunMin", 0.02f);
    static LLCachedControl<F32> s_soft(gSavedSettings, "RenderVKRTShadowSoftness", 0.01f);
    tmin = s_min; soft = s_soft;
#endif
    const float* lc = &mAtmos[2 * 4];
    const float lum = (0.2126f * lc[0] + 0.7152f * lc[1] + 0.0722f * lc[2]) * std::max(mAtmos[12 * 4 + 2], 0.f);
    float w = 1.f;
    if (tmin > 0.f)
    {
        const float t = std::clamp((lum - tmin) / tmin, 0.f, 1.f);
        w = t * t * (3.f - 2.f * t);
    }
    mShadowMergeSunW = std::isfinite(w) ? w : 1.f;
    mShadowMergeSunLum = std::isfinite(lum) ? lum : 0.f;
    if (mOffscreen) return;   // (a snapshot's frame: the live sequence's motion is measured on screen)
    // CALIBRATION LOG (merged frames only; the band changes with the time of day, not per frame):
    // the measure against RenderVKRTShadowMergeSunMin whenever the sun / moon ray turns off, fades
    // or is full.
    if (mShadowMergeNow || mShadowMergeTry)
    {
        const int band = mShadowMergeSunW <= 0.f ? 0 : (mShadowMergeSunW >= 1.f ? 2 : 1);
        if (band != mShadowMergeSunBand)
        {
            mShadowMergeSunBand = band;
            static const char* const kBand[3] = {"no sun / moon ray (weak light)", "fading toward lit", "full"};
            LL_INFOS("VKShadow") << "merged RT shadows: sun / moon " << kBand[band] << " -- light measure "
                                 << mShadowMergeSunLum << " (colour luminance x sun scale; RenderVKRTShadowMergeSunMin "
                                 << tmin << ": no ray below it, full from " << 2.f * tmin << ")" << LL_ENDL;
        }
    }
    const float* ld = &mAtmos[7 * 4];
    const float sunUp = mAtmos[9 * 4 + 3];
    mShadowMergeSunCut = false;
    mShadowMergeSunMove = 0.f;
    if (mShadowMergePrevOk)
    {
        double a[3] = {ld[0], ld[1], ld[2]};
        double b[3] = {mShadowMergePrevDir[0], mShadowMergePrevDir[1], mShadowMergePrevDir[2]};
        const double la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        const double lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
        double ang = 0.0;
        if (la > 1e-12 && lb > 1e-12)
        {
            for (int i = 0; i < 3; ++i) { a[i] /= la; b[i] /= lb; }
            const double cx = a[1] * b[2] - a[2] * b[1], cy = a[2] * b[0] - a[0] * b[2], cz = a[0] * b[1] - a[1] * b[0];
            ang = std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz), a[0] * b[0] + a[1] * b[1] + a[2] * b[2]);
        }
        const double disc = std::tan(std::max((double)std::clamp(soft, 0.f, 1.f) * 5.0, 0.05) * 0.01745329252);
        mShadowMergeSunMove = std::isfinite(ang) ? (float)std::min(ang / disc, 64.0) : 64.f;
        if (!std::isfinite(ang) || ang > 10.0 * 0.01745329252 || (sunUp > 0.5f) != (mShadowMergePrevSunUp > 0.5f))
            mShadowMergeSunCut = true;
    }
    mShadowMergePrevDir[0] = ld[0]; mShadowMergePrevDir[1] = ld[1]; mShadowMergePrevDir[2] = ld[2];
    mShadowMergePrevSunUp = sunUp;
    mShadowMergePrevOk = true;
}

// One line whenever a shadow source changes path (on screen; at most one per 2 s, the switches in
// between counted): sun / moon = merged RT | RT mask | maps | off, projectors = merged RT |
// stochastic RT | RT mask | RT, not traced this frame | maps | RT, none in view | off.
void LLCleanRendererVK::shadowPathLog()
{
    uint32_t sun = 0, proj = 0;
    if (!mShadowOn) sun = 0;                                   // off
    else if (!mSunRtNow) sun = 1;                              // maps
    else if (mShadowMergeNow) sun = 2;                         // merged RT
    else sun = 3;                                              // RT mask (#1415: selectLitVariant traces only with it)
    if (mProjMode == 0u) proj = 0;                             // off
    else if (mProjMode == 1u) proj = 1;                        // maps
    else if (mProjRecords == 0u) proj = 2;                     // RT, none in view
    else if (mShadowMergeNow) proj = 3;                        // merged RT
    else if (mPdnNow) proj = 4;                                // stochastic RT (denoised)
    else if (mRtMaskOn) proj = 5;                              // RT mask (exact, <= 4 per pixel)
    else proj = 6;                                             // RT, not traced this frame (TLAS / RT mask unavailable)
    const uint32_t state = sun | (proj << 4);
    static double s_next = 0.0;
    // (2026-11-21: the count was of FRAMES that differed from the last LOGGED state, so one withdrawn frame followed
    //  by 2 s of the old path read "88 changes". Now: real transitions frame to frame, and the frames whose merge
    //  prediction was withdrawn, with the reasons.)
    if (mShadowMergeNow) ++mMemLogMergedFrames;
    else if (mRtMaskOn) ++mMemLogMaskFrames;
    if (mShadowMergeWhyNow != 0u)
    {
        ++mShadowPathWithdrawn;
        mShadowPathWhy |= mShadowMergeWhyNow;
    }
    if (state != mShadowPathPrev)
    {
        if (mShadowPathPrev != 0xFFFFFFFFu) ++mShadowPathFlips;
        mShadowPathPrev = state;
    }
    if (state == mShadowPathLogged) return;
    const double now = pdnClock();
    if (now < s_next) return;
    s_next = now + 2.0;
    const uint32_t s_flips = mShadowPathFlips;
    std::string withdrawn;
    if (mShadowPathWithdrawn != 0u)
    {
        withdrawn = " [merge withdrawn on " + std::to_string(mShadowPathWithdrawn) + " frame(s):";
        if (mShadowPathWhy & 1u) withdrawn += " no G-buffer;";
        if (mShadowPathWhy & 2u) withdrawn += " split lighting not running (intermediates re-created / refused);";
        if (mShadowPathWhy & 4u) withdrawn += " denoiser not sampling;";
        if (mShadowPathWhy & 8u) withdrawn += " no denoiser sun channel;";
        if (mShadowPathWhy & 16u) withdrawn += " no merge kernel;";
        withdrawn += "]";
    }
    static const char* const kSun[4] = {"off", "shadow maps", "merged RT (one pass + denoiser with the projectors)",
                                        "RT shadow mask"};
    static const char* const kProj[7] = {"off", "shadow maps", "ray traced (none in view)",
                                         "merged RT (one pass + denoiser with the sun / moon)",
                                         "stochastic RT (denoised)", "RT shadow mask (exact, up to 4 per pixel)",
                                         "ray traced, not this frame (TLAS or RT shadow mask unavailable)"};
    LL_INFOS("VKShadow") << "shadow paths: sun / moon = " << kSun[sun]
                         << (sun == 2u ? " [light measure " + std::to_string(mShadowMergeSunLum) + ", weight "
                                             + std::to_string(mShadowMergeSunW) + "]"
                                       : std::string())
                         << "; projectors = " << kProj[proj]
                         << (s_flips > 1 ? " [" + std::to_string(s_flips) + " changes since the last line]" : std::string())
                         << withdrawn << LL_ENDL;
    mShadowPathFlips = 0;
    mShadowPathWithdrawn = 0;
    mShadowPathWhy = 0;
    mShadowPathLogged = state;
}

// recordDeferredLighting, merged frames: the merged pass + the denoiser BEFORE the SHADOW kernel
// (which reads the denoised sun / moon), then their final image -> its reads.
void LLCleanRendererVK::recordShadowMergeFirst(VkCommandBuffer cmd, const VKDeferPush& p, uint32_t viewOff, bool onAsync)
{
    recordProjStoch(cmd, p, viewOff, onAsync);
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &d);
}
