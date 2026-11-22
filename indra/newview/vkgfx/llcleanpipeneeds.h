/**
 * @file llcleanpipeneeds.h
 * @brief COMPILE ONLY WHAT IS NEEDED: the optional pipeline families and the settings that need
 *        them (pure, no Vulkan; scripts/check pipeline-needs compiles it).
 *
 * The renderer fills Settings from the live settings every frame (LLCleanRendererVK::
 * updatePipelineNeeds) and starts a family's compile only while needs() has its bit: at login
 * nothing of a family whose features are off compiles, and switching one on mid-session starts
 * its background job then (the existing async batch; the feature keeps its fallback -- plain
 * lighting, shadow maps, the ordered alpha path -- until the job lands; nothing is drawn with a
 * missing pipeline). Switching a feature off destroys nothing.
 * The same mask is handed to vk_pipeline (vkSetPipelineFamiliesAllowed): a job tagged with a
 * family outside it is REFUSED and logged (PipelineJobOpts::family) -- the guard against a
 * request path that forgot the rule.
 *
 * Families NOT listed here compile with the core set (always needed, or already lazy: they are
 * built by their own feature's first use -- god rays, motion blur, weather, cluster LOD, the
 * upscalers, VR space warp, the effects stack; see the per-family table in the commit / report).
 * The user's settings decide, not this frame's content: a feature that is on but has nothing to
 * draw right now (no projector in view) still needs its pipelines when the content appears. The
 * VRAM ladder's temporary stand-downs do not un-need a family (the pipelines are wanted again the
 * moment it lifts; compiling them costs host memory, not video memory).
 */
#pragma once

#include <cstdint>

namespace llpipeneeds
{
enum Family : uint32_t
{
    // The ray-traced effects' kernels (RT shadow mask, RT reflections / mirrors, ray-traced GI,
    // the stochastic projector shadows / ReSTIR, the RT probe deferred lighting): the renderer's
    // deferred group 0 (mRtPipelineJob).
    FAM_RT_KERNELS = 1u << 0,
    // The ray-query LIT variant (the clean_draw.frag ray-query twins of every lit slot -- the
    // heaviest compiles of the renderer, ~0.94M SPIR-V words each): only ray-traced sun or
    // projector shadows bind it (selectLitVariant). Deferred group 1 (mRtLitJob); the setting-
    // specialised sets' ray-query phase follows it.
    FAM_RT_LIT = 1u << 1,
    // WBOIT (#1410, RenderVKAlphaOIT): only the selected mode's pipelines, and its ray-query
    // twins (mode 1's accumulation, mode 2's resolve) only with the ray-query lit variant.
    FAM_WBOIT_M1 = 1u << 2,
    FAM_WBOIT_M2 = 1u << 3,
    FAM_WBOIT_RT = 1u << 4,
    // Speculative setting-specialised sets for keys the settings do not select
    // (RenderVKSpecPrewarm): never by default.
    FAM_SPEC_PREWARM = 1u << 5,
    // MOMENT-BASED OIT (RenderVKAlphaMBOIT, LLCleanAlphaMBOITVK): its moments / cutoff / shade / resolve
    // pipelines, and the ray-query shade twin only with the ray-query lit variant.
    FAM_MBOIT = 1u << 6,
    FAM_MBOIT_RT = 1u << 7,
    // SPLIT TRANSPARENCY (RenderVKBlendSplit, LLCleanBlendSplitVK): the volume / front surface / front light / blend
    // pipelines, and the volume's and the front light's ray-query twins only with the ray-query lit variant.
    FAM_BLEND_SPLIT = 1u << 8,
    FAM_BLEND_SPLIT_RT = 1u << 9,
    FAM_COUNT_BITS = 10
};

struct Settings
{
    bool rtDevice = false;          // the RT foundation exists (device has ray query + RenderVKRayTracing)
    bool sunShadowsOn = false;      // sun / moon shadows enabled (RenderShadowDetail > 0)
    uint32_t sunShadows = 1;        // RenderVKSunShadows (2 = ray traced)
    uint32_t projShadows = 1;       // RenderVKProjectorShadows (2 = ray traced)
    uint32_t shadowDetail = 2;      // RenderShadowDetail (projector shadows need >= 2)
    uint32_t giMode = 0;            // RenderVKGI (or SlipstreamVRGI's tier in VR)
    bool reflectionsOn = true;      // RenderReflectionsEnabled
    int32_t probeDetail = 1;        // RenderReflectionProbeDetail (3 = ray-traced captures)
    uint32_t rtReflections = 1;     // RenderVKRTReflections (1 = hybrid RT reflections)
    bool rtMirrors = false;         // RenderVKRTMirrors (or a stereo session: VR mirrors trace)
    uint32_t aoMethod = 1;          // RenderVKAOMethod (2 = ray traced)
    bool ssao = true;               // RenderDeferredSSAO
    uint32_t debugView = 0;         // RenderVKDebugView (the RT debug views trace)
    uint32_t alphaOit = 2;          // RenderVKAlphaOIT (0 off / 1 / 2)
    uint32_t maxColorAttachments = 8;   // mode 2 needs 5 accumulation targets
    uint32_t specPrewarm = 0;       // RenderVKSpecPrewarm
    uint32_t alphaMboit = 0;        // RenderVKAlphaMBOIT (0 off / 1 four / 2 six power moments)
    bool blendFrontLight = true;    // RenderVKBlendFrontLight (on: the sorted blend FS is the no-ray module)
    uint32_t blendSplit = 0;        // RenderVKBlendSplit (0 off / 1 the split transparency)
};

inline bool rtDebugView(uint32_t v) { return v == 13u || v == 14u || v == 15u || v == 18u || v == 30u; }

// Ray-traced sun or projector shadows: the ray-query lit variant (and its WBOIT twins) are bound.
inline bool rtLitNeeded(const Settings& s)
{
    if (!s.rtDevice) return false;
    return (s.sunShadowsOn && s.sunShadows == 2u) || (s.projShadows >= 2u && s.shadowDetail >= 2u) || rtDebugView(s.debugView);
}

// Any ray-traced consumer whose kernels live in the deferred group.
inline bool rtKernelsNeeded(const Settings& s)
{
    if (!s.rtDevice) return false;
    if (rtLitNeeded(s)) return true;
    if (s.giMode != 0u) return true;
    if (s.reflectionsOn && s.probeDetail >= 3) return true;
    if (s.rtReflections == 1u) return true;
    if (s.rtMirrors) return true;
    if (s.aoMethod == 2u && s.ssao) return true;
    return false;
}

// The WBOIT mode the frame will run (0 = the ordered path): mode 2 needs 5 colour attachments.
inline uint32_t oitMode(const Settings& s)
{
    const uint32_t want = s.alphaOit > 2u ? 2u : s.alphaOit;
    return (want == 2u && s.maxColorAttachments < 5u) ? 1u : want;
}

inline uint32_t needs(const Settings& s)
{
    uint32_t m = 0;
    if (rtKernelsNeeded(s)) m |= FAM_RT_KERNELS;
    if (rtLitNeeded(s)) m |= FAM_RT_LIT;
    const uint32_t oit = oitMode(s);
    if (oit == 1u) m |= FAM_WBOIT_M1;
    if (oit == 2u) m |= FAM_WBOIT_M2;
    if (oit != 0u && rtLitNeeded(s)) m |= FAM_WBOIT_RT;   // (mode 1's accumulation twins / mode 2's ray-query resolve)
    if (s.specPrewarm > 0u) m |= FAM_SPEC_PREWARM;
    if (s.alphaMboit != 0u) m |= FAM_MBOIT;
    if (s.alphaMboit != 0u && rtLitNeeded(s) && !s.blendFrontLight) m |= FAM_MBOIT_RT;   // (the full tier traces only as the sorted path does)
    if (s.blendSplit != 0u) m |= FAM_BLEND_SPLIT;
    if (s.blendSplit != 0u && rtLitNeeded(s)) m |= FAM_BLEND_SPLIT_RT;   // (the volume's and the front light's traced twins)
    return m;
}

inline const char* familyName(uint32_t bit)
{
    switch (bit)
    {
    case FAM_RT_KERNELS:   return "ray-traced effect kernels";
    case FAM_RT_LIT:       return "ray-query lit variant";
    case FAM_WBOIT_M1:     return "WBOIT mode 1";
    case FAM_WBOIT_M2:     return "WBOIT mode 2";
    case FAM_WBOIT_RT:     return "WBOIT ray-query twins";
    case FAM_SPEC_PREWARM: return "speculative setting-specialised sets";
    case FAM_MBOIT:        return "MBOIT";
    case FAM_MBOIT_RT:     return "MBOIT ray-query shade";
    case FAM_BLEND_SPLIT:  return "split transparency";
    case FAM_BLEND_SPLIT_RT: return "split transparency ray-query twins";
    default:               return "?";
    }
}
} // namespace llpipeneeds
