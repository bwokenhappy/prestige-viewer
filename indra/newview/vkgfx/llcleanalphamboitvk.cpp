/**
 * @file llcleanalphamboitvk.cpp
 * @brief Moment-based order-independent transparency (RenderVKAlphaMBOIT): see llcleanalphamboitvk.h,
 *        clean_mboit.glsl, clean_mb_cutoff.frag and clean_mb_resolve.frag.
 */
#include "linden_common.h"

#include "llcleanalphamboitvk.h"
#include "llcleanpipeneeds.h"
#include "vk_world_shaders_spv.h"

#include "llerror.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace
{
double mbClock()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

VkShaderModule mbModule(VkDevice dev, const uint32_t* code, size_t bytes, const char* name)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
    vkNoteShaderModule(m, code, bytes, name);
    return m;
}

void mbMemBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src, VkAccessFlags2 srcA, VkPipelineStageFlags2 dst, VkAccessFlags2 dstA)
{
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = src; mb.srcAccessMask = srcA; mb.dstStageMask = dst; mb.dstAccessMask = dstA;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &d);
}

constexpr VkPipelineStageFlags2 kDepthStages = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
} // namespace

namespace
{
const char* const kPipeName[LLCleanAlphaMBOITVK::P_COUNT] = {"MBOIT moments", "MBOIT shade full", "MBOIT shade full (rayquery)",
                                                              "MBOIT shade cheap", "MBOIT cutoff", "MBOIT resolve"};
}

bool LLCleanAlphaMBOITVK::pipelinesPending() const
{
    for (const PipelineJobSlot& j : mSlot) if (j.pending()) return true;
    return false;
}

void LLCleanAlphaMBOITVK::cancelJobs()
{
    for (PipelineJobSlot& j : mSlot) if (j.job) pipelineJobCancel(j.job);
}

bool LLCleanAlphaMBOITVK::ensurePipelines(const Build& b)
{
    if (!b.ctx || !b.statDesc[0].layout) return false;
    VkDevice dev = b.ctx->device();
    // NEVER BLOCKS: each pipeline is its own background job; a finished one only logs (poll never waits).
    for (uint32_t i = 0; i < P_COUNT; ++i)
        if (mSlot[i].poll())
            LL_INFOS("VKAlphaMBOIT") << kPipeName[i] << ": " << (mStage[i] ? "compiled" : "FAILED") << " in "
                                     << (S32)((mbClock() - mSince[i]) * 1000.0) << " ms wall (background)" << LL_ENDL;
    if (mInRound && !pipelinesPending())
    {
        // The round is done: install it whole (the frame keeps the sorted path -- or the previous set -- until
        // then). The ray-query twin is optional (the plain full tier shades without it).
        mInRound = false;
        const bool ok = mStage[P_MOMENTS] && mStage[P_SHADE_FULL] && (mStage[P_SHADE_CHEAP] || !mStageTiers)
                     && mStage[P_CUTOFF] && mStage[P_RESOLVE];
        if (ok)
        {
            if (mReady) destroyPipelines(dev);   // (a key / mode change: the old set may be in flight -- one device wait)
            std::memcpy(mLive, mStage, sizeof(mLive));
            std::memset(mStage, 0, sizeof(mStage));
            mReady = true;
            mBuiltMode = mStageMode;
            mBuiltRt = mStageRt;
            mBuiltTiers = mStageTiers;
        }
        else
        {
            destroyStage(dev);
            mRetryBuildAt = mbClock() + 5.0;
        }
        LL_INFOS("VKAlphaMBOIT") << "MBOIT pipelines " << (ok ? "ready" : (mReady ? "FAILED (the previous set stays)" : "FAILED (the sorted path stays; retry in 5 s)"))
                                 << " after " << (S32)((mbClock() - mRoundSince) * 1000.0) << " ms wall"
                                 << (ok && mStageRt && !mLive[P_SHADE_FULL_RT] ? "; the ray-query full tier failed (the plain one shades)" : "") << LL_ENDL;
    }
    const bool modeReady = mReady && mBuiltMode == b.mode && mBuiltTiers == b.tiers;
    if (pipelinesPending() || mbClock() < mRetryBuildAt) return modeReady;
    const bool fmtChanged = mBuiltFormats[0] != b.hdrFormat || mBuiltFormats[1] != b.glowFormat
                         || mBuiltFormats[2] != b.reactFormat || mBuiltFormats[3] != b.depthFormat;
    const bool sameSel = mWantMode == b.mode && mWantRt == b.rt && mWantTiers == b.tiers;
    if (mBuiltKey == b.specKey && !fmtChanged && sameSel) return modeReady;
    if (mReady && !fmtChanged && b.generic && sameSel) { mBuiltKey = b.specKey; return modeReady; }
    if (fmtChanged && mReady) destroyPipelines(dev);
    if (fmtChanged && mDepth.image)
    {   // (the cutoff depth takes the scene depth's format)
        vkDeviceWaitIdle(dev);
        freeTargets();
    }
    mCtx = b.ctx;
    bool setDirty = false;
    if (!mDsl)
    {
        setDirty = true;
        VkDescriptorSetLayoutBinding sb[6]{};
        for (uint32_t i = 0; i < 6; ++i)
        {
            sb[i].binding = i;
            sb[i].descriptorType = i < 5 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            sb[i].descriptorCount = 1;
            sb[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 6; lci.pBindings = sb;
        VK_CHECK(vkdCreateSetLayout(dev, &lci, &mDsl, "mDslAlphaMBOIT"));
        VK_CHECK(mPool.create(dev, "alphamboit", {{mDsl, 1}}));
        VKD_ALLOC_SOFT(mPool.allocate(mDsl, &mDset), { destroy(dev); return false; });
    }
    if (!mParams.buffer)
    {
        if (!setDirty) vkDeviceWaitIdle(dev);
        setDirty = true;
        mParams = mCtx->createBuffer(sizeof(Params), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!mParams.buffer) { mRetryBuildAt = mbClock() + 5.0; return false; }
    }
    if (!mLayout)
    {
        const VkDescriptorSetLayout sets[3] = {b.dslDraw0, b.dslDraw1, mDsl};
        VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, b.pushBytes};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 3; pli.pSetLayouts = sets;
        pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
        VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mLayout, "mAlphaMBOITLayout"));
        if (b.dslRt)
        {
            const VkDescriptorSetLayout setsRt[4] = {b.dslDraw0, b.dslDraw1, b.dslRt, mDsl};
            pli.setLayoutCount = 4; pli.pSetLayouts = setsRt;
            VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mLayoutRt, "mAlphaMBOITLayoutRt"));
        }
    }
    if (setDirty) writeSet();

    // ---- a new round: one job per pipeline ----
    const uint32_t bm = b.mode == 2u ? 2u : 1u;
    const bool rt = b.rt && mLayoutRt && b.statDesc[1].layout;
    auto job = [&](uint32_t i, const auto& fill)
    {
        mSince[i] = mbClock();
        {
            PipelineBatchScope batchScope(dev);   // (modules are made per job: each batch destroys its own)
            fill();
            PipelineJobOpts o;
            o.prio = VKPIPE_PRIO_USER;
            o.gate = false;
            o.what = kPipeName[i];
            o.family = i == P_SHADE_FULL_RT ? llpipeneeds::FAM_MBOIT_RT : llpipeneeds::FAM_MBOIT;
            mSlot[i].start(pipelineBatchFlushAsync(dev, o));
        }
        LL_INFOS("VKAlphaMBOIT") << kPipeName[i] << ": " << (mSlot[i].pending() ? "compile started (background)" : "refused / nothing queued")
                                 << LL_ENDL;
    };
    auto shadeDesc = [&](int v, VkShaderModule vs, VkShaderModule fs)
    {
        // Pass 3: the lit FS behind the cutoff depth (reversed-Z GEQUAL: the compare in conventional terms,
        // flipped by reverseZ; write off), the colour / glow sums additive + the multiplicative revealage.
        GraphicsPipelineDesc d = b.statDesc[v];
        d.vert = vs; d.frag = fs; d.layout = v ? mLayoutRt : mLayout; d.fragSpec = b.spec;
        d.views = 1; d.depthFormat = b.depthFormat; d.depthTest = true; d.depthWrite = false;
        d.depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL; d.reverseZ = true;
        d.relaxedDepthCompare = VK_COMPARE_OP_NEVER;   // (no #1088 relaxed substitute: this compare is the cutoff)
        d.cullMode = VK_CULL_MODE_NONE; d.blend = false; d.reactFormat = VK_FORMAT_UNDEFINED; d.reactWriteMask = 0;
        d.dynamicShadingRate = false; d.fsrAttachment = false;
        d.mrtCount = 3; d.mrtFormats[0] = kSumFormat; d.mrtFormats[1] = kSumFormat; d.mrtFormats[2] = kRevealFormat;
        d.oitMrt = true;
        return d;
    };
    auto mod = [&](const uint32_t* code, size_t bytes, const char* name) { return mbModule(dev, code, bytes, name); };
    auto drop = [&](VkShaderModule m) { if (m) destroyShaderModuleDeferred(dev, m, nullptr); };
    mInRound = true;
    mRoundSince = mbClock();
    mStageMode = bm;
    mStageRt = rt;
    mStageTiers = b.tiers;
    job(P_MOMENTS, [&]()
    {
        // Pass 1: the static alpha state (vertex input, dynamic raster state), the world-only VS, the moments
        // FS, two additive float targets; no depth attachment (the FS tests the scene depth).
        VkShaderModule vs = mod(kWorldVertMbWorldSpv, kWorldVertMbWorldSpv_bytes, "VertMbWorld");
        VkShaderModule fs = mod(kWorldFragMbMomentsSpv, kWorldFragMbMomentsSpv_bytes, "FragMbMoments");
        GraphicsPipelineDesc d = b.statDesc[0];
        d.vert = vs; d.frag = fs; d.layout = mLayout; d.fragSpec = b.spec;
        d.views = 1; d.depthFormat = VK_FORMAT_UNDEFINED; d.depthTest = false; d.depthWrite = false;
        d.cullMode = VK_CULL_MODE_NONE; d.blend = false; d.reactFormat = VK_FORMAT_UNDEFINED; d.reactWriteMask = 0;
        d.dynamicShadingRate = false; d.fsrAttachment = false;
        d.mrtCount = 2; d.mrtFormats[0] = kMomAFormat; d.mrtFormats[1] = bm == 2u ? kMomB2Format : kMomB1Format;
        d.addMrt = true;
        createGraphicsPipelineInto(dev, d, &mStage[P_MOMENTS], kPipeName[P_MOMENTS]);
        drop(vs); drop(fs);
    });
    job(P_SHADE_FULL, [&]()
    {
        VkShaderModule vs = mod(kWorldVertMbWorldSpv, kWorldVertMbWorldSpv_bytes, "VertMbWorld");
        VkShaderModule fs = mod(kWorldFragMbShadeSpv, kWorldFragMbShadeSpv_bytes, "FragMbShade");
        createGraphicsPipelineInto(dev, shadeDesc(0, vs, fs), &mStage[P_SHADE_FULL], kPipeName[P_SHADE_FULL]);
        drop(vs); drop(fs);
    });
    if (rt)
        job(P_SHADE_FULL_RT, [&]()
        {
            VkShaderModule vs = mod(kWorldVertMbWorldSpv, kWorldVertMbWorldSpv_bytes, "VertMbWorld");
            VkShaderModule fs = mod(kWorldFragMbShadeRtSpv, kWorldFragMbShadeRtSpv_bytes, "FragMbShadeRt");
            createGraphicsPipelineInto(dev, shadeDesc(1, vs, fs), &mStage[P_SHADE_FULL_RT], kPipeName[P_SHADE_FULL_RT]);
            drop(vs); drop(fs);
        });
    if (b.tiers) job(P_SHADE_CHEAP, [&]()
    {
        VkShaderModule vs = mod(kWorldVertMbWorldSpv, kWorldVertMbWorldSpv_bytes, "VertMbWorld");
        VkShaderModule fs = mod(kWorldFragMbShadeCheapSpv, kWorldFragMbShadeCheapSpv_bytes, "FragMbShadeCheap");
        createGraphicsPipelineInto(dev, shadeDesc(0, vs, fs), &mStage[P_SHADE_CHEAP], kPipeName[P_SHADE_CHEAP]);
        drop(vs); drop(fs);
    });
    job(P_CUTOFF, [&]()
    {
        // Pass 2: the cutoff depth (no colour, compare ALWAYS, write on).
        VkShaderModule vs = mod(kWorldMbFullscreenVertSpv, kWorldMbFullscreenVertSpv_bytes, "MbFullscreenVert");
        VkShaderModule fs = mod(kWorldMbCutoffFragSpv, kWorldMbCutoffFragSpv_bytes, "MbCutoffFrag");
        GraphicsPipelineDesc d;
        d.vert = vs; d.frag = fs; d.layout = mLayout;
        d.noColor = true; d.depthFormat = b.depthFormat; d.depthTest = true; d.depthWrite = true;
        d.depthCompare = VK_COMPARE_OP_ALWAYS; d.cullMode = VK_CULL_MODE_NONE;
        createGraphicsPipelineInto(dev, d, &mStage[P_CUTOFF], kPipeName[P_CUTOFF]);
        drop(vs); drop(fs);
    });
    job(P_RESOLVE, [&]()
    {
        // Resolve: inside pass 2 -- dst * revealage + value (ONE / SRC_ALPHA), glow likewise, coverage MAX
        // into the reactive mask (R). As the WBOIT resolve.
        VkShaderModule vs = mod(kWorldMbFullscreenVertSpv, kWorldMbFullscreenVertSpv_bytes, "MbFullscreenVert");
        VkShaderModule fs = mod(kWorldMbResolveFragSpv, kWorldMbResolveFragSpv_bytes, "MbResolveFrag");
        GraphicsPipelineDesc d;
        d.vert = vs; d.frag = fs; d.layout = mLayout;
        d.colorFormat = b.hdrFormat; d.glowFormat = b.glowFormat;
        d.reactFormat = b.reactFormat; d.reactWriteMask = VK_COLOR_COMPONENT_R_BIT;
        d.depthFormat = b.depthFormat; d.depthTest = false; d.depthWrite = false;
        d.customBlend = true; d.srcColor = VK_BLEND_FACTOR_ONE; d.dstColor = VK_BLEND_FACTOR_SRC_ALPHA;
        d.cullMode = VK_CULL_MODE_NONE;
        d.fsrAttachment = b.statDesc[0].fsrAttachment;
        createGraphicsPipelineInto(dev, d, &mStage[P_RESOLVE], kPipeName[P_RESOLVE]);
        drop(vs); drop(fs);
    });
    mWantMode = b.mode;
    mWantRt = b.rt;
    mWantTiers = b.tiers;
    mBuiltKey = b.specKey;
    mBuiltFormats[0] = b.hdrFormat; mBuiltFormats[1] = b.glowFormat;
    mBuiltFormats[2] = b.reactFormat; mBuiltFormats[3] = b.depthFormat;
    mDepthFormat = b.depthFormat;
    LL_INFOS("VKAlphaMBOIT") << "MBOIT pipelines (" << (bm == 2u ? "6" : "4") << " power moments"
                             << (rt ? ", ray-query full tier" : "") << (b.tiers ? ", full + cheap tier passes" : ", one shade pass (no tiers)")
                             << "): " << (4 + (rt ? 1 : 0) + (b.tiers ? 1 : 0))
                             << " background jobs; the " << (mReady ? "previous set" : "sorted path") << " until all land" << LL_ENDL;
    return modeReady;
}

bool LLCleanAlphaMBOITVK::ensureTargets(VkDeviceContext* ctx, uint32_t mode, uint32_t rw, uint32_t rh, bool half)
{
    if (!ctx || !mDset || !mParams.buffer || mDepthFormat == VK_FORMAT_UNDEFINED || rw < 2u || rh < 2u) return false;
    if (mode != 2u) mode = 1u;
    if (mMode == mode && mW == rw && mH == rh && mHalf == half && mTarget[0].image) return true;
    const double now = mbClock();
    if (now < mRetryAt) return false;
    mCtx = ctx;
    vkDeviceWaitIdle(ctx->device());   // the set / images may be in use by frames in flight
    freeTargets();
    const uint32_t mw = half ? (rw + 1u) / 2u : rw, mh = half ? (rh + 1u) / 2u : rh;
    bool ok = true;
    {
        VkMemScope memcat(VKMEM_TARGET);
        VkDeviceContext::SoftAllocScope soft(ctx);   // out of memory: the sorted path, not a crash
        const VkImageUsageFlags u = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        static const char* const kNames[5] = {"mboitMomentsA", "mboitMomentsB", "mboitSum", "mboitGlow", "mboitReveal"};
        const VkFormat fmt[5] = {kMomAFormat, mode == 2u ? kMomB2Format : kMomB1Format, kSumFormat, kSumFormat, kRevealFormat};
        for (uint32_t i = 0; i < 5u && ok; ++i)
        {
            const bool mom = i < 2u;
            mTarget[i] = ctx->createImage2D(mom ? mw : rw, mom ? mh : rh, fmt[i], u, VK_IMAGE_ASPECT_COLOR_BIT);
            ok = mTarget[i].image != VK_NULL_HANDLE;
            if (ok) ctx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)mTarget[i].image, kNames[i]);
        }
        if (ok)
        {
            mDepth = ctx->createImage2D(rw, rh, mDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
            ok = mDepth.image != VK_NULL_HANDLE;
            if (ok) ctx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)mDepth.image, "mboitCutoffDepth");
        }
    }
    if (!ok)
    {
        freeTargets();
        mRetryAt = now + 5.0;
        LL_WARNS("VKAlphaMBOIT") << "MBOIT: target allocation failed (" << rw << "x" << rh << (half ? ", half-resolution moments" : "")
                                 << ", mode " << mode << "); the sorted path, retry in 5 s" << LL_ENDL;
        return false;
    }
    mMode = mode; mW = rw; mH = rh; mMw = mw; mMh = mh; mHalf = half;
    mFresh = true;
    writeSet();
    LL_INFOS("VKAlphaMBOIT") << "MBOIT targets: " << (mode == 2u ? "6" : "4") << " power moments at " << mw << "x" << mh
                             << (half ? " (half resolution)" : "") << ", render " << rw << "x" << rh << ": "
                             << (targetBytes() >> 20) << " MB (dedicated, freed when the mode is off)" << LL_ENDL;
    return true;
}

uint64_t LLCleanAlphaMBOITVK::targetBytes() const
{
    if (!mTarget[0].image) return 0;
    const uint64_t mpx = (uint64_t)mMw * mMh, px = (uint64_t)mW * mH;
    return mpx * (16u + (mMode == 2u ? 16u : 8u)) + px * (8u + 8u + 2u + 4u);
}

void LLCleanAlphaMBOITVK::writeSet()
{
    if (!mCtx || !mDset) return;
    VkDescriptorImageInfo ii[5]{};
    VkWriteDescriptorSet wr[6]{};
    uint32_t n = 0;
    if (mTarget[0].image)
    {
        for (uint32_t i = 0; i < 5u; ++i)
        {
            ii[i] = {VK_NULL_HANDLE, mTarget[i].view, VK_IMAGE_LAYOUT_GENERAL};
            wr[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[n].dstSet = mDset; wr[n].dstBinding = i;
            wr[n].descriptorCount = 1; wr[n].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; wr[n].pImageInfo = &ii[i];
            ++n;
        }
    }
    VkDescriptorBufferInfo bi{mParams.buffer, 0, sizeof(Params)};
    if (mParams.buffer)
    {
        wr[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[n].dstSet = mDset; wr[n].dstBinding = 5;
        wr[n].descriptorCount = 1; wr[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; wr[n].pBufferInfo = &bi;
        ++n;
    }
    if (n) vkUpdateDescriptorSets(mCtx->device(), n, wr, 0, nullptr);
}

void LLCleanAlphaMBOITVK::writeParams(VkCommandBuffer cmd, const Params& p)
{
    if (!mParams.buffer) return;
    // last frame's uniform reads -> this update -> this frame's reads
    mbMemBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdUpdateBuffer(cmd, mParams.buffer, 0, sizeof(Params), &p);
    mbMemBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_UNIFORM_READ_BIT);
}

void LLCleanAlphaMBOITVK::bindSet(VkCommandBuffer cmd, bool rtLayout) const
{
    if (!mDset) return;
    if (rtLayout && mLayoutRt) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mLayoutRt, 3, 1, &mDset, 0, nullptr);
    else vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mLayout, 2, 1, &mDset, 0, nullptr);
}

void LLCleanAlphaMBOITVK::beginMoments(VkCommandBuffer cmd)
{
    if (mFresh)
    {
        // New targets: their one layout transition (GENERAL / DEPTH_ATTACHMENT_OPTIMAL for life).
        VkImageMemoryBarrier2 b[6]{};
        for (uint32_t i = 0; i < 6u; ++i)
        {
            const bool depth = i == 5u;
            b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b[i].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            b[i].dstStageMask = depth ? kDepthStages : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            b[i].dstAccessMask = depth ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                                       : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
            b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b[i].newLayout = depth ? VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
            b[i].image = depth ? mDepth.image : mTarget[i].image;
            b[i].subresourceRange = {(VkImageAspectFlags)(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, 1};
        }
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.imageMemoryBarrierCount = 6; d.pImageMemoryBarriers = b;
        vkCmdPipelineBarrier2(cmd, &d);
        mFresh = false;
    }
    // the previous side's / frame's reads of the moments (cutoff, shade) -> this round's clear + blend
    mbMemBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    VkRenderingAttachmentInfo ca[2]{};
    for (uint32_t i = 0; i < 2u; ++i)
    {
        ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        ca[i].imageView = mTarget[i].view;
        ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ca[i].clearValue.color = {{0.f, 0.f, 0.f, 0.f}};
    }
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {mMw, mMh}}; ri.layerCount = 1;
    ri.colorAttachmentCount = 2; ri.pColorAttachments = ca;
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport vp{0.f, 0.f, (float)mMw, (float)mMh, 0.f, 1.f};
    const VkRect2D sc{{0, 0}, {mMw, mMh}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

void LLCleanAlphaMBOITVK::endMoments(VkCommandBuffer cmd)
{
    vkCmdEndRendering(cmd);
    mbMemBarrier(cmd, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void LLCleanAlphaMBOITVK::recordCutoff(VkCommandBuffer cmd)
{
    if (!mReady || !mLive[P_CUTOFF] || !mDepth.image) return;
    // the previous shade pass's depth tests -> this write
    mbMemBarrier(cmd, kDepthStages, 0, kDepthStages,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    da.imageView = mDepth.view; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;   // (every pixel written)
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {mW, mH}}; ri.layerCount = 1;
    ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport vp{0.f, 0.f, (float)mW, (float)mH, 0.f, 1.f};
    const VkRect2D sc{{0, 0}, {mW, mH}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mLive[P_CUTOFF]);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mLayout, 2, 1, &mDset, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    mbMemBarrier(cmd, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 kDepthStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
}

void LLCleanAlphaMBOITVK::beginShade(VkCommandBuffer cmd)
{
    // the previous resolve's reads of the sums -> this round's clear + blend
    mbMemBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    VkRenderingAttachmentInfo ca[3]{};
    for (uint32_t i = 0; i < 3u; ++i)
    {
        ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        ca[i].imageView = mTarget[2u + i].view;
        ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        const float c = i == 2u ? 1.f : 0.f;   // sums 0, revealage 1
        ca[i].clearValue.color = {{c, c, c, c}};
    }
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    da.imageView = mDepth.view; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_NONE;   // (tested, never written)
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {mW, mH}}; ri.layerCount = 1;
    ri.colorAttachmentCount = 3; ri.pColorAttachments = ca; ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);
}

void LLCleanAlphaMBOITVK::endShade(VkCommandBuffer cmd)
{
    vkCmdEndRendering(cmd);
    mbMemBarrier(cmd, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void LLCleanAlphaMBOITVK::recordResolve(VkCommandBuffer cmd)
{
    if (!mReady || !mLive[P_RESOLVE] || !mDset) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mLive[P_RESOLVE]);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mLayout, 2, 1, &mDset, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void LLCleanAlphaMBOITVK::freeTargets()
{
    if (!mCtx) return;
    for (GpuImage& t : mTarget)
        if (t.image) { mCtx->destroyImage(t); t = GpuImage{}; }
    if (mDepth.image) { mCtx->destroyImage(mDepth); mDepth = GpuImage{}; }
    mMode = 0; mW = mH = mMw = mMh = 0;
    mHalf = false;
}

void LLCleanAlphaMBOITVK::destroyStage(VkDevice dev)
{
    for (VkPipeline& p : mStage)
    {
        if (p) { vkForgetPipeline(p); vkDestroyPipeline(dev, p, nullptr); }
        p = VK_NULL_HANDLE;
    }
}

void LLCleanAlphaMBOITVK::destroyPipelines(VkDevice dev)
{
    vkDeviceWaitIdle(dev);
    for (VkPipeline& p : mLive)
    {
        if (p) { vkForgetPipeline(p); vkDestroyPipeline(dev, p, nullptr); }
        p = VK_NULL_HANDLE;
    }
    mReady = false;
}

void LLCleanAlphaMBOITVK::destroy(VkDevice dev)
{
    if (!dev) return;
    // (the renderer calls this only with no job running, except at shutdown: stop them starting more compiles,
    //  then wait for the ones already in the driver -- the stage pipelines they write are ours)
    cancelJobs();
    for (PipelineJobSlot& j : mSlot) j.wait();
    mInRound = false;
    destroyStage(dev);
    destroyPipelines(dev);   // (waits for the device)
    if (mLayout) { vkDestroyPipelineLayout(dev, mLayout, nullptr); mLayout = VK_NULL_HANDLE; }
    if (mLayoutRt) { vkDestroyPipelineLayout(dev, mLayoutRt, nullptr); mLayoutRt = VK_NULL_HANDLE; }
    mBuiltKey = ~0ull;
    for (VkFormat& f : mBuiltFormats) f = VK_FORMAT_UNDEFINED;
    mWantMode = mBuiltMode = 0;
    freeTargets();
    if (mParams.buffer && mCtx) { mCtx->destroyBuffer(mParams); mParams = GpuBuffer{}; }
    mPool.destroy();
    if (mDsl) vkdDestroySetLayout(dev, mDsl);
    mDsl = VK_NULL_HANDLE; mDset = VK_NULL_HANDLE;
}
