/**
 * @file llvrspacewarpvk.cpp
 * @brief #1594 VR space warp, the GPU side (history capture + the warp pass). See llvrspacewarpvk.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 * $/LicenseInfo$
 */
#include "linden_common.h"

#include "llvrspacewarpvk.h"
#include "vk_world_shaders_spv.h"

#include "llerror.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace
{
// clean_vr_spacewarp.comp's std140 block, byte for byte.
struct EyeParams
{
    float fwd[16];
    float inv[16];
    float handFwd[2][16];
    float handInv[2][16];
    float toRel[16];
    float camPrev[16];
    float handSphere[2][4];
    float beamA[2][4];
    float beamB[2][4];
};
struct Params
{
    EyeParams eye[2];
    float dstSize[4];
    float mvSize[4];
    float misc[4];
};
static_assert(sizeof(EyeParams) == 608, "EyeParams = clean_vr_spacewarp.comp's WarpEye (std140)");
static_assert(sizeof(Params) == 1264, "Params = clean_vr_spacewarp.comp's WarpParams (std140)");

constexpr uint32_t kBindings = 5;      // 0 params, 1 colour, 2 depth, 3 motion, 4 output
constexpr uint32_t kRetryFrames = 600; // after an allocation failure
// The controller's capsules (metres): the hand model lies within ~0.1 m of the grip (llvrhands.cpp:
// a 0.15 m body and a 0.06 m disc 0.07 m up the handle); the beam is 8 mm across (+ depth precision).
constexpr float kHandRadius = 0.13f;
constexpr float kBeamRadius = 0.015f;

glm::dmat4 toD(const float m[16])
{
    glm::dmat4 r;
    for (int c = 0; c < 4; ++c)
        for (int k = 0; k < 4; ++k) r[c][k] = (double)m[c * 4 + k];
    return r;
}
void toF(const glm::dmat4& m, float out[16])
{
    for (int c = 0; c < 4; ++c)
        for (int k = 0; k < 4; ++k) out[c * 4 + k] = (float)m[c][k];
}
bool finiteM(const glm::dmat4& m)
{
    for (int c = 0; c < 4; ++c)
        for (int k = 0; k < 4; ++k)
            if (!std::isfinite(m[c][k])) return false;
    return true;
}
// A controller's rigid pose (agent space): columns x, y, z, position.
glm::dmat4 handMatrix(const LLVRSpaceWarpVK::Hand& h)
{
    glm::dmat4 m(1.0);
    for (int a = 0; a < 3; ++a)
        m[a] = glm::dvec4(h.axis[a][0], h.axis[a][1], h.axis[a][2], 0.0);
    m[3] = glm::dvec4(h.pos[0], h.pos[1], h.pos[2], 1.0);
    return m;
}

void imgBarrier(VkImageMemoryBarrier2& b, VkImage img, VkImageAspectFlags aspect, uint32_t layers,
                VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    b = VkImageMemoryBarrier2{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, 0, 1, 0, layers};
}
void barriers(VkCommandBuffer cmd, const VkImageMemoryBarrier2* b, uint32_t n)
{
    if (!n) return;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = n;
    di.pImageMemoryBarriers = b;
    vkCmdPipelineBarrier2(cmd, &di);
}
} // namespace

bool LLVRSpaceWarpVK::ensurePipeline(VkDeviceContext& dev)
{
    VkDevice d = dev.device();
    if (mJob.pending())
    {
        if (!mJob.poll()) return false;   // still compiling
        mPipe = mStage;
        mStage = VK_NULL_HANDLE;
        if (!mPipe)
        {
            LL_WARNS("VR") << "space warp: the warp pipeline failed to build; space warp stays off" << LL_ENDL;
            mPipeFailed = true;
            return false;
        }
        LL_INFOS("VR") << "space warp: warp pipeline built" << LL_ENDL;
    }
    if (mPipe) return true;
    if (mPipeFailed) return false;

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(d, &sci, nullptr, &mLinear));
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    VK_CHECK(vkCreateSampler(d, &sci, nullptr, &mNearest));

    VkDescriptorSetLayoutBinding b[kBindings]{};
    for (uint32_t i = 0; i < kBindings; ++i)
    {
        b[i].binding = i;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        b[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                            : i == 4 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                     : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = kBindings;
    lci.pBindings = b;
    VK_CHECK(vkdCreateSetLayout(d, &lci, &mDsl, "mDslVrSpaceWarp"));
    VK_CHECK(mPool.create(d, "vrSpaceWarp", {{mDsl, 1}}));
    VKD_ALLOC_SOFT(mPool.allocate(mDsl, &mSet), {
        // (released with the rest at the session's end)
        LL_WARNS("VR") << "space warp: out of memory for its descriptor set; space warp stays off" << LL_ENDL;
        mSet = VK_NULL_HANDLE;
        mPipeFailed = true;
        return false;
    });
    mSetDirty = true;
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &mDsl;
    VK_CHECK(vkdCreatePipelineLayout(d, &pli, &mLayout, "mVrSpaceWarpLayout"));
    {
        PipelineBatchScope batchScope(d);   // queued; compiled in the background
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = kWorldVrSpaceWarpSpv_bytes;
        ci.pCode = kWorldVrSpaceWarpSpv;
        VkShaderModule m = VK_NULL_HANDLE;
        VK_CHECK(vkCreateShaderModule(d, &ci, nullptr, &m));
        vkNoteShaderModule(m, kWorldVrSpaceWarpSpv, kWorldVrSpaceWarpSpv_bytes, "kWorldVrSpaceWarpSpv");
        createComputePipelineInto(d, m, mLayout, 0, nullptr, &mStage, "VR space warp");
        destroyShaderModuleDeferred(d, m, nullptr);
        PipelineJobOpts o;
        o.what = "VR space warp";
        mJob.start(pipelineBatchFlushAsync(d, o));
    }
    LL_INFOS("VR") << "space warp: warp pipeline compiling in the background (the worlds render as usual until it lands)"
                   << LL_ENDL;
    return false;
}

void LLVRSpaceWarpVK::destroyImages(VkDeviceContext& dev)
{
    for (GpuImage* im : {&mColor, &mDepth, &mMotion, &mOut})
    {
        if (im->image) dev.destroyImage(*im);
        *im = GpuImage{};
    }
    mHistory = false;
    mSetDirty = true;
}

bool LLVRSpaceWarpVK::ensureImages(VkDeviceContext& dev, const CaptureIn& in)
{
    const VkExtent2D motionExt = in.motion ? in.motionExtent : VkExtent2D{1, 1};
    const VkFormat motionFmt = in.motion ? in.motionFormat : VK_FORMAT_R16G16B16A16_SFLOAT;
    auto same = [](const GpuImage& g, VkExtent2D e, VkFormat f) {
        return g.image && g.extent.width == e.width && g.extent.height == e.height && g.format == f;
    };
    if (same(mColor, in.colorExtent, in.colorFormat) && same(mDepth, in.depthExtent, in.depthFormat) &&
        same(mMotion, motionExt, motionFmt) && same(mOut, in.colorExtent, VK_FORMAT_R8G8B8A8_UNORM))
        return true;
    if (mImagesFailed && (int32_t)(mFrameNo - mRetryAt) < 0) return false;   // failed recently
    if (mColor.image || mDepth.image || mMotion.image || mOut.image)
    {
        vkDeviceWaitIdle(dev.device());   // a size change (rare): the set and the images may be in flight
        destroyImages(dev);
    }
    {
        VkMemScope memcat(VKMEM_TARGET);
        VkDeviceContext::SoftAllocScope soft(&dev);   // out of memory refuses the warp, never aborts
        const VkImageUsageFlags hist = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        mColor = dev.createScreenImage(in.colorExtent.width, in.colorExtent.height, kEyes, in.colorFormat, hist,
                                       VK_IMAGE_ASPECT_COLOR_BIT);
        if (mColor.image)
            mDepth = dev.createScreenImage(in.depthExtent.width, in.depthExtent.height, kEyes, in.depthFormat, hist,
                                           VK_IMAGE_ASPECT_DEPTH_BIT);
        if (mDepth.image)
            mMotion = dev.createScreenImage(motionExt.width, motionExt.height, kEyes, motionFmt, hist,
                                            VK_IMAGE_ASPECT_COLOR_BIT);
        if (mMotion.image)
            mOut = dev.createScreenImage(in.colorExtent.width, in.colorExtent.height, kEyes, VK_FORMAT_R8G8B8A8_UNORM,
                                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                         VK_IMAGE_ASPECT_COLOR_BIT);
    }
    if (!mColor.arrayView || !mDepth.arrayView || !mMotion.arrayView || !mOut.arrayView)
    {
        LL_WARNS("VR") << "space warp: could not allocate the warp history (" << in.colorExtent.width << "x"
                       << in.colorExtent.height << " x" << kEyes << "): space warp refused, retried in " << kRetryFrames
                       << " frames" << LL_ENDL;
        destroyImages(dev);
        mImagesFailed = true;
        mRetryAt = mFrameNo + kRetryFrames;
        return false;
    }
    mImagesFailed = false;
    dev.setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)mColor.image, "vrWarpHistColor");
    dev.setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)mDepth.image, "vrWarpHistDepth");
    dev.setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)mMotion.image, "vrWarpHistMotion");
    dev.setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)mOut.image, "vrWarpOut");
    const double mb = ((double)in.colorExtent.width * in.colorExtent.height * (4.0 + 4.0) +
                       (double)in.depthExtent.width * in.depthExtent.height * 4.0 +
                       (double)motionExt.width * motionExt.height * 8.0) * kEyes / (1024.0 * 1024.0);
    LL_INFOS("VR") << "space warp: history " << in.colorExtent.width << "x" << in.colorExtent.height << " colour, "
                   << in.depthExtent.width << "x" << in.depthExtent.height << " depth, " << motionExt.width << "x"
                   << motionExt.height << " motion, x" << kEyes << " eyes, + the output: ~" << (int)mb << " MB" << LL_ENDL;
    mSetDirty = true;
    return true;
}

void LLVRSpaceWarpVK::writeSet(VkDeviceContext& dev)
{
    if (!mSet || !mParams.buffer) return;
    const VkImageLayout RO = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorBufferInfo bi{mParams.buffer, 0, sizeof(Params)};
    const VkDescriptorImageInfo ii[kBindings] = {
        {},
        {mLinear, mColor.arrayView, RO},
        {mNearest, mDepth.arrayView, RO},
        {mNearest, mMotion.arrayView, RO},
        {VK_NULL_HANDLE, mOut.arrayView, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet wr[kBindings]{};
    for (uint32_t i = 0; i < kBindings; ++i)
    {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = mSet;
        wr[i].dstBinding = i;
        wr[i].descriptorCount = 1;
        if (i == 0)
        {
            wr[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            wr[i].pBufferInfo = &bi;
        }
        else
        {
            wr[i].descriptorType = i == 4 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wr[i].pImageInfo = &ii[i];
        }
    }
    vkUpdateDescriptorSets(dev.device(), kBindings, wr, 0, nullptr);
    mSetDirty = false;
}

bool LLVRSpaceWarpVK::capture(VkDeviceContext& dev, VkCommandBuffer cmd, const CaptureIn& in)
{
    ++mFrameNo;
    mHistory = false;
    if (!cmd || !in.color || !in.depth || in.layers < kEyes || !in.colorExtent.width || !in.colorExtent.height ||
        !in.depthExtent.width || !in.depthExtent.height || in.depthLayout == VK_IMAGE_LAYOUT_UNDEFINED)
        return false;
    // (a depth-only format: a combined depth / stencil image would need both aspects in its barriers)
    if (in.depthFormat != VK_FORMAT_D32_SFLOAT && in.depthFormat != VK_FORMAT_D16_UNORM &&
        in.depthFormat != VK_FORMAT_X8_D24_UNORM_PACK32)
    {
        if (!mLogged)
        {
            mLogged = true;
            LL_WARNS("VR") << "space warp: the world depth format (" << (int)in.depthFormat << ") is not supported" << LL_ENDL;
        }
        return false;
    }
    if (!ensurePipeline(dev)) return false;
    if (!mParams.buffer)
    {
        VkMemScope memcat(VKMEM_TARGET);
        VkDeviceContext::SoftAllocScope soft(&dev);
        mParams = dev.createBuffer(sizeof(Params), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!mParams.buffer) return false;
        mSetDirty = true;
    }
    if (!ensureImages(dev, in)) return false;

    const VkImageAspectFlags srcDepthAspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    const VkPipelineStageFlags2 DS = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    const bool motion = in.motion != VK_NULL_HANDLE && in.motionLayout != VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageMemoryBarrier2 b[6];
    uint32_t n = 0;
    // The history: the previous warp's reads (earlier in the queue) before these writes; discarded.
    imgBarrier(b[n++], mColor.image, VK_IMAGE_ASPECT_COLOR_BIT, kEyes, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    imgBarrier(b[n++], mDepth.image, VK_IMAGE_ASPECT_DEPTH_BIT, kEyes, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    imgBarrier(b[n++], mMotion.image, VK_IMAGE_ASPECT_COLOR_BIT, kEyes, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    // The world's depth and motion: their writers this frame before the copies' reads. (The colour
    // was moved to TRANSFER_SRC by the caller, for the eye copies.)
    imgBarrier(b[n++], in.depth, srcDepthAspect, kEyes, in.depthLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               DS | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    if (motion)
        imgBarrier(b[n++], in.motion, VK_IMAGE_ASPECT_COLOR_BIT, kEyes, in.motionLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    barriers(cmd, b, n);

    VkImageCopy rg{};
    rg.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, kEyes};
    rg.dstSubresource = rg.srcSubresource;
    rg.extent = {in.colorExtent.width, in.colorExtent.height, 1};
    vkCmdCopyImage(cmd, in.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mColor.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    rg.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, kEyes};
    rg.dstSubresource = rg.srcSubresource;
    rg.extent = {in.depthExtent.width, in.depthExtent.height, 1};
    vkCmdCopyImage(cmd, in.depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mDepth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    if (motion)
    {
        rg.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, kEyes};
        rg.dstSubresource = rg.srcSubresource;
        rg.extent = {in.motionExtent.width, in.motionExtent.height, 1};
        vkCmdCopyImage(cmd, in.motion, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mMotion.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &rg);
    }

    n = 0;
    imgBarrier(b[n++], in.depth, srcDepthAspect, kEyes, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, in.depthLayout,
               VK_PIPELINE_STAGE_2_COPY_BIT, 0,
               DS | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                   VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    if (motion)
        imgBarrier(b[n++], in.motion, VK_IMAGE_ASPECT_COLOR_BIT, kEyes, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, in.motionLayout,
                   VK_PIPELINE_STAGE_2_COPY_BIT, 0,
                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                       VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    for (GpuImage* h : {&mColor, &mDepth, &mMotion})
    {
        const VkImageAspectFlags a = h == &mDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        // (the motion history of a frame without motion: only made valid for sampling, never read)
        imgBarrier(b[n++], h->image, a, kEyes,
                   (h == &mMotion && !motion) ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    barriers(cmd, b, n);
    vkBreadcrumb(cmd, "vr_warp_capture");

    // The frame's data (no images).
    mSrc = in;
    mSrc.color = mSrc.depth = mSrc.motion = VK_NULL_HANDLE;
    mSrc.motionValid = motion && in.motionValid;
    mSrc.motionExtent = motion ? in.motionExtent : VkExtent2D{1, 1};
    mHistory = true;
    return true;
}

bool LLVRSpaceWarpVK::ensureCommands(VkDeviceContext& dev)
{
    if (mCmdPool) return true;
    VkDevice d = dev.device();
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = dev.queueFamily();
    if (vkCreateCommandPool(d, &pci, nullptr, &mCmdPool) != VK_SUCCESS)
    {
        mCmdPool = VK_NULL_HANDLE;
        return false;
    }
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = mCmdPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = kRing;
    bool ok = vkAllocateCommandBuffers(d, &ai, mCmd) == VK_SUCCESS;
    for (uint32_t i = 0; ok && i < kRing; ++i)
    {
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        ok = vkCreateFence(d, &fci, nullptr, &mFence[i]) == VK_SUCCESS;
        mSubmitted[i] = false;
    }
    // GPU timer (where the queue has timestamps): two per ring entry.
    uint32_t famCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev.physicalDevice(), &famCount, nullptr);
    std::vector<VkQueueFamilyProperties> fams(famCount);
    vkGetPhysicalDeviceQueueFamilyProperties(dev.physicalDevice(), &famCount, fams.data());
    if (ok && dev.queueFamily() < famCount && fams[dev.queueFamily()].timestampValidBits > 0)
    {
        VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = 2 * kRing;
        if (vkCreateQueryPool(d, &qci, nullptr, &mQueries) != VK_SUCCESS) mQueries = VK_NULL_HANDLE;
    }
    if (!ok)
    {
        LL_WARNS("VR") << "space warp: could not create its command buffers / fences: space warp refused" << LL_ENDL;
        for (VkFence& f : mFence)
            if (f) { vkDestroyFence(d, f, nullptr); f = VK_NULL_HANDLE; }
        vkDestroyCommandPool(d, mCmdPool, nullptr);
        mCmdPool = VK_NULL_HANDLE;
        for (VkCommandBuffer& c : mCmd) c = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

float LLVRSpaceWarpVK::takeGpuMs()
{
    const float ms = mGpuMs;
    mGpuMs = -1.f;
    return ms;
}

bool LLVRSpaceWarpVK::warpAndSubmit(VkDeviceContext& dev, VkQueue queue, const WarpIn& in,
                                    const std::function<void(VkCommandBuffer, VkImage, VkExtent2D, uint32_t)>& recordXr)
{
    if (!mHistory || !mPipe || !mSet || !mParams.buffer || !mOut.image || !ensureCommands(dev)) return false;
    VkDevice d = dev.device();
    const uint32_t r = mRingPos;
    if (mSubmitted[r])
    {
        // Three warps back: long finished. A fence that does not come back (a hang) refuses the warp;
        // the device-loss path raises it as any other wait would.
        const VkResult wr = vkWaitForFences(d, 1, &mFence[r], VK_TRUE, 2000000000ull);
        if (wr == VK_TIMEOUT)
        {
            LL_WARNS("VR") << "space warp: a warp submitted 3 frames ago has not finished: refused this frame" << LL_ENDL;
            return false;
        }
        VK_CHECK(wr);
        VK_CHECK(vkResetFences(d, 1, &mFence[r]));
        mSubmitted[r] = false;
        if (mQueries && mQueryWritten[r])
        {
            uint64_t ts[2] = {0, 0};
            if (vkGetQueryPoolResults(d, mQueries, 2 * r, 2, sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) ==
                    VK_SUCCESS &&
                ts[1] > ts[0])
                mGpuMs = (float)((double)(ts[1] - ts[0]) * dev.timestampPeriodNs() * 1e-6);
            mQueryWritten[r] = false;
        }
    }
    if (mSetDirty) writeSet(dev);

    // The per-eye maps, in double (agent coordinates reach kilometres).
    Params p{};
    for (uint32_t v = 0; v < kEyes; ++v)
    {
        const Eye& s = mSrc.eyes[v];
        const Eye& t = in.eyes[v];
        const glm::dmat4 invVs = glm::inverse(toD(s.view));
        const glm::dmat4 invPs = glm::inverse(toD(s.proj));
        const glm::dmat4 PtVt = toD(t.proj) * toD(t.view);
        const glm::dmat4 fwd = PtVt * invVs * invPs;
        glm::dmat4 rel(1.0);
        rel[3] = glm::dvec4(-(double)s.pos[0], -(double)s.pos[1], -(double)s.pos[2], 1.0);
        const glm::dmat4 toRel = rel * invVs * invPs;
        if (!finiteM(fwd) || !finiteM(toRel)) return false;
        EyeParams& e = p.eye[v];
        toF(fwd, e.fwd);
        toF(glm::inverse(fwd), e.inv);
        toF(toRel, e.toRel);
        std::memcpy(e.camPrev, mSrc.camPrev[v], sizeof(e.camPrev));
        for (uint32_t h = 0; h < 2; ++h)
        {
            const Hand& hs = mSrc.hands[h];
            const Hand& ht = in.hands[h];
            glm::dmat4 hf = fwd;
            if (hs.valid && ht.valid)
            {
                // The controller's own pose change: a point on it moves with it.
                const glm::dmat4 T = handMatrix(ht) * glm::inverse(handMatrix(hs));
                hf = PtVt * T * invVs * invPs;
                if (!finiteM(hf)) hf = fwd;
                e.handSphere[h][0] = hs.pos[0] - s.pos[0];
                e.handSphere[h][1] = hs.pos[1] - s.pos[1];
                e.handSphere[h][2] = hs.pos[2] - s.pos[2];
                e.handSphere[h][3] = kHandRadius;
                if (hs.beam)
                {
                    for (int k = 0; k < 3; ++k)
                    {
                        e.beamA[h][k] = hs.beamFrom[k] - s.pos[k];
                        e.beamB[h][k] = hs.beamTo[k] - s.pos[k];
                    }
                    e.beamA[h][3] = kBeamRadius;
                }
            }
            toF(hf, e.handFwd[h]);
            toF(glm::inverse(hf), e.handInv[h]);
        }
    }
    const float ow = (float)mOut.extent.width, oh = (float)mOut.extent.height;
    const float mw = (float)mSrc.motionExtent.width, mh = (float)mSrc.motionExtent.height;
    p.dstSize[0] = ow; p.dstSize[1] = oh; p.dstSize[2] = 1.f / ow; p.dstSize[3] = 1.f / oh;
    p.mvSize[0] = mw; p.mvSize[1] = mh; p.mvSize[2] = 1.f / mw; p.mvSize[3] = 1.f / mh;
    p.misc[0] = std::clamp(in.extrapolate, 0.f, 1.5f);
    p.misc[1] = mSrc.motionValid ? 1.f : 0.f;
    p.misc[2] = in.swapRB ? 1.f : 0.f;
    p.misc[3] = (float)std::min<uint32_t>(in.debug, 2u);

    VkCommandBuffer cmd = mCmd[r];
    VK_CHECK(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    vkBreadcrumb(cmd, "vr_warp");
    if (mQueries) vkCmdResetQueryPool(cmd, mQueries, 2 * r, 2);
    // (the previous warp's uniform reads, earlier in the queue, before this update)
    {
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        mb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &di);
    }
    vkCmdUpdateBuffer(cmd, mParams.buffer, 0, sizeof(Params), &p);
    {
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
        VkImageMemoryBarrier2 ib;
        // The output: the previous frame's eye copies read it (earlier in the queue); rewritten whole.
        imgBarrier(ib, mOut.image, VK_IMAGE_ASPECT_COLOR_BIT, kEyes, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                   VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &mb;
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &ib;
        vkCmdPipelineBarrier2(cmd, &di);
    }
    // (ALL_COMMANDS: the start once everything before it on the queue -- a real frame still running --
    // has finished, so the time is the warp's own)
    if (mQueries) vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mQueries, 2 * r);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout, 0, 1, &mSet, 0, nullptr);
    vkCmdDispatch(cmd, (mOut.extent.width + 15u) / 16u, (mOut.extent.height + 15u) / 16u, kEyes);
    if (mQueries)
    {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mQueries, 2 * r + 1);
        mQueryWritten[r] = true;
    }
    {
        VkImageMemoryBarrier2 ib;
        imgBarrier(ib, mOut.image, VK_IMAGE_ASPECT_COLOR_BIT, kEyes, VK_IMAGE_LAYOUT_GENERAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        barriers(cmd, &ib, 1);
    }
    if (recordXr) recordXr(cmd, mOut.image, mOut.extent, kEyes);
    vkBreadcrumb(cmd, "vr_warp_done");
    VK_CHECK(vkEndCommandBuffer(cmd));

    VkCommandBufferSubmitInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    ci.commandBuffer = cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &ci;
    const VkResult sr = vkQueueSubmit2(queue, 1, &si, mFence[r]);
    if (vkIsOom(sr))
    {
        LL_WARNS("VR") << "space warp: the warp's submit ran out of memory: refused this frame" << LL_ENDL;
        mQueryWritten[r] = false;
        return false;
    }
    VK_CHECK(sr);
    mSubmitted[r] = true;
    mRingPos = (r + 1u) % kRing;
    if (!mLogged)
    {
        mLogged = true;
        LL_INFOS("VR") << "space warp: first warp submitted (" << mOut.extent.width << "x" << mOut.extent.height << " x"
                       << kEyes << ", motion " << (mSrc.motionValid ? "carried forward" : "not available")
                       << ", extrapolation " << p.misc[0] << ")" << LL_ENDL;
    }
    return true;
}

void LLVRSpaceWarpVK::release(VkDeviceContext& dev)
{
    VkDevice d = dev.device();
    if (!d) return;
    mJob.wait();
    if (mStage) { vkDestroyPipeline(d, mStage, nullptr); mStage = VK_NULL_HANDLE; }
    const bool any = mPipe || mCmdPool || mColor.image || mOut.image || mParams.buffer || mDsl;
    if (!any) return;
    // (a lost device: no wait -- destroying its objects stays valid, and they must go before it does)
    if (!vkDeviceIsLost()) vkDeviceWaitIdle(d);
    destroyImages(dev);
    if (mParams.buffer) dev.destroyBuffer(mParams);
    mParams = GpuBuffer{};
    if (mPipe) vkDestroyPipeline(d, mPipe, nullptr);
    if (mLayout) vkDestroyPipelineLayout(d, mLayout, nullptr);
    mPool.destroy();
    if (mDsl) vkdDestroySetLayout(d, mDsl);
    if (mLinear) vkDestroySampler(d, mLinear, nullptr);
    if (mNearest) vkDestroySampler(d, mNearest, nullptr);
    for (uint32_t i = 0; i < kRing; ++i)
    {
        if (mFence[i]) vkDestroyFence(d, mFence[i], nullptr);
        mFence[i] = VK_NULL_HANDLE;
        mSubmitted[i] = mQueryWritten[i] = false;
        mCmd[i] = VK_NULL_HANDLE;
    }
    if (mCmdPool) vkDestroyCommandPool(d, mCmdPool, nullptr);
    if (mQueries) vkDestroyQueryPool(d, mQueries, nullptr);
    mCmdPool = VK_NULL_HANDLE;
    mQueries = VK_NULL_HANDLE;
    mPipe = VK_NULL_HANDLE;
    mLayout = VK_NULL_HANDLE;
    mDsl = VK_NULL_HANDLE;
    mSet = VK_NULL_HANDLE;
    mLinear = mNearest = VK_NULL_HANDLE;
    mPipeFailed = mImagesFailed = false;
    mSetDirty = true;
    mRingPos = 0;
    mGpuMs = -1.f;
    mLogged = false;
}
