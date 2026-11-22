/**
 * @file llcleanblendsplitvk.cpp
 * @brief Split transparency (RenderVKBlendSplit): see llcleanblendsplitvk.h, clean_bs_common.glsl,
 *        clean_bs_volume.frag, clean_bs_light.frag, clean_bs_debug.frag and clean_draw.frag (CLEAN_BS_FRONT,
 *        CLEAN_BLEND_SPLIT).
 */
#include "linden_common.h"

#include "llcleanblendsplitvk.h"
#include "llcleanalphamboitvk.h"
#include "llcleanpipeneeds.h"
#include "vk_world_shaders_spv.h"

#include "llerror.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace
{
double bsClock()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

VkShaderModule bsModule(VkDevice dev, const uint32_t* code, size_t bytes, const char* name)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
    vkNoteShaderModule(m, code, bytes, name);
    return m;
}

void bsMemBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src, VkAccessFlags2 srcA, VkPipelineStageFlags2 dst, VkAccessFlags2 dstA)
{
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = src; mb.srcAccessMask = srcA; mb.dstStageMask = dst; mb.dstAccessMask = dstA;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &d);
}

constexpr VkPipelineStageFlags2 kDepthStages = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

// The set's bindings (clean_bs_common.glsl THE SET).
constexpr uint32_t kBindParams = 0, kBindVolRead = 1, kBindFront = 7, kBindDepth = 12, kBindLit = 13, kBindVolWrite = 14,
                   kBindHist = 20, kBindCount = 24;

// CREATE-INFO GUARD (the first in-world run crashed in the driver, 2026-11-22: stage B / D took the LIT_ALPHA desc,
// which the renderer records WITHOUT its modules -- mLitDbgDesc keeps vert / frag / fragSpec null -- so the driver
// dereferenced a null VS). Every pipeline is checked before it is queued: a vertex (or mesh) and a fragment module,
// a layout, and no blending on an integer attachment. A refused desc is logged and not built (the round then fails
// and the sorted path stays).
bool bsIntegerFormat(VkFormat f)
{
    switch (f)
    {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R16_UINT:
    case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R32_SINT:
    case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32A32_SINT:
        return true;
    default:
        return false;
    }
}
const char* bsDescProblem(const GraphicsPipelineDesc& d)
{
    if (!d.vert && !d.mesh) return "no vertex shader module";
    if (!d.frag) return "no fragment shader module";
    if (!d.layout) return "no pipeline layout";
    if (d.fragSpec && d.fragSpec->dataSize && !d.fragSpec->pData) return "specialization data size without data";
    if (d.fragSpec && d.fragSpec->mapEntryCount && !d.fragSpec->pMapEntries) return "specialization entries missing";
    if (!d.noColor)
    {
        const bool anyBlend = d.blend || d.additive || d.customBlend || d.oitMrt || d.addMrt || d.premultipliedOver;
        if (d.mrtCount > 0)
        {
            for (uint32_t i = 0; i < std::min<uint32_t>(d.mrtCount, GraphicsPipelineDesc::kMaxMrt); ++i)
            {
                if (d.mrtFormats[i] == VK_FORMAT_UNDEFINED) return "an MRT attachment without a format";
                if ((d.oitMrt || d.addMrt) && bsIntegerFormat(d.mrtFormats[i])) return "blending on an integer MRT attachment";
            }
        }
        else if (anyBlend && (bsIntegerFormat(d.colorFormat) || bsIntegerFormat(d.glowFormat) || bsIntegerFormat(d.reactFormat)))
            return "blending on an integer attachment";
    }
    return nullptr;
}
void bsCreate(VkDevice dev, const GraphicsPipelineDesc& d, VkPipeline* out, const char* name)
{
    if (const char* why = bsDescProblem(d))
    {
        LL_WARNS("VKBlendSplit") << name << ": pipeline NOT created -- invalid create info (" << why << ")" << LL_ENDL;
        return;
    }
    createGraphicsPipelineInto(dev, d, out, name);
}

const char* const kPipeName[LLCleanBlendSplitVK::P_COUNT] = {
    "split transparency volume", "split transparency volume (rayquery)", "split transparency front surface",
    "split transparency front light", "split transparency front light (rayquery)", "split transparency blend",
    "split transparency blend (MBOIT shade)", "split transparency slice view"};
} // namespace

bool LLCleanBlendSplitVK::pipelinesPending() const
{
    for (const PipelineJobSlot& j : mSlot) if (j.pending()) return true;
    return false;
}

void LLCleanBlendSplitVK::cancelJobs()
{
    for (PipelineJobSlot& j : mSlot) if (j.job) pipelineJobCancel(j.job);
}

bool LLCleanBlendSplitVK::ensurePipelines(const Build& b)
{
    if (!b.ctx || !b.alphaDesc.layout) return false;
    VkDevice dev = b.ctx->device();
    for (uint32_t i = 0; i < P_COUNT; ++i)
        if (mSlot[i].poll())
            LL_INFOS("VKBlendSplit") << kPipeName[i] << ": " << (mStage[i] ? "compiled" : "FAILED") << " in "
                                     << (S32)((bsClock() - mSince[i]) * 1000.0) << " ms wall (background)" << LL_ENDL;
    if (mInRound && !pipelinesPending())
    {
        // The round is done: install it whole. The ray-query twins and the MBOIT shade are optional (the plain
        // volume / light take a traced frame's place; MBOIT frames keep MBOIT's own shade).
        mInRound = false;
        const bool ok = mStage[P_VOLUME] && mStage[P_FRONT] && mStage[P_LIGHT] && mStage[P_BLEND] && mStage[P_SLICE];
        if (ok)
        {
            if (mReady) destroyPipelines(dev);   // (a key change: the old set may be in flight -- one device wait)
            std::memcpy(mLive, mStage, sizeof(mLive));
            std::memset(mStage, 0, sizeof(mStage));
            mReady = true;
        }
        else
        {
            destroyStage(dev);
            mRetryBuildAt = bsClock() + 5.0;
        }
        LL_INFOS("VKBlendSplit") << "split transparency pipelines "
                                 << (ok ? "ready" : (mReady ? "FAILED (the previous set stays)" : "FAILED (the sorted path stays; retry in 5 s)"))
                                 << " after " << (S32)((bsClock() - mRoundSince) * 1000.0) << " ms wall"
                                 << (ok && mStageRt && !(mLive[P_VOLUME_RT] && mLive[P_LIGHT_RT]) ? "; a ray-query twin failed (the plain ones run)" : "")
                                 << LL_ENDL;
    }
    if (pipelinesPending() || bsClock() < mRetryBuildAt) return mReady;
    const bool fmtChanged = mBuiltFormats[0] != b.hdrFormat || mBuiltFormats[1] != b.glowFormat
                         || mBuiltFormats[2] != b.reactFormat || mBuiltFormats[3] != b.depthFormat;
    const bool wantMb = b.dslMb != VK_NULL_HANDLE;
    const bool sameSel = mWantRt == b.rt && mWantMb == wantMb;
    if (mBuiltKey == b.specKey && !fmtChanged && sameSel) return mReady;
    if (mReady && !fmtChanged && b.generic && sameSel) { mBuiltKey = b.specKey; return mReady; }
    if (fmtChanged && mReady) destroyPipelines(dev);
    if (fmtChanged && mFrontDepth.image)
    {   // (the front depth takes the scene depth's format)
        vkDeviceWaitIdle(dev);
        freeTargets();
    }
    mCtx = b.ctx;
    bool setDirty = false;
    if (!mDsl)
    {
        setDirty = true;
        VkDescriptorSetLayoutBinding sb[kBindCount]{};
        for (uint32_t i = 0; i < kBindCount; ++i)
        {
            sb[i].binding = i;
            sb[i].descriptorCount = 1;
            sb[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            if (i == kBindParams) sb[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            else if (i < kBindFront || i >= kBindHist) sb[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            else if (i < kBindVolWrite) sb[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            else sb[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        }
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = kBindCount; lci.pBindings = sb;
        VK_CHECK(vkdCreateSetLayout(dev, &lci, &mDsl, "mDslBlendSplit"));
        VK_CHECK(mPool.create(dev, "blendsplit", {{mDsl, 2}}));
        VKD_ALLOC_SOFT(mPool.allocate(mDsl, &mDset[0]), { destroy(dev); return false; });
        VKD_ALLOC_SOFT(mPool.allocate(mDsl, &mDset[1]), { destroy(dev); return false; });
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VK_CHECK(vkCreateSampler(dev, &sci, nullptr, &mSampler));
    }
    if (!mParams.buffer)
    {
        if (!setDirty) vkDeviceWaitIdle(dev);
        setDirty = true;
        mParams = mCtx->createBuffer(sizeof(Params), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!mParams.buffer) { mRetryBuildAt = bsClock() + 5.0; return false; }
    }
    VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, b.pushBytes};
    if (!mLayout)
    {
        const VkDescriptorSetLayout sets[3] = {b.dslDraw0, b.dslDraw1, mDsl};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 3; pli.pSetLayouts = sets;
        pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
        VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mLayout, "mBlendSplitLayout"));
    }
    if (!mLayoutRt && b.dslRt)
    {
        const VkDescriptorSetLayout sets[4] = {b.dslDraw0, b.dslDraw1, mDsl, b.dslRt};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 4; pli.pSetLayouts = sets;
        pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
        VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mLayoutRt, "mBlendSplitLayoutRt"));
    }
    if (!mLayoutMb && b.dslMb)
    {
        const VkDescriptorSetLayout sets[4] = {b.dslDraw0, b.dslDraw1, b.dslMb, mDsl};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 4; pli.pSetLayouts = sets;
        pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
        VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mLayoutMb, "mBlendSplitLayoutMb"));
    }
    if (setDirty) writeSets();

    // ---- a new round: one job per pipeline ----
    const bool rt = b.rt && mLayoutRt;
    auto job = [&](uint32_t i, const auto& fill)
    {
        mSince[i] = bsClock();
        {
            PipelineBatchScope batchScope(dev);   // (modules are made per job: each batch destroys its own)
            fill();
            PipelineJobOpts o;
            o.prio = VKPIPE_PRIO_USER;
            o.gate = false;
            o.what = kPipeName[i];
            o.family = (i == P_VOLUME_RT || i == P_LIGHT_RT) ? llpipeneeds::FAM_BLEND_SPLIT_RT : llpipeneeds::FAM_BLEND_SPLIT;
            mSlot[i].start(pipelineBatchFlushAsync(dev, o));
        }
        LL_INFOS("VKBlendSplit") << kPipeName[i] << ": " << (mSlot[i].pending() ? "compile started (background)" : "refused / nothing queued")
                                 << LL_ENDL;
    };
    auto mod = [&](const uint32_t* code, size_t bytes, const char* name) { return bsModule(dev, code, bytes, name); };
    auto drop = [&](VkShaderModule m) { if (m) destroyShaderModuleDeferred(dev, m, nullptr); };
    // A fullscreen pass (the MBOIT fullscreen VS): no vertex input.
    auto fullscreen = [&](VkShaderModule vs, VkShaderModule fs, VkPipelineLayout lay)
    {
        GraphicsPipelineDesc d;
        d.vert = vs; d.frag = fs; d.layout = lay; d.fragSpec = b.spec;
        d.cullMode = VK_CULL_MODE_NONE; d.depthTest = false; d.depthWrite = false;
        return d;
    };
    mInRound = true;
    mRoundSince = bsClock();
    mStageRt = rt;
    auto volume = [&](uint32_t i, const uint32_t* code, size_t bytes, const char* name, VkPipelineLayout lay)
    {
        job(i, [&]()
        {
            // A: no attachments (every fragment stores its cell).
            VkShaderModule vs = mod(kWorldMbFullscreenVertSpv, kWorldMbFullscreenVertSpv_bytes, "MbFullscreenVert");
            VkShaderModule fs = mod(code, bytes, name);
            GraphicsPipelineDesc d = fullscreen(vs, fs, lay);
            d.noColor = true; d.depthFormat = VK_FORMAT_UNDEFINED;
            bsCreate(dev, d, &mStage[i], kPipeName[i]);
            drop(vs); drop(fs);
        });
    };
    auto light = [&](uint32_t i, const uint32_t* code, size_t bytes, const char* name, VkPipelineLayout lay)
    {
        job(i, [&]()
        {
            // C: the lit colour, written in full (pixels without a front layer discard).
            VkShaderModule vs = mod(kWorldMbFullscreenVertSpv, kWorldMbFullscreenVertSpv_bytes, "MbFullscreenVert");
            VkShaderModule fs = mod(code, bytes, name);
            GraphicsPipelineDesc d = fullscreen(vs, fs, lay);
            d.colorFormat = kLitFormat; d.glowFormat = VK_FORMAT_UNDEFINED; d.depthFormat = VK_FORMAT_UNDEFINED;
            bsCreate(dev, d, &mStage[i], kPipeName[i]);
            drop(vs); drop(fs);
        });
    };
    volume(P_VOLUME, kWorldBsVolumeSpv, kWorldBsVolumeSpv_bytes, "BsVolume", mLayout);
    if (rt) volume(P_VOLUME_RT, kWorldBsVolumeRtSpv, kWorldBsVolumeRtSpv_bytes, "BsVolumeRt", mLayoutRt);
    light(P_LIGHT, kWorldBsLightSpv, kWorldBsLightSpv_bytes, "BsLight", mLayout);
    if (rt) light(P_LIGHT_RT, kWorldBsLightRtSpv, kWorldBsLightRtSpv_bytes, "BsLightRt", mLayoutRt);
    job(P_FRONT, [&]()
    {
        // B: the static alpha state's vertex input and VS, the front-surface FS; the nearest qualifying layer by the
        // hardware test (reversed-Z GREATER, written); five plain attachments; both faces in one draw (the FS drops a
        // single-sided face's back, as the sorted pass's FS / VS collapse do).
        // (the recorded LIT_ALPHA desc carries NO modules -- mLitDbgDesc: the static world VS is ours to make)
        VkShaderModule vs = mod(kWorldVertSpv, kWorldVertSpv_bytes, "kWorldVertSpv");
        VkShaderModule fs = mod(kWorldFragBsFrontSpv, kWorldFragBsFrontSpv_bytes, "FragBsFront");
        GraphicsPipelineDesc d = b.alphaDesc;
        d.vert = vs; d.frag = fs; d.layout = mLayout; d.fragSpec = b.spec;
        d.views = 1; d.depthFormat = b.depthFormat; d.depthTest = true; d.depthWrite = true;
        d.depthCompare = VK_COMPARE_OP_LESS; d.reverseZ = true;
        d.relaxedDepthCompare = VK_COMPARE_OP_NEVER;
        d.dynamicDepthCull = false;
        d.cullMode = VK_CULL_MODE_NONE; d.blend = false; d.reactFormat = VK_FORMAT_UNDEFINED; d.reactWriteMask = 0;
        d.dynamicShadingRate = false; d.fsrAttachment = false;
        d.mrtCount = 5;
        d.mrtFormats[0] = kF0Format; d.mrtFormats[1] = kF1Format; d.mrtFormats[2] = kF2Format;
        d.mrtFormats[3] = kF3Format; d.mrtFormats[4] = kF4Format;
        bsCreate(dev, d, &mStage[P_FRONT], kPipeName[P_FRONT]);
        drop(vs); drop(fs);
    });
    job(P_BLEND, [&]()
    {
        // D: the sorted LIT_ALPHA pipeline's state exactly (attachments, blend, dynamic raster, VS), the split FS and
        // the module's layout.
        VkShaderModule vs = mod(kWorldVertSpv, kWorldVertSpv_bytes, "kWorldVertSpv");
        VkShaderModule fs = mod(kWorldFragBsBlendSpv, kWorldFragBsBlendSpv_bytes, "FragBsBlend");
        GraphicsPipelineDesc d = b.alphaDesc;
        d.vert = vs; d.frag = fs; d.layout = mLayout; d.fragSpec = b.spec;
        bsCreate(dev, d, &mStage[P_BLEND], kPipeName[P_BLEND]);
        drop(vs); drop(fs);
    });
    if (b.dslMb && mLayoutMb)
        job(P_BLEND_MB, [&]()
        {
            // D under MBOIT: its shade pass's state (llcleanalphamboitvk.cpp shadeDesc), the split FS, one pass.
            VkShaderModule vs = mod(kWorldVertMbWorldSpv, kWorldVertMbWorldSpv_bytes, "VertMbWorld");
            VkShaderModule fs = mod(kWorldFragBsBlendMbSpv, kWorldFragBsBlendMbSpv_bytes, "FragBsBlendMb");
            GraphicsPipelineDesc d = b.alphaDesc;
            d.vert = vs; d.frag = fs; d.layout = mLayoutMb; d.fragSpec = b.spec;
            d.views = 1; d.depthFormat = b.depthFormat; d.depthTest = true; d.depthWrite = false;
            d.depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL; d.reverseZ = true;
            d.relaxedDepthCompare = VK_COMPARE_OP_NEVER;
            d.cullMode = VK_CULL_MODE_NONE; d.blend = false; d.reactFormat = VK_FORMAT_UNDEFINED; d.reactWriteMask = 0;
            d.dynamicShadingRate = false; d.fsrAttachment = false;
            d.mrtCount = 3;
            d.mrtFormats[0] = LLCleanAlphaMBOITVK::kSumFormat; d.mrtFormats[1] = LLCleanAlphaMBOITVK::kSumFormat;
            d.mrtFormats[2] = LLCleanAlphaMBOITVK::kRevealFormat;
            d.oitMrt = true;
            bsCreate(dev, d, &mStage[P_BLEND_MB], kPipeName[P_BLEND_MB]);
            drop(vs); drop(fs);
        });
    job(P_SLICE, [&]()
    {
        // The slice view: inside pass 2 (its attachments; colour blended over, glow / reactive masked).
        VkShaderModule vs = mod(kWorldMbFullscreenVertSpv, kWorldMbFullscreenVertSpv_bytes, "MbFullscreenVert");
        VkShaderModule fs = mod(kWorldBsDebugSpv, kWorldBsDebugSpv_bytes, "BsDebug");
        GraphicsPipelineDesc d = fullscreen(vs, fs, mLayout);
        d.fragSpec = nullptr;
        d.colorFormat = b.hdrFormat; d.glowFormat = b.glowFormat; d.glowWrite = false;
        d.reactFormat = b.reactFormat; d.reactWriteMask = 0;
        d.depthFormat = b.depthFormat; d.blend = true;
        d.fsrAttachment = b.alphaDesc.fsrAttachment;
        bsCreate(dev, d, &mStage[P_SLICE], kPipeName[P_SLICE]);
        drop(vs); drop(fs);
    });
    mWantRt = b.rt;
    mWantMb = wantMb;
    mBuiltKey = b.specKey;
    mBuiltFormats[0] = b.hdrFormat; mBuiltFormats[1] = b.glowFormat;
    mBuiltFormats[2] = b.reactFormat; mBuiltFormats[3] = b.depthFormat;
    mDepthFormat = b.depthFormat;
    LL_INFOS("VKBlendSplit") << "split transparency pipelines" << (rt ? " (+ ray-query twins)" : "") << (wantMb ? " (+ MBOIT shade)" : "")
                             << ": background jobs; the " << (mReady ? "previous set" : "sorted path") << " until all land" << LL_ENDL;
    return mReady;
}

bool LLCleanBlendSplitVK::ensureTargets(VkDeviceContext* ctx, uint32_t w, uint32_t h, uint32_t d, uint32_t rw, uint32_t rh)
{
    if (!ctx || !mDset[0] || !mParams.buffer || mDepthFormat == VK_FORMAT_UNDEFINED || rw < 2u || rh < 2u) return false;
    if (mVw == w && mVh == h && mVd == d && mW == rw && mH == rh && mVol[0][0].image && mFront[0].image) return true;
    const double now = bsClock();
    if (now < mRetryAt) return false;
    mCtx = ctx;
    vkDeviceWaitIdle(ctx->device());   // the sets / images may be in use by frames in flight
    freeTargets();
    bool ok = true;
    {
        VkMemScope memcat(VKMEM_TARGET);
        VkDeviceContext::SoftAllocScope soft(ctx);   // out of memory: the sorted path, not a crash
        const VkImageUsageFlags u3 = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        for (uint32_t p = 0; p < 2u && ok; ++p)
            for (uint32_t i = 0; i < 4u && ok; ++i)
            {
                mVol[p][i] = ctx->createImage3D(w, h, d, kVolFormat, u3);
                ok = mVol[p][i].image != VK_NULL_HANDLE;
                if (ok) ctx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)mVol[p][i].image, "blendSplitVolume");
            }
        for (uint32_t i = 0; i < 2u && ok; ++i)
        {
            mPost[i] = ctx->createImage3D(w, h, d, kVolFormat, u3);
            ok = mPost[i].image != VK_NULL_HANDLE;
            if (ok) ctx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)mPost[i].image, "blendSplitTail");
        }
        const VkFormat ff[5] = {kF0Format, kF1Format, kF2Format, kF3Format, kF4Format};
        const VkImageUsageFlags u2 = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        for (uint32_t i = 0; i < 5u && ok; ++i)
        {
            mFront[i] = ctx->createImage2D(rw, rh, ff[i], u2, VK_IMAGE_ASPECT_COLOR_BIT);
            ok = mFront[i].image != VK_NULL_HANDLE;
            if (ok) ctx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)mFront[i].image, "blendSplitFront");
        }
        if (ok)
        {
            mLit = ctx->createImage2D(rw, rh, kLitFormat, u2, VK_IMAGE_ASPECT_COLOR_BIT);
            ok = mLit.image != VK_NULL_HANDLE;
            if (ok) ctx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)mLit.image, "blendSplitLit");
        }
        if (ok)
        {
            mFrontDepth = ctx->createImage2D(rw, rh, mDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                             VK_IMAGE_ASPECT_DEPTH_BIT);
            ok = mFrontDepth.image != VK_NULL_HANDLE;
            if (ok) ctx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)mFrontDepth.image, "blendSplitFrontDepth");
        }
    }
    if (!ok)
    {
        freeTargets();
        mRetryAt = now + 5.0;
        LL_WARNS("VKBlendSplit") << "split transparency: target allocation failed (volume " << w << "x" << h << "x" << d
                                 << ", render " << rw << "x" << rh << "); the sorted path, retry in 5 s" << LL_ENDL;
        return false;
    }
    mVw = w; mVh = h; mVd = d; mW = rw; mH = rh;
    // The volume pass's 2D layout: as many slices per row as the 16384-pixel viewport limit allows.
    mTilesX = std::max<uint32_t>(1u, std::min<uint32_t>(d, 16384u / std::max(w, 1u)));
    mTilesY = (d + mTilesX - 1u) / mTilesX;
    mFresh = true;
    mParity = 0;
    writeSets();
    LL_INFOS("VKBlendSplit") << "split transparency targets: volume " << w << "x" << h << "x" << d << " ("
                             << (volumeBytes() >> 20) << " MB), front surface " << rw << "x" << rh << " ("
                             << (frontBytes() >> 20) << " MB); dedicated, freed when the mode is off" << LL_ENDL;
    return true;
}

uint64_t LLCleanBlendSplitVK::volumeBytes() const
{
    return mVol[0][0].image ? (uint64_t)mVw * mVh * mVd * 8u * 10u : 0u;
}

uint64_t LLCleanBlendSplitVK::frontBytes() const
{
    return mFront[0].image ? (uint64_t)mW * mH * (4u + 8u + 4u + 4u + 8u + 4u + 8u) : 0u;
}

void LLCleanBlendSplitVK::writeSets()
{
    if (!mCtx || !mDset[0]) return;
    for (uint32_t p = 0; p < 2u; ++p)
    {
        VkDescriptorImageInfo ii[kBindCount]{};
        VkWriteDescriptorSet wr[kBindCount]{};
        uint32_t n = 0;
        auto img = [&](uint32_t binding, VkDescriptorType type, VkImageView view, VkImageLayout layout, VkSampler smp)
        {
            ii[binding] = {smp, view, layout};
            wr[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[n].dstSet = mDset[p]; wr[n].dstBinding = binding;
            wr[n].descriptorCount = 1; wr[n].descriptorType = type; wr[n].pImageInfo = &ii[binding];
            ++n;
        };
        if (mVol[0][0].image && mFront[0].image)
        {
            for (uint32_t i = 0; i < 4u; ++i)
            {
                img(kBindVolRead + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, mVol[p][i].view, VK_IMAGE_LAYOUT_GENERAL, mSampler);
                img(kBindVolWrite + i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, mVol[p][i].view, VK_IMAGE_LAYOUT_GENERAL, VK_NULL_HANDLE);
                img(kBindHist + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, mVol[p ^ 1u][i].view, VK_IMAGE_LAYOUT_GENERAL, mSampler);
            }
            for (uint32_t i = 0; i < 2u; ++i)
            {
                img(kBindVolRead + 4u + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, mPost[i].view, VK_IMAGE_LAYOUT_GENERAL, mSampler);
                img(kBindVolWrite + 4u + i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, mPost[i].view, VK_IMAGE_LAYOUT_GENERAL, VK_NULL_HANDLE);
            }
            for (uint32_t i = 0; i < 5u; ++i)
                img(kBindFront + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, mFront[i].view, VK_IMAGE_LAYOUT_GENERAL, VK_NULL_HANDLE);
            img(kBindDepth, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, mFrontDepth.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_NULL_HANDLE);
            img(kBindLit, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, mLit.view, VK_IMAGE_LAYOUT_GENERAL, VK_NULL_HANDLE);
        }
        VkDescriptorBufferInfo bi{mParams.buffer, 0, sizeof(Params)};
        if (mParams.buffer)
        {
            wr[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[n].dstSet = mDset[p]; wr[n].dstBinding = kBindParams;
            wr[n].descriptorCount = 1; wr[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; wr[n].pBufferInfo = &bi;
            ++n;
        }
        if (n) vkUpdateDescriptorSets(mCtx->device(), n, wr, 0, nullptr);
    }
}

void LLCleanBlendSplitVK::beginFrame(VkCommandBuffer cmd, const Params& p)
{
    if (!mParams.buffer) return;
    mParity ^= 1u;
    // last frame's uniform reads -> this update -> this frame's reads
    bsMemBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdUpdateBuffer(cmd, mParams.buffer, 0, sizeof(Params), &p);
    bsMemBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_UNIFORM_READ_BIT);
    if (mFresh)
    {
        // New targets: their one layout transition (GENERAL for life; the front depth goes per frame).
        VkImageMemoryBarrier2 b[16]{};
        uint32_t n = 0;
        auto toGeneral = [&](const GpuImage& im)
        {
            b[n].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b[n].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            b[n].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            b[n].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
                               | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            b[n].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b[n].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b[n].image = im.image;
            b[n].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            ++n;
        };
        for (uint32_t q = 0; q < 2u; ++q) for (uint32_t i = 0; i < 4u; ++i) toGeneral(mVol[q][i]);
        for (uint32_t i = 0; i < 2u; ++i) toGeneral(mPost[i]);
        for (uint32_t i = 0; i < 5u; ++i) toGeneral(mFront[i]);
        toGeneral(mLit);
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.imageMemoryBarrierCount = n; d.pImageMemoryBarriers = b;
        vkCmdPipelineBarrier2(cmd, &d);
        mFresh = false;
    }
}

void LLCleanBlendSplitVK::bindSet(VkCommandBuffer cmd) const
{
    if (mDset[mParity]) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mLayout, 2, 1, &mDset[mParity], 0, nullptr);
}

void LLCleanBlendSplitVK::bindSetMb(VkCommandBuffer cmd) const
{
    if (mDset[mParity] && mLayoutMb)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mLayoutMb, 3, 1, &mDset[mParity], 0, nullptr);
}

void LLCleanBlendSplitVK::recordVolume(VkCommandBuffer cmd, VkDescriptorSet rtSet)
{
    if (!mReady || !mVol[0][0].image) return;
    const bool rt = rtSet && mLive[P_VOLUME_RT] && mLayoutRt;
    // the previous readers of this parity's lighting / the tail (stage D and C two frames back, the history read last
    // frame, the tail last frame) -> this pass's stores
    bsMemBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    const uint32_t vw = mVw * mTilesX, vh = mVh * mTilesY;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {vw, vh}}; ri.layerCount = 1;
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport vp{0.f, 0.f, (float)vw, (float)vh, 0.f, 1.f};
    const VkRect2D sc{{0, 0}, {vw, vh}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rt ? mLive[P_VOLUME_RT] : mLive[P_VOLUME]);
    if (rt)
    {
        const VkDescriptorSet s[2] = {mDset[mParity], rtSet};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mLayoutRt, 2, 2, s, 0, nullptr);
    }
    else vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mLayout, 2, 1, &mDset[mParity], 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    // the stores -> stages B (none), C and D's sampled reads, next frame's history read
    bsMemBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void LLCleanBlendSplitVK::beginFront(VkCommandBuffer cmd)
{
    // last frame's reads of the surface / its depth (C, D) -> this round's clear + writes; the depth's contents are
    // dropped (UNDEFINED: it is cleared)
    bsMemBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT; b.srcAccessMask = 0;
        b.dstStageMask = kDepthStages;
        b.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        b.image = mFrontDepth.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &d);
    }
    VkRenderingAttachmentInfo ca[5]{};
    for (uint32_t i = 0; i < 5u; ++i)
    {
        ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        ca[i].imageView = mFront[i].view;
        ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        // (F4, the identity, is cleared: 0 = no front layer; the others are read only where it is valid)
        ca[i].loadOp = i == 4u ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ca[i].clearValue.color = {{0.f, 0.f, 0.f, 0.f}};
    }
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    da.imageView = mFrontDepth.view; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.clearValue.depthStencil = {0.f, 0u};   // (reversed-Z: the far plane)
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {mW, mH}}; ri.layerCount = 1;
    ri.colorAttachmentCount = 5; ri.pColorAttachments = ca; ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport vp{0.f, 0.f, (float)mW, (float)mH, 0.f, 1.f};
    const VkRect2D sc{{0, 0}, {mW, mH}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

void LLCleanBlendSplitVK::endFront(VkCommandBuffer cmd)
{
    vkCmdEndRendering(cmd);
    bsMemBarrier(cmd, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT; b.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT; b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    b.image = mFrontDepth.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

void LLCleanBlendSplitVK::recordLight(VkCommandBuffer cmd, VkDescriptorSet rtSet)
{
    if (!mReady || !mLit.image) return;
    const bool rt = rtSet && mLive[P_LIGHT_RT] && mLayoutRt;
    // last frame's reads of the lit colour (D) -> this write
    bsMemBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = mLit.view; ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;   // (read only at front pixels)
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {mW, mH}}; ri.layerCount = 1;
    ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport vp{0.f, 0.f, (float)mW, (float)mH, 0.f, 1.f};
    const VkRect2D sc{{0, 0}, {mW, mH}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rt ? mLive[P_LIGHT_RT] : mLive[P_LIGHT]);
    if (rt)
    {
        const VkDescriptorSet s[2] = {mDset[mParity], rtSet};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mLayoutRt, 2, 2, s, 0, nullptr);
    }
    else vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mLayout, 2, 1, &mDset[mParity], 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    bsMemBarrier(cmd, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void LLCleanBlendSplitVK::recordSlice(VkCommandBuffer cmd)
{
    if (!mReady || !mLive[P_SLICE] || !mVol[0][0].image) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mLive[P_SLICE]);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mLayout, 2, 1, &mDset[mParity], 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void LLCleanBlendSplitVK::freeTargets()
{
    if (!mCtx) return;
    for (auto& row : mVol) for (GpuImage& t : row) if (t.image) { mCtx->destroyImage(t); t = GpuImage{}; }
    for (GpuImage& t : mPost) if (t.image) { mCtx->destroyImage(t); t = GpuImage{}; }
    for (GpuImage& t : mFront) if (t.image) { mCtx->destroyImage(t); t = GpuImage{}; }
    if (mFrontDepth.image) { mCtx->destroyImage(mFrontDepth); mFrontDepth = GpuImage{}; }
    if (mLit.image) { mCtx->destroyImage(mLit); mLit = GpuImage{}; }
    mVw = mVh = mVd = mW = mH = 0;
}

void LLCleanBlendSplitVK::destroyStage(VkDevice dev)
{
    for (VkPipeline& p : mStage)
    {
        if (p) { vkForgetPipeline(p); vkDestroyPipeline(dev, p, nullptr); }
        p = VK_NULL_HANDLE;
    }
}

void LLCleanBlendSplitVK::destroyPipelines(VkDevice dev)
{
    vkDeviceWaitIdle(dev);
    for (VkPipeline& p : mLive)
    {
        if (p) { vkForgetPipeline(p); vkDestroyPipeline(dev, p, nullptr); }
        p = VK_NULL_HANDLE;
    }
    mReady = false;
}

void LLCleanBlendSplitVK::destroy(VkDevice dev)
{
    if (!dev) return;
    cancelJobs();
    for (PipelineJobSlot& j : mSlot) j.wait();
    mInRound = false;
    destroyStage(dev);
    destroyPipelines(dev);   // (waits for the device)
    if (mLayout) { vkDestroyPipelineLayout(dev, mLayout, nullptr); mLayout = VK_NULL_HANDLE; }
    if (mLayoutRt) { vkDestroyPipelineLayout(dev, mLayoutRt, nullptr); mLayoutRt = VK_NULL_HANDLE; }
    if (mLayoutMb) { vkDestroyPipelineLayout(dev, mLayoutMb, nullptr); mLayoutMb = VK_NULL_HANDLE; }
    mBuiltKey = ~0ull;
    for (VkFormat& f : mBuiltFormats) f = VK_FORMAT_UNDEFINED;
    mWantRt = mWantMb = false;
    freeTargets();
    if (mParams.buffer && mCtx) { mCtx->destroyBuffer(mParams); mParams = GpuBuffer{}; }
    if (mSampler) { vkDestroySampler(dev, mSampler, nullptr); mSampler = VK_NULL_HANDLE; }
    mPool.destroy();
    if (mDsl) vkdDestroySetLayout(dev, mDsl);
    mDsl = VK_NULL_HANDLE; mDset[0] = mDset[1] = VK_NULL_HANDLE;
}
