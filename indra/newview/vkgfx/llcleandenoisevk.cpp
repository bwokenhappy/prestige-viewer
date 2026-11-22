/**
 * @file llcleandenoisevk.cpp
 * @brief Diffuse / specular lighting denoiser (#85 phase 2): see llcleandenoisevk.h and
 *        indra/vkclean/shaders/clean_pdn_common.glsl. The renderer side (the projector
 *        stochastic sampler, its settings, the set 3 bindings) is llcleanprojstochvk.cpp.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 * $/LicenseInfo$
 */
#include "linden_common.h"

#include "llcleandenoisevk.h"
#include <optional>
#include "llcleanrenderervk.h"

#include "vk_common.h"
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
VkShaderModule dnModule(VkDevice dev, const uint32_t* code, size_t bytes, const char* name)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
    vkNoteShaderModule(m, code, bytes, name);   // (names the batch report / straggler lines)
    return m;
}

// (#1608 LAYERED STEREO) kViewCount (clean_view.glsl, constant 20 -- GraphicsPipelineDesc::kViewCountConstId) = views
// for the passes' stereo pipelines (they run the views in one dispatch, z = views); nullptr at one view (as before).
// Static: a queued pipeline keeps the pointer.
const VkSpecializationInfo* dnViewSpec(uint32_t views)
{
    if (views <= 1u) return nullptr;
    static const VkSpecializationMapEntry kEntry{GraphicsPipelineDesc::kViewCountConstId, 0u, sizeof(int32_t)};
    static const int32_t kViews[3] = {2, 3, 4};
    static const VkSpecializationInfo kInfo[3] = {{1u, &kEntry, sizeof(int32_t), &kViews[0]},
                                                  {1u, &kEntry, sizeof(int32_t), &kViews[1]},
                                                  {1u, &kEntry, sizeof(int32_t), &kViews[2]}};
    return &kInfo[std::min<uint32_t>(views, 4u) - 2u];
}

// clean_pdn_common.glsl IMAGES: IN rgba16f[2], MOT rgba32ui, GEOM rgba32ui[2], HIST rgba32ui[2],
// OUT rgba16f[2] -- 112 B per texel. With the sun / moon channel (the merged RT shadow pass):
// IN / OUT one more layer (2), HIST two more (2 + parity) -- 160 B per texel. (#869) With the SPLIT
// channel (clean_pdn_common.glsl SPLIT CHANNEL): GEOM three more (2 + parity, 4 the final), IN / OUT
// the sun layer (allocated with it either way, so the split layer is always the last) + one more
// -- +64 B per texel with the sun channel, +80 B without it.
// (THE LEAN HISTORY) The old HIST (colour 2 + sun 2 + ages 2 + split ages 2 layers) is three images now, the same
// bytes in all: HIST the diffuse / specular colours (2), AGE the ages / countdowns / mover record (2; binding 6),
// SUNHIST the sun / moon channel's history (2, only with the sun channel; binding 7).
// HIST and SUNHIST are the 1 x 1 placeholders while lean (HistMode) -- the shaders tell by their extent.
// (PER-PIXEL MEMORY, 2026-11-13) The split slots' ages left AGE for their own image, SPLITAGE (binding 8,
// rgba16ui[2]: 16 B per texel where AGE's two float32 split layers took 32; the 1 x 1 placeholder without the split
// channel), and the GI instance's AGE is r32ui (THE NARROW AGE: its age word is x alone -- 8 B per texel, not 32).
// Both formats are in Vulkan's mandatory storage-image format set (no feature / tier gate).
struct DnImg { VkFormat f; uint32_t layers; uint32_t sunLayers; uint32_t splitLayers; uint32_t splitSunLayers = 0u; };
constexpr uint32_t kDnImages = 8u;   // IN, MOT, GEOM, HIST, OUT, AGE, SUNHIST, SPLITAGE (set 1 bindings 0-4, 6-8)
constexpr uint32_t kDnHistIdx = 3u, kDnAgeIdx = 5u, kDnSunHistIdx = 6u, kDnSplitAgeIdx = 7u;
const DnImg kDnImg[kDnImages] = {{VK_FORMAT_R16G16B16A16_SFLOAT, 2u, 3u, 4u}, {VK_FORMAT_R32G32B32A32_UINT, 1u, 1u, 1u},
                                 {VK_FORMAT_R32G32B32A32_UINT, 2u, 2u, 5u}, {VK_FORMAT_R32G32B32A32_UINT, 2u, 2u, 2u},
                                 {VK_FORMAT_R16G16B16A16_SFLOAT, 2u, 3u, 4u}, {VK_FORMAT_R32G32B32A32_UINT, 2u, 2u, 2u},
                                 {VK_FORMAT_R32G32B32A32_UINT, 2u, 2u, 2u}, {VK_FORMAT_R16G16B16A16_UINT, 2u, 2u, 2u}};
constexpr uint32_t kDnBinding[kDnImages] = {0u, 1u, 2u, 3u, 4u, 6u, 7u, 8u};   // (5: the motion vectors, sampled)
// Image i's format: AGE is r32ui on THE NARROW AGE instance, as clean_dn_common.glsl DN_AGE_NARROW declares it.
VkFormat dnFormat(uint32_t i, bool narrowAge)
{
    return (i == kDnAgeIdx && narrowAge) ? VK_FORMAT_R32_UINT : kDnImg[i].f;
}
// The layers of image e: the base, the sun channel's, the split channel's (each a superset of the
// base).
uint32_t dnLayers(const DnImg& e, bool sun, bool split)
{
    uint32_t n = sun ? e.sunLayers : e.layers;
    if (split) n = std::max(n, e.splitLayers);
    if (sun && split && e.splitSunLayers != 0u) n = e.splitSunLayers;
    return n;
}
// Image i's extent and layers: HIST lean (HIST_NO_COLOUR) and SUNHIST without the sun channel or lean (HIST_NO_SUN)
// are the 1 x 1 placeholder (clean_dn_common.glsl dnColourHist / dnSunHistImg); SPLITAGE without the split channel
// (every access of it is behind dnSplitOn).
bool dnPlaceholder(uint32_t i, bool sun, bool split, uint32_t hist)
{
    if (i == kDnHistIdx) return (hist & LLCleanDenoiserVK::HIST_NO_COLOUR) != 0u;
    if (i == kDnSunHistIdx) return !sun || (hist & LLCleanDenoiserVK::HIST_NO_SUN) != 0u;
    if (i == kDnSplitAgeIdx) return !split;
    return false;
}
// (#1608) every layer once per view (clean_dn_common.glsl DN_L: layer l of view v at l x views + v; MOT: a layer per
// view) -- the placeholders too (1 x 1: nothing to speak of; one shape for every image).
void dnShape(uint32_t i, uint32_t w, uint32_t h, bool sun, bool split, uint32_t hist, uint32_t views,
             uint32_t& iw, uint32_t& ih, uint32_t& layers)
{
    const bool ph = dnPlaceholder(i, sun, split, hist);
    iw = ph ? 1u : w; ih = ph ? 1u : h;
    layers = dnLayers(kDnImg[i], sun, split) * std::max(views, 1u);
}
constexpr uint32_t kDnMotIdx = 1u;
constexpr VkImageUsageFlags kDnUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
// Only split GEOM is copied: current RAW (2 + parity) -> scratch/final (4).
constexpr uint32_t kDnSplitGeomPar = 2u, kDnSplitGeomFinal = 4u;
constexpr VkImageUsageFlags dnUsage(uint32_t index, bool split)
{
    return kDnUsage | (index == 2u && split ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0u);
}
} // namespace

bool LLCleanDenoiserVK::init(VkDeviceContext* ctx, VkDescriptorSetLayout dslDraw0, VkSampler nearest, const char* name,
                             bool narrowAge, uint32_t views)
{
    if (!ctx || !dslDraw0 || !nearest) return false;
    mCtx = ctx;
    mName = name ? name : "denoiser";
    mNearest = nearest;
    mDslDraw0 = dslDraw0;
    mNarrowAge = narrowAge;
    mTag = narrowAge ? "gidn" : "pdn";   // (THE NARROW AGE is the GI instance's)
    mViews = std::clamp<uint32_t>(views, 1u, 4u);
    VkDevice dev = mCtx->device();
    // Set 1: 0 IN, 1 MOT, 2 GEOM, 3 HIST, 4 OUT (storage images), 5 the motion vectors (sampled), 6 AGE, 7 SUNHIST
    // (storage; THE LEAN HISTORY), 8 SPLITAGE (storage; the split slots' ages).
    VkDescriptorSetLayoutBinding b[kDnImages + 1u]{};
    for (uint32_t i = 0; i <= kDnImages; ++i)
    {
        b[i].binding = i; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        b[i].descriptorType = i == 5 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = kDnImages + 1u; li.pBindings = b;
    VK_CHECK(vkdCreateSetLayout(dev, &li, &mDsl, "LLCleanDenoiserVK::mDsl"));
    VK_CHECK(mPool.create(dev, "diffuse / specular denoiser", {{mDsl, 1}}));
    VK_CHECK(mPool.allocate(mDsl, &mDset));
    const VkDescriptorSetLayout sl[2] = {dslDraw0, mDsl};
    VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 2; pli.pSetLayouts = sl;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pc;
    VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mLayout, "LLCleanDenoiserVK::mLayout"));
    // Placeholders (hard: tiny) so the set -- and the callers' own bindings of these views --
    // are always valid.
    {
        VkMemScope memcat(VKMEM_DENOISE);
        if (!createSet(1u, 1u, false, false, false, HIST_FULL)) return false;
        // (#1608: a 2D_ARRAY view -- binding 5 / set 3 binding 28 are sampler2DArray, the pre-pass motion vectors a
        //  layer per view; one layer: it is never read)
        mMotionPh = mCtx->createImageLayers(1u, 1u, 1u, 1, VK_FORMAT_R16G16B16A16_SFLOAT,
                                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                            VK_IMAGE_ASPECT_COLOR_BIT, false, VK_IMAGE_VIEW_TYPE_2D_ARRAY);
        if (!mMotionPh.image) return false;
    }
    mW = mH = 1u;
    clearImages();
    writeSet();
    // The two passes: queued into the caller's pipeline batch when one is open. THE NARROW AGE instance takes the
    // DN_AGE_NARROW pair (AGE declared r32ui; the sun / split / mover-record paths compiled out).
    VkShaderModule tm = mNarrowAge ? dnModule(dev, kWorldDnTemporalGiSpv, kWorldDnTemporalGiSpv_bytes, "DnTemporalGi")
                                   : dnModule(dev, kWorldDnTemporalSpv, kWorldDnTemporalSpv_bytes, "DnTemporal");
    createComputePipelineInto(dev, tm, mLayout, 0u, dnViewSpec(mViews), &mTempPipe, "LLCleanDenoiserVK temporal");
    destroyShaderModuleDeferred(dev, tm, nullptr);
    VkShaderModule am = mNarrowAge ? dnModule(dev, kWorldDnAtrousGiSpv, kWorldDnAtrousGiSpv_bytes, "DnAtrousGi")
                                   : dnModule(dev, kWorldDnAtrousSpv, kWorldDnAtrousSpv_bytes, "DnAtrous");
    createComputePipelineInto(dev, am, mLayout, 0u, dnViewSpec(mViews), &mAtrousPipe, "LLCleanDenoiserVK a-trous");
    destroyShaderModuleDeferred(dev, am, nullptr);
    return true;
}

void LLCleanDenoiserVK::destroy()
{
    if (!mCtx) return;
    VkDevice dev = mCtx->device();
    vkDeviceWaitIdle(dev);
    noiseRelease();
    if (mNoisePipe) vkDestroyPipeline(dev, mNoisePipe, nullptr);
    mNoisePipe = VK_NULL_HANDLE;
    if (mNoiseLayout) vkDestroyPipelineLayout(dev, mNoiseLayout, nullptr);
    mNoiseLayout = VK_NULL_HANDLE;
    mNoisePool.destroy();
    mNoiseSet = VK_NULL_HANDLE;
    if (mNoiseDsl) vkdDestroySetLayout(dev, mNoiseDsl);
    mNoiseDsl = VK_NULL_HANDLE;
    destroyImages();
    if (mMotionPh.image) { mCtx->destroyImage(mMotionPh); mMotionPh = GpuImage{}; }
    if (mTempPipe) vkDestroyPipeline(dev, mTempPipe, nullptr);
    if (mAtrousPipe) vkDestroyPipeline(dev, mAtrousPipe, nullptr);
    mTempPipe = mAtrousPipe = VK_NULL_HANDLE;
    if (mLayout) vkDestroyPipelineLayout(dev, mLayout, nullptr);
    mLayout = VK_NULL_HANDLE;
    mPool.destroy();
    mDset = VK_NULL_HANDLE;
    if (mDsl) vkdDestroySetLayout(dev, mDsl);
    mDsl = VK_NULL_HANDLE;
    mCtx = nullptr;
}

uint64_t LLCleanDenoiserVK::bytesAt(uint32_t w, uint32_t h, bool sun, bool split, uint32_t hist) const
{
    if (!mCtx) return 0;
    uint64_t b = 0;
    for (uint32_t i = 0; i < kDnImages; ++i)
    {
        uint32_t iw, ih, layers;
        dnShape(i, w, h, sun, split, hist, mViews, iw, ih, layers);
        b += mCtx->imageMemoryBytes(iw, ih, layers, 1, dnFormat(i, mNarrowAge), dnUsage(i, split), false);
    }
    return b;
}

GpuImage* LLCleanDenoiserVK::image(uint32_t i)
{
    GpuImage* const im[kDnImages] = {&mIn, &mMot, &mGeom, &mHist, &mOut, &mAge, &mSunHist, &mSplitAge};
    return i < kDnImages ? im[i] : nullptr;
}

bool LLCleanDenoiserVK::createImage(uint32_t i, uint32_t w, uint32_t h, bool sun, bool split, uint32_t hist)
{
    static const char* const kNames[kDnImages] = {"In", "Mot", "Geom", "Hist", "Out", "Age", "SunHist", "SplitAge"};
    const VkFormat f = dnFormat(i, mNarrowAge);
    uint32_t iw, ih, layers;
    dnShape(i, w, h, sun, split, hist, mViews, iw, ih, layers);
    const VkImageUsageFlags usage = dnUsage(i, split);
    GpuImage* g = image(i);
    // (the shaders declare image2DArray for every binding: an array view; every image but MOT has 2+ layers.
    //  #1608: MOT is a screen image of a layer per view -- `view` its layer 0, for the single-view DLSS RR guides
    //  (motView2D), arrayView every layer (motView: the denoiser's binding 1, set 3's MOT); one view: the 2D image of
    //  before + a 1-layer 2D_ARRAY view, no memory)
    if (i == kDnMotIdx)
    {
        *g = mCtx->createScreenImage(iw, ih, layers, f, usage, VK_IMAGE_ASPECT_COLOR_BIT);
        if (g->image && !mCtx->addArrayView(*g, VK_IMAGE_ASPECT_COLOR_BIT)) { mCtx->destroyImage(*g); *g = GpuImage{}; }
    }
    else
        *g = mCtx->createImageLayers(iw, ih, layers, 1, f, usage, VK_IMAGE_ASPECT_COLOR_BIT, false,
                                     VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    if (!g->image) return false;
    char nm[24];
    std::snprintf(nm, sizeof(nm), "%s.%s", mTag, kNames[i]);
    mCtx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)g->image, nm);
    if (i == kDnMotIdx)
    {
        // (motSerial: process-wide, so a denoiser re-made from scratch never repeats an earlier instance's serial)
        static std::atomic<uint64_t> sMotSerial{0};
        mMotSerial = ++sMotSerial;
    }
    return true;
}

bool LLCleanDenoiserVK::createSet(uint32_t w, uint32_t h, bool soft, bool sun, bool split, uint32_t hist)
{
    VkDeviceContext::SoftAllocScope scope(mCtx, soft);
    for (uint32_t i = 0; i < kDnImages; ++i)
        if (!createImage(i, w, h, sun, split, hist)) return false;
    return true;
}

void LLCleanDenoiserVK::destroyImages()
{
    for (uint32_t i = 0; i < kDnImages; ++i)
    {
        GpuImage* im = image(i);
        if (im->image) { mCtx->destroyImage(*im); *im = GpuImage{}; }
    }
}

// The images -> GENERAL, zeroed (GEOM depth 0 = no surface: a fresh history validates nothing).
void LLCleanDenoiserVK::clearImage(GpuImage* const* im, uint32_t count)
{
    mCtx->submitImmediate([&](VkCommandBuffer c) {
        VkImageMemoryBarrier2 b[kDnImages + 1u]{};
        uint32_t n = 0;
        for (uint32_t k = 0; k < count && n < kDnImages + 1u; ++k)
        {
            const GpuImage* g = im[k];
            if (!g || !g->image) continue;
            VkImageMemoryBarrier2& x = b[n++];
            x.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            x.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            x.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT; x.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            x.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; x.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            x.image = g->image;
            x.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
        }
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.imageMemoryBarrierCount = n; d.pImageMemoryBarriers = b;
        vkCmdPipelineBarrier2(c, &d);
        const VkClearColorValue zero{};
        const VkImageSubresourceRange rr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
        for (uint32_t i = 0; i < n; ++i) vkCmdClearColorImage(c, b[i].image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rr);
        for (uint32_t i = 0; i < n; ++i)
        {
            b[i].srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT; b[i].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b[i].dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b[i].dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            b[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
        vkCmdPipelineBarrier2(c, &d);
    });
}

void LLCleanDenoiserVK::clearImages()
{
    GpuImage* const im[kDnImages + 1u] = {&mIn, &mMot, &mGeom, &mHist, &mOut, &mAge, &mSunHist, &mSplitAge, &mMotionPh};
    clearImage(im, kDnImages + 1u);
}

void LLCleanDenoiserVK::writeSet()
{
    if (!mDset) return;
    VkDescriptorImageInfo ii[kDnImages + 1u]{};
    VkWriteDescriptorSet w[kDnImages + 1u]{};
    for (uint32_t i = 0; i <= kDnImages; ++i)
    {
        // (the storage images at their bindings, then binding 5: the motion vectors)
        const bool motion = i == kDnImages;
        if (!motion) ii[i] = {VK_NULL_HANDLE, i == kDnMotIdx ? image(i)->arrayView : image(i)->view, VK_IMAGE_LAYOUT_GENERAL};
        else if (mMotionView) ii[i] = {mMotionSampler ? mMotionSampler : mNearest, mMotionView, mMotionLayout};
        else ii[i] = {mNearest, mMotionPh.view, VK_IMAGE_LAYOUT_GENERAL};
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = mDset;
        w[i].dstBinding = motion ? 5u : kDnBinding[i];
        w[i].descriptorCount = 1; w[i].pImageInfo = &ii[i];
        w[i].descriptorType = motion ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    vkUpdateDescriptorSets(mCtx->device(), kDnImages + 1u, w, 0, nullptr);
}

void LLCleanDenoiserVK::setMotion(VkImageView view, VkSampler sampler, VkImageLayout layout)
{
    mMotionView = view;
    mMotionSampler = sampler;
    mMotionLayout = layout;
    writeSet();
}

bool LLCleanDenoiserVK::ensure(uint32_t w, uint32_t h, bool sun, bool split, uint32_t hist)
{
    if (!mCtx || !mDset) return false;
    w = std::max(w, 1u); h = std::max(h, 1u);
    if (mNarrowAge && (sun || split))
    {
        // THE NARROW AGE: its AGE has no sun age / countdown / record words and its passes compile those channels out
        LL_WARNS_ONCE("Vulkan") << mName << ": the narrow-age denoiser has no sun / split channel; allocated without"
                                << LL_ENDL;
        sun = split = false;
    }
    hist &= HIST_NONE;
    if (!sun) hist &= ~(uint32_t)HIST_NO_SUN;   // (no sun history to leave out: SUNHIST is the placeholder anyway)
    mViewsKept = false;
    if (allocated() && w == mW && h == mH && sun == mSun && split == mSplit && hist == mHistMode) return true;
    vkDeviceWaitIdle(mCtx->device());   // the set and the callers' bindings may be in flight
    ++mGen;
    // (THE LEAN HISTORY) only HIST / SUNHIST change: re-create just them -- the final image, GEOM, MOT and AGE stay
    // (the consumers keep their views; the one-frame-latency GI keeps last frame's image). The re-created history
    // restarts (its flags off for the next frame); a refused growth keeps it out (the frame runs without it).
    if (allocated() && w == mW && h == mH && sun == mSun && split == mSplit)
    {
        bool ok = true;
        GpuImage* changed[2] = {};
        uint32_t nc = 0;
        for (uint32_t i : {kDnHistIdx, kDnSunHistIdx})
        {
            const uint32_t bit = i == kDnHistIdx ? (uint32_t)HIST_NO_COLOUR : (uint32_t)HIST_NO_SUN;
            if (((hist ^ mHistMode) & bit) == 0u) continue;
            GpuImage* g = image(i);
            if (g->image) { mCtx->destroyImage(*g); *g = GpuImage{}; }
            bool made;
            {
                VkMemScope memcat(VKMEM_DENOISE);
                VkDeviceContext::SoftAllocScope scope(mCtx, true);
                made = createImage(i, w, h, sun, split, hist);
            }
            if (!made)
            {
                // #697: the growth refused -- that history stays out (the placeholder; no history read / written)
                ok = false;
                hist |= bit;
                VkMemScope memcat(VKMEM_DENOISE);
                createImage(i, w, h, sun, split, hist);
                LL_WARNS("Vulkan") << mName << ": its " << (i == kDnHistIdx ? "colour" : "sun / moon")
                                   << " history (" << w << "x" << h << ") was refused (VRAM); without it" << LL_ENDL;
            }
            if (i == kDnHistIdx) mHistOk = mRan = false;
            else mSunHistOk = mSunRan = false;
            changed[nc++] = g;
        }
        mHistMode = hist;
        if (nc) clearImage(changed, nc);
        writeSet();
        mViewsKept = true;
        LL_INFOS("Vulkan") << mName << ": history " << ((hist & HIST_NO_COLOUR) ? "without colour" : "with colour")
                           << (mSun ? ((hist & HIST_NO_SUN) ? ", without the sun's" : ", with the sun's") : "")
                           << ((hist != HIST_FULL) ? " (lean: DLSS RR takes the signal raw)" : "") << ", "
                           << (bytes() >> 20) << " MB (" << bytes() / ((uint64_t)w * h) << " B per texel)" << LL_ENDL;
        return ok;
    }
    destroyImages();
    bool ok;
    {
        VkMemScope memcat(VKMEM_DENOISE);
        ok = createSet(w, h, true, sun, split, hist);
        if (!ok)
        {
            // #697: never a crash -- the placeholders back, the caller keeps the exact path.
            destroyImages();
            LL_WARNS("Vulkan") << mName << ": " << w << "x" << h << (sun ? " (+ sun / moon channel)" : "")
                               << (split ? " (+ split channel)" : "")
                               << " refused (VRAM, ~" << (bytesAt(w, h, sun, split, hist) >> 20) << " MB); placeholders" << LL_ENDL;
            createSet(1u, 1u, false, false, false, HIST_FULL);
            w = h = 1u;
            sun = false;
            split = false;
            hist = HIST_FULL;
        }
    }
    mW = w; mH = h;
    mSun = sun;
    mSplit = split;
    mHistMode = hist;
    mHistOk = mRan = false;
    mSunHistOk = mSunRan = false;
    clearImages();
    writeSet();
    if (ok)
        LL_INFOS("Vulkan") << mName << ": " << w << "x" << h << (mSun ? " + sun / moon channel" : "")
                           << (mSplit ? " + split channel" : "")
                           << ((hist & HIST_NO_COLOUR) ? " (lean: no colour history -- DLSS RR takes the signal raw)" : "")
                           << ((mSun && (hist & HIST_NO_SUN)) ? " (lean: no sun history -- DLSS RR takes the sun raw)" : "")
                           << ", " << (bytes() >> 20) << " MB (" << bytes() / ((uint64_t)w * h) << " B per texel)" << LL_ENDL;
    return ok;
}

void LLCleanDenoiserVK::release()
{
    if (!mCtx || !allocated()) return;
    vkDeviceWaitIdle(mCtx->device());
    ++mGen;
    destroyImages();
    {
        VkMemScope memcat(VKMEM_DENOISE);
        createSet(1u, 1u, false, false, false, HIST_FULL);
    }
    mW = mH = 1u;
    mSun = false;
    mSplit = false;
    mHistMode = HIST_FULL;
    mHistOk = mRan = false;
    mSunHistOk = mSunRan = false;
    clearImages();
    writeSet();
    LL_INFOS("Vulkan") << mName << ": released" << LL_ENDL;
}

void LLCleanDenoiserVK::beginFrame()
{
    mHistOk = mRan && allocated();
    mRan = false;
    // The sun channel's history: the channel was accumulated last frame as well (a frame
    // whose producer skipped it -- merge off, the sun not traced -- leaves nothing to reproject).
    mSunHistOk = mSunRan && mHistOk && hasSun();
    mSunRan = false;
    mParity ^= 1u;
}

LLCleanDenoiserVK::Push LLCleanDenoiserVK::makePush(uint32_t rw, uint32_t rh, const Params& p) const
{
    Push q{};
    q.dims[0] = (float)mW; q.dims[1] = (float)mH; q.dims[2] = (float)rw; q.dims[3] = (float)rh;
    q.prm0[0] = std::clamp(p.maxHistDiffuse, 1.f, 63.f);
    q.prm0[1] = std::clamp(p.maxHistSpecular, 1.f, 63.f);
    q.prm0[2] = std::clamp(p.clampGamma, 0.25f, 16.f);
    q.prm0[3] = std::clamp(p.lumPhi, 0.1f, 64.f);
    q.prm1[0] = std::clamp(p.depthPhi, 0.05f, 16.f);
    q.prm1[1] = std::clamp(p.normalPow, 1.f, 512.f);
    q.prm1[2] = std::clamp(p.wCut, 0.f, 4.f);
    q.prm1[3] = std::clamp(p.minHistSpecular, 1.f, q.prm0[1]);
    // clean_dn_common.glsl DN_F_*: bit 0 history, bit 1 parity, bits 4-7 the debug view; the
    // sun / moon channel (only with the images carrying it): bit 8 on, bit 9 its history is
    // valid, bit 10 hard sun, bit 11 exact reprojection; bit 12 the no-information rule (DN_F_INFO);
    // bits 13-15 the ReSTIR-converged diffuse's a-trous iterations (DN_CONV_SHIFT); bits 16-19 the
    // moving-light history (DN_MOVE_SHIFT, 0 = off); bits 27-29 the disocclusion history fix
    // (DN_HFIX_SHIFT), bit 30 the reprojection search (DN_F_REPROJ_SEARCH); (#1038) bit 2 the young-texel
    // boost (DN_F_YOUNG_BOOST), bit 31 the moving-surface depth tolerance (DN_F_MOVE_ZTOL); bit 3 the
    // reservoir-reusing radiance signal (DN_F_RADIANCE, RenderVKReSTIRRadiance).
    const bool sun = p.sun && hasSun();
    q.ctl[0] = (mHistOk ? 1u : 0u) | (mParity ? 2u : 0u) | (p.youngBoost ? 4u : 0u) | (p.moveZTol ? (1u << 31) : 0u)
             | (p.radiance ? 8u : 0u)
             | ((std::min(p.debug, 15u)) << 4u)
             | (sun ? 256u : 0u) | (sun && mSunHistOk && !p.sunCut ? 512u : 0u) | (sun && p.sunHard ? 1024u : 0u)
             | (sun && p.sunExact ? 2048u : 0u) | (p.info ? 4096u : 0u)
             | (std::min(p.convIters, 7u) << 13u)    // (#869 / #85: DN_CONV_SHIFT)
             | (std::min(p.moveHist, 15u) << 16u)    // (#869: DN_MOVE_SHIFT)
             | (p.raw ? (1u << 20) : 0u) | (p.rawSun ? (1u << 21) : 0u)    // (#870: DN_F_RAW / DN_F_RAW_SUN)
             | ((uint32_t)std::clamp(p.surfMoveTexels * 4.f + 0.5f, 0.f, 15.f) << 22u)   // (#869: DN_SURF_MOVE_SHIFT)
             | (p.moveDiffuseOnly ? (1u << 26) : 0u)    // (DN_F_MOVE_DIFF_ONLY)
             | (std::min(p.histFix, 7u) << 27u)         // (#87: DN_HFIX_SHIFT)
             | (p.reprojSearch ? (1u << 30) : 0u);      // (#87: DN_F_REPROJ_SEARCH)
    // Temporal flags; recordSpatial replaces this word with its iteration before
    // every dispatch. Bits7..8 select the caster policy (3: 2 + the mover record); GI has no caster signal.
    q.ctl[1] = (p.shadowPlaneReconstruction ? 512u : 0u) | (std::min(p.casterHistoryPolicy, 3u) << 7u) | (p.radiance && p.radianceMoveReset ? 1u : 0u)
             | (p.localReceiverGuides ? 2u : 0u)
             | (p.radiance && p.localReceiverGuides && p.skinnedVarianceRecovery ? 4u : 0u)
             | (sun && p.sunNoResolve ? 8u : 0u) // temporal sun self-integration
             | (p.radiance && p.specularLobeGuard ? 32u : 0u)
             | (p.radiance && p.exactSpecular ? 64u : 0u)
             | (p.giChangedCut && !p.radiance ? 1024u : 0u)    // (dnGiChangedCut)
             | (p.giStableSamples && !p.radiance ? 2048u : 0u)  // (DN_F_GI_STABLE)
             | (p.radiance && p.moverExact ? 4096u : 0u)        // (DN_T_MOVER_EXACT, RenderVKShadowMoverMask 2)
             // (radiance, THE CONVERGENCE RULE) THE MOVER COUNTDOWN's length (bits 14-17) and cap (18-21)
             | (p.radiance ? (std::min(p.moverCdLen, 15u) << 14u) | (std::min(p.moverCdCap, 15u) << 18u) : 0u)
             // (radiance, RenderVKProjStochasticAdaptiveHistory: THE ADAPTIVE MOVER HISTORY, bits 24-26 -- DN_T_ADAPT_SHIFT)
             | (p.radiance ? (std::min(p.adaptPasses, 7u) << 24u) : 0u)
             // (radiance, RenderVKShadowFixMoverVariance: THE COUNTDOWN'S NOISE, bits 22-23 -- DN_T_MOVER_VAR_SHIFT)
             | (p.radiance ? (std::min(p.moverVarFix, 2u) << 22u) : 0u);
    q.ctl[2] = clampIters(p.atrous);
    q.ctl[3] = p.frame;
    q.sun0[0] = std::clamp(p.sunMaxHist, 1.f, 64.f);
    q.sun0[1] = std::clamp(p.sunDynHist, 1.f, 15.f);
    q.sun0[2] = std::clamp(p.sunDynMax, 0.f, 15.f);
    q.sun0[3] = std::clamp(p.sunMove, 0.f, 64.f);
    q.sun1[0] = std::clamp(p.sunSpatialCap, 0.f, 16.f);
    q.sun1[1] = std::clamp(p.sunClamp, 0.5f, 8.f);
    q.sun1[2] = (float)std::min(p.sunPersist, 7u);
    q.sun1[3] = p.sunRays > 0.f ? std::min(p.sunRays, 64.f) : 0.f;   // (0 = the frame info's sun_rt.y)
    return q;
}

static void dnComputeBarrier(VkCommandBuffer cmd)
{
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &d);
}

static void dnSplitRawSnapshot(VkCommandBuffer cmd, VkImage geom, uint32_t w, uint32_t h, uint32_t par, uint32_t views)
{
    // (#1608) every view's layer of each: layer l of view v at l x views + v, the views of one layer contiguous
    // Layer 4's old FINAL consumers precede this recording (queue order, or the
    // lighting fork's ALL_COMMANDS semaphore). No current-frame reader needs FINAL
    // until after recordSpatial's last pass. Snapshot RAW before temporal overwrites
    // 2 + parity with HISTORY; the two copy regions never overlap.
    VkImageMemoryBarrier2 b[2]{};
    for (uint32_t i = 0; i < 2u; ++i)
    {
        b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b[i].oldLayout = b[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b[i].srcQueueFamilyIndex = b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[i].image = geom;
        b[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, (i == 0u ? kDnSplitGeomPar + par : kDnSplitGeomFinal) * views,
                                 views};
        b[i].dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    }
    b[0].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    b[0].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    b[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    // ALL_COMMANDS includes prior CS/FS readers on graphics, and only supported
    // stages on a compute-only queue. Cross-queue readers are ordered by the fork.
    b[1].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b[1].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                       | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 2u; d.pImageMemoryBarriers = b;
    vkCmdPipelineBarrier2(cmd, &d);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, (kDnSplitGeomPar + par) * views, views};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kDnSplitGeomFinal * views, views};
    region.extent = {w, h, 1};
    vkCmdCopyImage(cmd, geom, VK_IMAGE_LAYOUT_GENERAL, geom, VK_IMAGE_LAYOUT_GENERAL, 1u, &region);
    // Copy reads must finish before temporal writes current HISTORY. Snapshot
    // writes must be visible to temporal's centre and neighbouring RAW reads.
    b[0].srcStageMask = b[1].srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b[0].srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    b[0].dstStageMask = b[1].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    b[0].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    b[1].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b[1].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &d);
    // recordSpatial's compute barriers also order these temporal snapshot reads
    // before its last iteration overwrites layer 4 with FINAL again.
}

void LLCleanDenoiserVK::recordTemporal(VkCommandBuffer cmd, VkDescriptorSet dsetDraw0, uint32_t rw, uint32_t rh,
                                       const Params& p, LLCleanRendererVK* timerRenderer, const char* timerName)
{
    if (!pipelinesReady() || !dsetDraw0) return;
    if (p.sun && hasSun())
    {
        // Do not blend a previous exact/linear reprojection policy after a live RR-history change.
        if (!mSunNoResolveKnown || mSunNoResolve != p.sunNoResolve) mSunHistOk = false;
        mSunNoResolve = p.sunNoResolve;
        mSunNoResolveKnown = true;
    }
    dnComputeBarrier(cmd);   // the producer's IN / MOT / GEOM writes
    if (mSplit) dnSplitRawSnapshot(cmd, mGeom.image, mW, mH, mParity, mViews);
    const VkDescriptorSet sets[2] = {dsetDraw0, mDset};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout, 0, 2, sets, 0, nullptr);
    if (mSampleGridKnown && (mSampleGridHistory != p.sampleGridHistory
        || mShadowPlaneReconstruction != p.shadowPlaneReconstruction
        || mSpecularLobeGuard != p.specularLobeGuard || mExactSpecular != p.exactSpecular
        || mCasterHistoryPolicy != p.casterHistoryPolicy || mCasterCauseDebug != (p.debug == 11u)))
        mHistOk = mSunHistOk = false;
    mShadowPlaneReconstruction = p.shadowPlaneReconstruction;
    mCasterHistoryPolicy = p.casterHistoryPolicy;
    mCasterCauseDebug = p.debug == 11u;
    mSampleGridHistory = p.sampleGridHistory;
    mSampleGridKnown = true;
    mSpecularLobeGuard = p.specularLobeGuard;
    mExactSpecular = p.exactSpecular;
    Push q = makePush(rw, rh, p);
    if (p.sampleGridHistory)
    {
        // Full precision matters: quantizing jitter can choose a different integer
        // sample at odd extents. ctl.zw are unused by the temporal shader otherwise.
        for (uint32_t axis = 0u; axis < 2u; ++axis)
        {
            const float phase = std::isfinite(mPreviousSampleJitter[axis])
                ? std::clamp(mPreviousSampleJitter[axis], -0.5f, 0.5f) : 0.f;
            std::memcpy(&q.ctl[2u + axis], &phase, sizeof(phase));
        }
        q.ctl[1] |= 16u;
    }
    VK_PUSH_BUDGET_CHECK(q);
    vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(q), &q);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mTempPipe);
    {
        std::optional<LLCleanRendererVK::GpuPassScope> scope;
        if (timerRenderer && timerName) scope.emplace(*timerRenderer, cmd, timerName);
        vkCmdDispatch(cmd, (mW + 7u) / 8u, (mH + 7u) / 8u, mViews);   // (#1608: z = the views)
    }
    mPreviousSampleJitter[0] = p.rasterJitter[0];
    mPreviousSampleJitter[1] = p.rasterJitter[1];
    mRan = true;
    mSunRan = p.sun && hasSun();
}

void LLCleanDenoiserVK::recordSpatial(VkCommandBuffer cmd, uint32_t rw, uint32_t rh, const Params& p, LLCleanRendererVK* timerRenderer, const char* timerName)
{
    if (!pipelinesReady() || !mRan) return;
    // (the sets are still bound from recordTemporal: same layout, nothing bound in between)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mAtrousPipe);
    Push q = makePush(rw, rh, p);
    const uint32_t n = clampIters(p.atrous);
    for (uint32_t k = 0; k < n; ++k)
    {
        dnComputeBarrier(cmd);   // the previous pass's writes -> this iteration's neighbourhood reads
        q.ctl[1] = k | (p.shadowPlaneReconstruction ? 512u : 0u) | (p.radiance && p.specularLobeGuard ? 32u : 0u) | (p.radiance && p.exactSpecular ? 64u : 0u)
                 | (p.radiance && p.moverExact ? 4096u : 0u);   // (DN_T_MOVER_EXACT: an exact centre takes exact taps only)
        vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(q), &q);
        {
            std::optional<LLCleanRendererVK::GpuPassScope> scope;
            if (timerRenderer && timerName) scope.emplace(*timerRenderer, cmd, timerName);
            vkCmdDispatch(cmd, (mW + 7u) / 8u, (mH + 7u) / 8u, mViews);   // (#1608: z = the views)
        }
    }
}

// ---- NOISE PROBE (RenderVKNoiseStats; clean_dn_noise.comp) ----
namespace
{
struct DnNoisePush { uint32_t ctl[4]; };
constexpr VkDeviceSize kDnNoiseAccBytes = 64u * sizeof(uint32_t);
}

void LLCleanDenoiserVK::noiseRelease()
{
    if (!mCtx) return;
    if (mNoiseBuf.buffer || mNoiseRb.buffer) vkDeviceWaitIdle(mCtx->device());   // (a probe may be in flight)
    if (mNoiseBuf.buffer) mCtx->destroyBuffer(mNoiseBuf);
    if (mNoiseRb.buffer) mCtx->destroyBuffer(mNoiseRb);
    mNoiseBuf = GpuBuffer{};
    mNoiseRb = GpuBuffer{};
    mNoiseW = mNoiseH = 0;
    mNoiseRbUsed[0] = mNoiseRbUsed[1] = mNoiseRbUsed[2] = false;
    mNoiseLast[0] = mNoiseLast[1] = 0;
    mNoiseBegun = false;
}

void LLCleanDenoiserVK::noiseBegin(VkCommandBuffer cmd)
{
    mNoiseBegun = false;
    // (#1608: a measurement of ONE view -- clean_dn_noise.comp reads the layers of before; not in a layered stereo
    //  session, named in the stereo-off line)
    if (!mCtx || !allocated() || mViews > 1u) return;
    VkDevice dev = mCtx->device();
    if (!mNoisePipe)
    {
        // (lazily: the probe's set layout, pool, layout and pipeline -- created once, kept until destroy())
        if (!mNoiseDsl)
        {
            VkDescriptorSetLayoutBinding b{};
            b.binding = 0; b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            li.bindingCount = 1; li.pBindings = &b;
            if (vkdCreateSetLayout(dev, &li, &mNoiseDsl, "LLCleanDenoiserVK::mNoiseDsl") != VK_SUCCESS) return;
            if (mNoisePool.create(dev, "denoiser noise probe", {{mNoiseDsl, 1}}) != VK_SUCCESS) return;
            if (mNoisePool.allocate(mNoiseDsl, &mNoiseSet) != VK_SUCCESS) { mNoiseSet = VK_NULL_HANDLE; return; }
        }
        if (!mNoiseSet) return;
        if (!mNoiseLayout)
        {
            const VkDescriptorSetLayout sl[3] = {mDslDraw0, mDsl, mNoiseDsl};
            VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DnNoisePush)};
            VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pli.setLayoutCount = 3; pli.pSetLayouts = sl;
            pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pc;
            if (vkdCreatePipelineLayout(dev, &pli, &mNoiseLayout, "LLCleanDenoiserVK::mNoiseLayout") != VK_SUCCESS)
            {
                mNoiseLayout = VK_NULL_HANDLE;
                return;
            }
        }
        VkShaderModule nm = dnModule(dev, kWorldDnNoiseSpv, kWorldDnNoiseSpv_bytes, "DnNoise");
        mNoisePipe = createComputePipeline(dev, nm, mNoiseLayout, 0u, nullptr, "LLCleanDenoiserVK noise probe");
        vkDestroyShaderModule(dev, nm, nullptr);
        if (!mNoisePipe) return;
    }
    // (the extent changed: new prev[] -- last frame's values belong to another grid)
    if (mNoiseBuf.buffer && (mNoiseW != mW || mNoiseH != mH)) noiseRelease();
    if (!mNoiseBuf.buffer)
    {
        VkDeviceContext::SoftAllocScope soft(mCtx);
        VkMemScope memcat(VKMEM_DENOISE);
        mNoiseBuf = mCtx->createBuffer(kDnNoiseAccBytes + 2ull * mW * mH * sizeof(float),
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                           | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!mNoiseBuf.buffer) return;   // (refused: no probe this frame, retried the next)
        mNoiseW = mW; mNoiseH = mH;
        mNoiseLast[0] = mNoiseLast[1] = 0;
        VkDescriptorBufferInfo bi{mNoiseBuf.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = mNoiseSet; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi;
        vkDeviceWaitIdle(dev);   // (the set may be bound by a frame in flight)
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    }
    if (!mNoiseRb.buffer)
    {
        VkDeviceContext::SoftAllocScope soft(mCtx);
        VkMemScope memcat(VKMEM_READBACK);
        mNoiseRb = mCtx->createBuffer(3u * kDnNoiseAccBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!mNoiseRb.buffer || !mNoiseRb.mapped) return;
    }
    // Harvest the ring slot this frame will write (3 frames old: its copy has long completed).
    const uint32_t s = mNoiseRbSlot;
    if (mNoiseRbUsed[s])
    {
        const uint32_t* a = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mNoiseRb.mapped)
                                                              + (size_t)s * kDnNoiseAccBytes);
        for (uint32_t k = 0; k < kNoiseAcc; ++k) mNoiseSum[k] += a[k];
        ++mNoiseFrames;
        for (uint32_t sl = 0; sl < 2u; ++sl)   // (the flip floor's mean: this frame's surface texels, L x 16)
            if (a[sl * 24u] > 0u) mNoiseMeanL[sl] = (float)(a[sl * 24u + 2u] / 16.0 / a[sl * 24u]);
        mNoiseRbUsed[s] = false;
    }
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT;
    m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT; m.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &d);
    vkCmdFillBuffer(cmd, mNoiseBuf.buffer, 0, kDnNoiseAccBytes, 0u);
    m.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT; m.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    vkCmdPipelineBarrier2(cmd, &d);
    ++mNoiseFrame;
    mNoiseBegun = true;
}

void LLCleanDenoiserVK::recordNoise(VkCommandBuffer cmd, uint32_t slot, bool finalImage, uint32_t atrous,
                                    uint32_t layerMask, bool still, LLCleanRendererVK* timerRenderer, const char* timerName)
{
    if (!mNoiseBegun || !mNoisePipe || !mNoiseBuf.buffer || slot > 1u) return;
    if (finalImage && !mRan) return;   // (no denoiser pass this frame: no final image)
    dnComputeBarrier(cmd);   // the producer's (raw) or the last a-trous iteration's (final) writes
    // (a still camera for kNoiseSettle frames: the history the denoiser shows has settled after the cut)
    if (slot == 0u || mNoiseLast[0] != mNoiseFrame) mNoiseStillRun = still ? mNoiseStillRun + 1u : 0u;
    const bool prevOk = still && mNoiseStillRun > kNoiseSettle && mNoiseLast[slot] != 0
                     && mNoiseLast[slot] + 1u == mNoiseFrame;
    if (!still) ++mNoiseMoving;
    DnNoisePush q{};
    q.ctl[0] = (mW & 0xFFFFu) | (mH << 16u);
    q.ctl[1] = (finalImage && !finalIsIn(atrous) ? 1u : 0u) | ((mParity & 1u) << 1u) | (prevOk ? 4u : 0u)
             | ((layerMask & 15u) << 8u);
    q.ctl[2] = slot;
    const float flipFloor = 0.05f * mNoiseMeanL[slot];
    std::memcpy(&q.ctl[3], &flipFloor, sizeof(flipFloor));
    const VkDescriptorSet sets[2] = {mDset, mNoiseSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mNoiseLayout, 1, 2, sets, 0, nullptr);
    vkCmdPushConstants(cmd, mNoiseLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(q), &q);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mNoisePipe);
    {
        std::optional<LLCleanRendererVK::GpuPassScope> scope;
        if (timerRenderer && timerName) scope.emplace(*timerRenderer, cmd, timerName);
        vkCmdDispatch(cmd, (mW + 7u) / 8u, (mH + 7u) / 8u, 1);
    }
    dnComputeBarrier(cmd);   // (the probe only reads the images; its buffer writes before the next probe / copy)
    mNoiseLast[slot] = mNoiseFrame;
}

void LLCleanDenoiserVK::noiseEnd(VkCommandBuffer cmd)
{
    if (!mNoiseBegun || !mNoiseRb.buffer) return;
    mNoiseBegun = false;
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; m.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &d);
    const VkBufferCopy c{0, (VkDeviceSize)mNoiseRbSlot * kDnNoiseAccBytes, kDnNoiseAccBytes};
    vkCmdCopyBuffer(cmd, mNoiseBuf.buffer, mNoiseRb.buffer, 1, &c);
    mNoiseRbUsed[mNoiseRbSlot] = true;
    mNoiseRbSlot = (mNoiseRbSlot + 1u) % 3u;
}

std::string LLCleanDenoiserVK::noiseText()
{
    if (!mNoiseFrames)
    {
        mNoiseMoving = 0;
        return std::string();
    }
    static const char* const kGrp[6] = {"all", "near<4m", "mid", "far>=16m", "edge", "interior"};
    static const char* const kSlot[2] = {"raw", "final"};
    char buf[160];
    std::string o = std::string(mName) + " over " + std::to_string(mNoiseFrames) + " frames ("
                  + std::to_string(mNoiseMoving) + " probes with a moving camera; the first "
                  + std::to_string(kNoiseSettle) + " still frames after a move not compared):";
    for (uint32_t s = 0; s < 2u; ++s)
    {
        const double* q0 = mNoiseSum + s * 24u;
        if (q0[0] <= 0.0) continue;
        o += std::string(" ") + kSlot[s] + " [";
        for (uint32_t g = 0; g < 6u; ++g)
        {
            const double* q = q0 + g * 4u;
            std::snprintf(buf, sizeof(buf), "%s%s %.4f flip %.4f (%.0f)", g ? ", " : "", kGrp[g],
                          q[2] > 0.0 ? q[1] / q[2] : 0.0, q[0] > 0.0 ? q[3] / q[0] : 0.0, q[0] / mNoiseFrames);
            o += buf;
        }
        o += "]";
    }
    for (double& v : mNoiseSum) v = 0.0;
    mNoiseFrames = 0;
    mNoiseMoving = 0;
    return o;
}
