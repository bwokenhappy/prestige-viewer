/**
 * @file llcleanfsr3vk.cpp
 * @brief AMD FidelityFX Super Resolution 3.1 upscaler host side: see llcleanfsr3vk.h.
 *
 * The pass sequence, resources, clears, constants and dispatch sizes follow
 * FidelityFX SDK v1.1.4 sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp
 * (fsr3upscalerCreate / fsr3upscalerDispatch; MIT, Copyright (C) 2024 Advanced Micro
 * Devices, Inc. -- indra/vkclean/third_party/fidelityfx/LICENSE.txt).
 */
#include "linden_common.h"

#include "llcleanfsr3vk.h"
#include "llvkframegen.h"   // #1544: the main view's camera for frame generation
#include "llvkframegenfsrmath.h"   // llfsr::deviceToViewDepth
#include "vk_pipeline.h"
#include "vk_fsr3_shaders_spv.h"

#include "llerror.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

namespace
{
// cbFSR3Upscaler (ffx_fsr3upscaler_callbacks_glsl.h; the SDK's Fsr3UpscalerConstants), std140.
struct FsrConstants
{
    int32_t renderSize[2];
    int32_t previousFrameRenderSize[2];
    int32_t upscaleSize[2];
    int32_t previousFrameUpscaleSize[2];
    int32_t maxRenderSize[2];
    int32_t maxUpscaleSize[2];
    float   deviceToViewDepth[4];
    float   jitterOffset[2];
    float   previousFrameJitterOffset[2];
    float   motionVectorScale[2];
    float   downscaleFactor[2];
    float   motionVectorJitterCancellation[2];
    float   tanHalfFOV;
    float   jitterPhaseCount;
    float   deltaTime;
    float   deltaPreExposure;
    float   viewSpaceToMetersFactor;
    float   frameIndex;
    float   velocityFactor;
    float   reactivenessScale;
    float   shadingChangeScale;
    float   accumulationAddedPerFrame;
    float   minDisocclusionAccumulation;
};
static_assert(sizeof(FsrConstants) == 148, "FsrConstants must match cbFSR3Upscaler (std140)");

struct SpdConstants   // cbSPD
{
    uint32_t mips;
    uint32_t numWorkGroups;
    uint32_t workGroupOffset[2];
    uint32_t renderSize[2];
};

struct RcasConstants { uint32_t rcasConfig[4]; };   // cbRCAS

// clean_fsr_inputs.comp FsrInputs (std140, 144 B).
struct InputsParams
{
    float reproj[16];
    float rscreen[4];
    float jitter[4];
    float wplane[4];
    float p0[4];
    float p1[4];
};
static_assert(sizeof(InputsParams) == 144, "InputsParams must match clean_fsr_inputs.comp");
static_assert(sizeof(FsrConstants) <= 256 && sizeof(InputsParams) <= 256, "constant-buffer sub-slots are 256 B");

constexpr VkDescriptorType S  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
constexpr VkDescriptorType U  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
constexpr VkDescriptorType C  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
constexpr VkDescriptorType CI = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

// Each pass's set 0: the bindings its .glsl entry declares (ffx_fsr3upscaler_*_pass.glsl;
// the Lanczos LUT is left out: REPROJECT_USE_LANCZOS_TYPE=0 does not read it), plus the
// two samplers at 1000 / 1001 (immutable) on the SDK passes.
struct Bind { uint32_t binding; VkDescriptorType type; };
struct PassDesc { const char* name; Bind binds[16]; uint32_t count; bool samplers; };
const PassDesc kPasses[] = {
    {"fsrInputs",             {{0,CI},{1,CI},{2,CI},{3,CI},{4,CI},{5,U},{6,U},{7,U},{8,U},{9,U},{10,U},{11,C},{12,CI}}, 13, false},
    {"fsrPrepareInputs",      {{0,S},{1,S},{2,S},{3,U},{4,U},{5,U},{6,U},{7,U},{8,C}}, 9, true},
    {"fsrLumaPyramid",        {{0,S},{1,S},{2,U},{3,U},{4,U},{5,U},{6,U},{7,U},{8,U},{9,U},{10,U},{11,C},{12,C}}, 13, true},
    {"fsrShadingPyramid",     {{0,S},{1,S},{2,S},{3,S},{4,U},{5,U},{6,U},{7,U},{8,U},{9,U},{10,U},{11,C},{12,C}}, 13, true},
    {"fsrShadingChange",      {{0,S},{1,U},{2,C}}, 3, true},
    {"fsrPrepareReactivity",  {{0,S},{1,S},{2,S},{3,S},{4,S},{5,S},{6,S},{7,S},{8,S},{9,U},{10,U},{11,U},{12,C}}, 13, true},
    {"fsrLumaInstability",    {{0,S},{1,S},{2,S},{3,S},{4,S},{5,S},{6,S},{7,U},{8,U},{9,C}}, 10, true},
    {"fsrAccumulate",         {{0,S},{1,S},{2,S},{3,S},{5,S},{6,S},{7,S},{8,S},{9,U},{10,U},{11,U},{12,C}}, 12, true},
    {"fsrRcas",               {{0,S},{1,S},{2,U},{3,C},{4,C}}, 5, true},
    {"fsrDebugView",          {{0,S},{1,S},{2,S},{3,S},{4,S},{5,U},{6,C}}, 7, true},
};
static_assert(sizeof(kPasses) / sizeof(kPasses[0]) == 10, "one PassDesc per LLCleanFSR3VK::Pass");

VkShaderModule fsrModule(VkDevice dev, const uint32_t* code, size_t bytes)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
    return m;
}

// ffxSpdSetup (ffx_spd.h, FFX_CPU) for the rect {0, 0, w, h}, mips = -1.
void spdSetup(uint32_t w, uint32_t h, uint32_t groups[2], uint32_t offset[2], uint32_t& numGroups, uint32_t& mips)
{
    offset[0] = offset[1] = 0;
    const uint32_t endX = (w - 1) / 64, endY = (h - 1) / 64;
    groups[0] = endX + 1;
    groups[1] = endY + 1;
    numGroups = groups[0] * groups[1];
    mips = (uint32_t)std::min(std::floor(std::log2((float)std::max(w, h))), 12.f);
}

// FsrRcasCon (ffx_fsr1.h): sharpness in stops (0 = maximum).
void rcasCon(uint32_t con[4], float stops)
{
    const float s = std::exp2(-stops);
    uint32_t bits;
    std::memcpy(&bits, &s, 4);
    // ffxPackHalf2x16(s, s): IEEE half, round to nearest (s is in (0, 1]).
    uint32_t h;
    {
        const uint32_t sign = (bits >> 16) & 0x8000u;
        const int32_t  e = (int32_t)((bits >> 23) & 0xffu) - 127 + 15;
        uint32_t m = bits & 0x7fffffu;
        if (e <= 0) h = sign;   // (not reached: s >= 0.25)
        else if (e >= 31) h = sign | 0x7c00u;
        else
        {
            h = sign | ((uint32_t)e << 10) | (m >> 13);
            if (m & 0x1000u) ++h;   // round half up
        }
    }
    con[0] = bits;
    con[1] = h | (h << 16);
    con[2] = 0;
    con[3] = 0;
}

bool storageFormatOk(VkPhysicalDevice pd, VkFormat f, bool sampled)
{
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(pd, f, &fp);
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    if (sampled) need |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    return (fp.optimalTilingFeatures & need) == need;
}
} // namespace

bool LLCleanFSR3VK::deviceSupported(VkDeviceContext* ctx)
{
    static int s_logged = -1;
    if (!ctx) return false;
    const char* why = nullptr;
    if (!ctx->storageImageExtendedFormats())
        why = "shaderStorageImageExtendedFormats is not available";
    else
    {
        const VkFormat fmts[] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT,
                                 VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT, VK_FORMAT_R8G8B8A8_UNORM,
                                 VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT};
        for (VkFormat f : fmts)
            if (!storageFormatOk(ctx->physicalDevice(), f, true)) { why = "a required storage image format is missing"; break; }
        // The passes sample these through s_LinearClamp (R32F linear filtering is optional
        // in Vulkan: dilated depth).
        const VkFormat lin[] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT,
                                VK_FORMAT_R32_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT};
        for (VkFormat f : lin)
        {
            if (why) break;
            VkFormatProperties fp{};
            vkGetPhysicalDeviceFormatProperties(ctx->physicalDevice(), f, &fp);
            if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
                why = "a required format is not linearly filterable";
        }
    }
    const int ok = why ? 0 : 1;
    if (s_logged != ok)
    {
        s_logged = ok;
        if (why) LL_WARNS("Vulkan") << "FSR 3.1: unavailable on this device (" << why << "); the upscaler runs as off (native, no temporal anti-aliasing)" << LL_ENDL;
        else     LL_INFOS("Vulkan") << "FSR 3.1: supported (FidelityFX SDK 1.1.4 / FSR 3.1.4 upscaler)" << LL_ENDL;
    }
    return ok != 0;
}

bool LLCleanFSR3VK::ensurePipelines(VkDeviceContext* ctx)
{
    if (mPipe[P_ACCUMULATE]) return true;
    mCtx = ctx;
    VkDevice dev = ctx->device();

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    VK_CHECK(vkCreateSampler(dev, &sci, nullptr, &mPointClamp));
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    VK_CHECK(vkCreateSampler(dev, &sci, nullptr, &mLinearClamp));

    // SPD: subgroup quad operations in compute, else the LDS-only permutation.
    {
        VkPhysicalDeviceSubgroupProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &sg;
        vkGetPhysicalDeviceProperties2(ctx->physicalDevice(), &p2);
        mWaveOps = (sg.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT)
                && (sg.supportedOperations & VK_SUBGROUP_FEATURE_QUAD_BIT)
                && (sg.supportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT);
    }

    std::vector<VkdPoolUse> uses;
    for (uint32_t p = 0; p < P_COUNT; ++p)
    {
        const PassDesc& pd = kPasses[p];
        VkDescriptorSetLayoutBinding b[18]{};
        uint32_t n = 0;
        for (uint32_t i = 0; i < pd.count; ++i, ++n)
        {
            b[n].binding = pd.binds[i].binding; b[n].descriptorType = pd.binds[i].type;
            b[n].descriptorCount = 1; b[n].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        if (pd.samplers)
        {
            b[n].binding = 1000; b[n].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            b[n].descriptorCount = 1; b[n].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[n].pImmutableSamplers = &mPointClamp; ++n;
            b[n].binding = 1001; b[n].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            b[n].descriptorCount = 1; b[n].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[n].pImmutableSamplers = &mLinearClamp; ++n;
        }
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = n; lci.pBindings = b;
        VK_CHECK(vkdCreateSetLayout(dev, &lci, &mDsl[p], pd.name));
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1; pli.pSetLayouts = &mDsl[p];
        VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mLayout[p], pd.name));
        uses.push_back({mDsl[p], 2});
    }
    VK_CHECK(mPool.create(dev, "fsr3", uses));
    for (uint32_t p = 0; p < P_COUNT; ++p)
        for (int i = 0; i < 2; ++i)
            VKD_ALLOC_SOFT(mPool.allocate(mDsl[p], &mSet[p][i]), { destroy(dev); return false; });

    auto make = [&](Pass p, const uint32_t* code, size_t bytes) -> VkPipeline
    {
        VkShaderModule m = fsrModule(dev, code, bytes);
        VkPipeline pipe = createComputePipeline(dev, m, mLayout[p]);
        vkDestroyShaderModule(dev, m, nullptr);
        return pipe;
    };
    mPipe[P_INPUTS]             = make(P_INPUTS, kFsrInputsSpv, kFsrInputsSpv_bytes);
    mPipe[P_PREPARE_INPUTS]     = make(P_PREPARE_INPUTS, kFsrPrepareInputsSpv, kFsrPrepareInputsSpv_bytes);
    mPipe[P_LUMA_PYRAMID]       = mWaveOps ? make(P_LUMA_PYRAMID, kFsrLumaPyramidSpv, kFsrLumaPyramidSpv_bytes)
                                           : make(P_LUMA_PYRAMID, kFsrLumaPyramidNoWaveSpv, kFsrLumaPyramidNoWaveSpv_bytes);
    mPipe[P_SHADING_PYRAMID]    = mWaveOps ? make(P_SHADING_PYRAMID, kFsrShadingPyramidSpv, kFsrShadingPyramidSpv_bytes)
                                           : make(P_SHADING_PYRAMID, kFsrShadingPyramidNoWaveSpv, kFsrShadingPyramidNoWaveSpv_bytes);
    mPipe[P_SHADING_CHANGE]     = make(P_SHADING_CHANGE, kFsrShadingChangeSpv, kFsrShadingChangeSpv_bytes);
    mPipe[P_PREPARE_REACTIVITY] = make(P_PREPARE_REACTIVITY, kFsrPrepareReactivitySpv, kFsrPrepareReactivitySpv_bytes);
    mPipe[P_LUMA_INSTABILITY]   = make(P_LUMA_INSTABILITY, kFsrLumaInstabilitySpv, kFsrLumaInstabilitySpv_bytes);
    mPipe[P_ACCUMULATE]         = make(P_ACCUMULATE, kFsrAccumulateSpv, kFsrAccumulateSpv_bytes);
    mAccumulateSharpen          = make(P_ACCUMULATE, kFsrAccumulateSharpenSpv, kFsrAccumulateSharpenSpv_bytes);
    mPipe[P_RCAS]               = make(P_RCAS, kFsrRcasSpv, kFsrRcasSpv_bytes);
    mPipe[P_DEBUG]              = make(P_DEBUG, kFsrDebugViewSpv, kFsrDebugViewSpv_bytes);
    for (uint32_t p = 0; p < P_COUNT; ++p)
        if (!mPipe[p]) { LL_WARNS("Vulkan") << "FSR 3.1: pipeline " << kPasses[p].name << " failed" << LL_ENDL; return false; }
    if (!mAccumulateSharpen) return false;
    LL_INFOS("Vulkan") << "FSR 3.1: pipelines ready (SPD " << (mWaveOps ? "subgroup quad" : "LDS-only") << ")" << LL_ENDL;
    return true;
}

bool LLCleanFSR3VK::ensureCb(uint32_t slotCount)
{
    VkMemScope memcat(VKMEM_UPSCALER);   // #407
    slotCount = std::max(slotCount, 1u);
    if (mCb.buffer && mCbSlots >= slotCount) return true;
    if (mCb.buffer)
    {
        vkDeviceWaitIdle(mCtx->device());   // the sets point at it
        mCtx->destroyBuffer(mCb);
    }
    mCb = mCtx->createBuffer((VkDeviceSize)kCbSlotBytes * slotCount, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    mCbSlots = mCb.buffer ? slotCount : 0;
    std::memset(mBound, 0, sizeof(mBound));   // rewrite the sets (new buffer)
    mBoundGen = 0;
    return mCb.buffer && mCb.mapped;
}

void LLCleanFSR3VK::destroyResources()
{
    if (!mCtx) return;
    VkDevice dev = mCtx->device();
    for (VkImageView& v : mSpdMipView)
        if (v) { vkDestroyImageView(dev, v, nullptr); v = VK_NULL_HANDLE; }
    for (GpuImage* im : {&mInMotion, &mInDepth, &mInReactive, &mInTc, &mInExposure, &mDilMotion, &mDilDepth,
                         &mReconDepth, &mIntermediate, &mLuma[0], &mLuma[1], &mLumaHist[0], &mLumaHist[1],
                         &mAccum[0], &mAccum[1], &mSpdAtomic, &mFrameInfo, &mSpdMips, &mFarthestMip1,
                         &mShading, &mDilReactive, &mNewLocks, &mUpscaled[0], &mUpscaled[1], &mStage})
        if (im->image) { mCtx->destroyImage(*im); *im = GpuImage{}; }
    mRw = mRh = mDw = mDh = 0;
    mSpdMipCount = 0;
    std::memset(mBound, 0, sizeof(mBound));
    mBoundGen = 0;
}

bool LLCleanFSR3VK::ensureResources(VkCommandBuffer cmd, const Frame& f)
{
    VkMemScope memcat(VKMEM_UPSCALER);   // #407
    const bool needStage = !f.outView;
    if (mRw == f.rw && mRh == f.rh && mDw == f.dw && mDh == f.dh && (!needStage || mStage.image))
        return true;
    vkDeviceWaitIdle(mCtx->device());   // the sets of frames in flight still point at them
    destroyResources();

    const VkImageUsageFlags rwUse = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    bool ok = true;
    std::vector<VkImage> fresh;
    auto mk = [&](GpuImage& im, uint32_t w, uint32_t h, VkFormat fmt, const char* name, uint32_t mips = 1,
                  VkImageUsageFlags extra = 0)
    {
        im = mCtx->createImage2D(std::max(w, 1u), std::max(h, 1u), fmt, rwUse | extra, VK_IMAGE_ASPECT_COLOR_BIT, mips);
        if (!im.image) { ok = false; return; }
        mCtx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)im.image, name);
        fresh.push_back(im.image);
    };
    const uint32_t hw = std::max(f.rw / 2, 1u), hh = std::max(f.rh / 2, 1u);
    mSpdMipCount = (uint32_t)std::floor(std::log2((float)std::max(hw, hh))) + 1u;
    // (TRANSFER_SRC: frame generation copies the two into its present slot after the upscale, as DLSS's)
    mk(mInMotion,     f.rw, f.rh, VK_FORMAT_R16G16_SFLOAT,       "fsrInputMotion", 1, VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    mk(mInDepth,      f.rw, f.rh, VK_FORMAT_R32_SFLOAT,          "fsrInputDepth", 1, VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    mk(mInReactive,   f.rw, f.rh, VK_FORMAT_R8_UNORM,            "fsrInputReactive");
    mk(mInTc,         f.rw, f.rh, VK_FORMAT_R8_UNORM,            "fsrInputTransparencyComposition");
    mk(mInExposure,   1, 1,       VK_FORMAT_R32_SFLOAT,          "fsrInputExposure");
    mk(mDilMotion,    f.rw, f.rh, VK_FORMAT_R16G16_SFLOAT,       "fsrDilatedMotion");
    mk(mDilDepth,     f.rw, f.rh, VK_FORMAT_R32_SFLOAT,          "fsrDilatedDepth");
    mk(mReconDepth,   f.rw, f.rh, VK_FORMAT_R32_UINT,            "fsrReconstructedPrevDepth");
    mk(mIntermediate, f.rw, f.rh, VK_FORMAT_R16_SFLOAT,          "fsrIntermediateFp16");
    mk(mLuma[0],      f.rw, f.rh, VK_FORMAT_R16_SFLOAT,          "fsrLuma1");
    mk(mLuma[1],      f.rw, f.rh, VK_FORMAT_R16_SFLOAT,          "fsrLuma2");
    mk(mLumaHist[0],  f.rw, f.rh, VK_FORMAT_R16G16B16A16_SFLOAT, "fsrLumaHistory1");
    mk(mLumaHist[1],  f.rw, f.rh, VK_FORMAT_R16G16B16A16_SFLOAT, "fsrLumaHistory2");
    mk(mAccum[0],     f.rw, f.rh, VK_FORMAT_R8_UNORM,            "fsrAccumulation1");
    mk(mAccum[1],     f.rw, f.rh, VK_FORMAT_R8_UNORM,            "fsrAccumulation2");
    mk(mSpdAtomic,    1, 1,       VK_FORMAT_R32_UINT,            "fsrSpdAtomic");
    mk(mFrameInfo,    1, 1,       VK_FORMAT_R32G32B32A32_SFLOAT, "fsrFrameInfo");
    mk(mSpdMips,      hw, hh,     VK_FORMAT_R16G16_SFLOAT,       "fsrSpdMips", mSpdMipCount);
    mk(mFarthestMip1, hw, hh,     VK_FORMAT_R16_SFLOAT,          "fsrFarthestDepthMip1");
    mk(mShading,      hw, hh,     VK_FORMAT_R8_UNORM,            "fsrShadingChange");
    mk(mDilReactive,  f.rw, f.rh, VK_FORMAT_R8G8B8A8_UNORM,      "fsrDilatedReactiveMasks");
    mk(mNewLocks,     f.dw, f.dh, VK_FORMAT_R8_UNORM,            "fsrNewLocks");
    mk(mUpscaled[0],  f.dw, f.dh, VK_FORMAT_R16G16B16A16_SFLOAT, "fsrInternalUpscaled1");
    mk(mUpscaled[1],  f.dw, f.dh, VK_FORMAT_R16G16B16A16_SFLOAT, "fsrInternalUpscaled2");
    if (needStage)
        mk(mStage,    f.dw, f.dh, VK_FORMAT_R16G16B16A16_SFLOAT, "fsrOutput", 1, VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (ok)
        for (uint32_t m = 0; m < 6; ++m)
        {
            mSpdMipView[m] = mCtx->createView(mSpdMips.image, VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_VIEW_TYPE_2D,
                                              VK_IMAGE_ASPECT_COLOR_BIT, std::min(m, mSpdMipCount - 1), 1, 0, 1);
            if (!mSpdMipView[m]) ok = false;
        }
    if (!ok)
    {
        LL_WARNS("Vulkan") << "FSR 3.1: resource allocation failed at " << f.rw << "x" << f.rh << " -> "
                           << f.dw << "x" << f.dh << LL_ENDL;
        destroyResources();
        return false;
    }
    // Everything lives in GENERAL for good (storage + sampled + clears).
    std::vector<VkImageMemoryBarrier2> b(fresh.size());
    for (size_t i = 0; i < fresh.size(); ++i)
    {
        b[i] = VkImageMemoryBarrier2{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b[i].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        b[i].dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b[i].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                           | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b[i].image = fresh[i];
        b[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1};
    }
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = (uint32_t)b.size(); dep.pImageMemoryBarriers = b.data();
    vkCmdPipelineBarrier2(cmd, &dep);
    mRw = f.rw; mRh = f.rh; mDw = f.dw; mDh = f.dh;
    mFirst = true;
    LL_INFOS("Vulkan") << "FSR 3.1: resources " << f.rw << "x" << f.rh << " -> " << f.dw << "x" << f.dh
                       << " (SPD mips " << mSpdMipCount << (needStage ? ", staged output" : "") << ")" << LL_ENDL;
    return true;
}

// Both parities of every pass's set, from the current resources and the external views.
// Callers waited idle (or the sets are fresh).
void LLCleanFSR3VK::writeSets(const Frame& f, VkImageView outView)
{
    VkDevice dev = mCtx->device();
    std::vector<VkWriteDescriptorSet> w;
    std::vector<VkDescriptorImageInfo> ii;
    std::vector<VkDescriptorBufferInfo> bi;
    ii.reserve(256); bi.reserve(64);   // stable addresses (no reallocation below)
    const VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL, RO = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    for (uint32_t parity = 0; parity < 2; ++parity)
    {
        // The SDK's isOddFrame selection (fsr3upscalerDispatch).
        const bool odd = parity != 0;
        const GpuImage& accS = mAccum[odd ? 1 : 0];    const GpuImage& accU = mAccum[odd ? 0 : 1];
        const GpuImage& upS  = mUpscaled[odd ? 1 : 0]; const GpuImage& upU  = mUpscaled[odd ? 0 : 1];
        const GpuImage& lhS  = mLumaHist[odd ? 1 : 0]; const GpuImage& lhU  = mLumaHist[odd ? 0 : 1];
        const GpuImage& cur  = mLuma[odd ? 1 : 0];     const GpuImage& prev = mLuma[odd ? 0 : 1];
        auto img = [&](Pass p, uint32_t binding, VkImageView v, VkImageLayout l, VkDescriptorType t,
                       VkSampler s = VK_NULL_HANDLE)
        {
            ii.push_back({s, v, l});
            VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            x.dstSet = mSet[p][parity]; x.dstBinding = binding; x.descriptorCount = 1;
            x.descriptorType = t; x.pImageInfo = &ii.back();
            w.push_back(x);
        };
        auto s  = [&](Pass p, uint32_t b, const GpuImage& im) { img(p, b, im.view, G, S); };
        auto u  = [&](Pass p, uint32_t b, const GpuImage& im) { img(p, b, im.view, G, U); };
        // (Every sub-slot is 256 B: bound whole, so the range covers the std140 block's
        // rounded size, e.g. 160 B for the 148 B cbFSR3Upscaler.)
        auto cb = [&](Pass p, uint32_t b, uint32_t off, uint32_t /*bytes*/)
        {
            bi.push_back({mCb.buffer, off, 256});
            VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            x.dstSet = mSet[p][parity]; x.dstBinding = b; x.descriptorCount = 1;
            x.descriptorType = C; x.pBufferInfo = &bi.back();
            w.push_back(x);
        };
        const uint32_t fsrCb = (uint32_t)sizeof(FsrConstants);
        // 0. inputs (ours)
        img(P_INPUTS, 0, f.depthView,  VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, CI, f.nearestSampler);
        img(P_INPUTS, 1, f.motionView, RO, CI, f.nearestSampler);
        img(P_INPUTS, 2, f.reactView,  RO, CI, f.nearestSampler);
        img(P_INPUTS, 3, f.colorView,  RO, CI, f.nearestSampler);
        img(P_INPUTS, 4, f.refView,    RO, CI, f.nearestSampler);
        img(P_INPUTS, 12, f.noTemporalView, RO, CI, f.nearestSampler);   // #605
        img(P_INPUTS, 5, f.exposureView, G, U);
        u(P_INPUTS, 6, mInMotion); u(P_INPUTS, 7, mInDepth); u(P_INPUTS, 8, mInReactive);
        u(P_INPUTS, 9, mInTc); u(P_INPUTS, 10, mInExposure);
        cb(P_INPUTS, 11, kCbInputs, (uint32_t)sizeof(InputsParams));
        // 1. prepare inputs
        s(P_PREPARE_INPUTS, 0, mInMotion); s(P_PREPARE_INPUTS, 1, mInDepth);
        img(P_PREPARE_INPUTS, 2, f.colorView, RO, S);
        u(P_PREPARE_INPUTS, 3, mDilMotion); u(P_PREPARE_INPUTS, 4, mDilDepth); u(P_PREPARE_INPUTS, 5, mReconDepth);
        u(P_PREPARE_INPUTS, 6, mIntermediate); u(P_PREPARE_INPUTS, 7, cur);
        cb(P_PREPARE_INPUTS, 8, kCbFsr, fsrCb);
        // 2. luma pyramid
        s(P_LUMA_PYRAMID, 0, cur); s(P_LUMA_PYRAMID, 1, mIntermediate);
        u(P_LUMA_PYRAMID, 2, mSpdAtomic); u(P_LUMA_PYRAMID, 3, mFrameInfo);
        for (uint32_t m = 0; m < 6; ++m) img(P_LUMA_PYRAMID, 4 + m, mSpdMipView[m], G, U);
        u(P_LUMA_PYRAMID, 10, mFarthestMip1);
        cb(P_LUMA_PYRAMID, 11, kCbFsr, fsrCb); cb(P_LUMA_PYRAMID, 12, kCbSpd, (uint32_t)sizeof(SpdConstants));
        // 3. shading change pyramid
        s(P_SHADING_PYRAMID, 0, cur); s(P_SHADING_PYRAMID, 1, prev);
        s(P_SHADING_PYRAMID, 2, mDilMotion); s(P_SHADING_PYRAMID, 3, mInExposure);
        u(P_SHADING_PYRAMID, 4, mSpdAtomic);
        for (uint32_t m = 0; m < 6; ++m) img(P_SHADING_PYRAMID, 5 + m, mSpdMipView[m], G, U);
        cb(P_SHADING_PYRAMID, 11, kCbFsr, fsrCb); cb(P_SHADING_PYRAMID, 12, kCbSpd, (uint32_t)sizeof(SpdConstants));
        // 4. shading change
        s(P_SHADING_CHANGE, 0, mSpdMips); u(P_SHADING_CHANGE, 1, mShading);
        cb(P_SHADING_CHANGE, 2, kCbFsr, fsrCb);
        // 5. prepare reactivity
        s(P_PREPARE_REACTIVITY, 0, mReconDepth); s(P_PREPARE_REACTIVITY, 1, mDilMotion);
        s(P_PREPARE_REACTIVITY, 2, mDilDepth); s(P_PREPARE_REACTIVITY, 3, mInReactive);
        s(P_PREPARE_REACTIVITY, 4, mInTc); s(P_PREPARE_REACTIVITY, 5, accS);
        s(P_PREPARE_REACTIVITY, 6, mShading); s(P_PREPARE_REACTIVITY, 7, cur);
        s(P_PREPARE_REACTIVITY, 8, mInExposure);
        u(P_PREPARE_REACTIVITY, 9, mDilReactive); u(P_PREPARE_REACTIVITY, 10, mNewLocks);
        u(P_PREPARE_REACTIVITY, 11, accU);
        cb(P_PREPARE_REACTIVITY, 12, kCbFsr, fsrCb);
        // 6. luma instability
        s(P_LUMA_INSTABILITY, 0, mInExposure); s(P_LUMA_INSTABILITY, 1, mDilReactive);
        s(P_LUMA_INSTABILITY, 2, mDilMotion); s(P_LUMA_INSTABILITY, 3, mFrameInfo);
        s(P_LUMA_INSTABILITY, 4, lhS); s(P_LUMA_INSTABILITY, 5, mFarthestMip1);
        s(P_LUMA_INSTABILITY, 6, cur);
        u(P_LUMA_INSTABILITY, 7, lhU); u(P_LUMA_INSTABILITY, 8, mIntermediate);
        cb(P_LUMA_INSTABILITY, 9, kCbFsr, fsrCb);
        // 7. accumulate (both permutations)
        s(P_ACCUMULATE, 0, mInExposure); s(P_ACCUMULATE, 1, mDilReactive); s(P_ACCUMULATE, 2, mDilMotion);
        s(P_ACCUMULATE, 3, upS); s(P_ACCUMULATE, 5, mFarthestMip1); s(P_ACCUMULATE, 6, cur);
        s(P_ACCUMULATE, 7, mIntermediate);
        img(P_ACCUMULATE, 8, f.colorView, RO, S);
        u(P_ACCUMULATE, 9, upU);
        img(P_ACCUMULATE, 10, outView, G, U);
        u(P_ACCUMULATE, 11, mNewLocks);
        cb(P_ACCUMULATE, 12, kCbFsr, fsrCb);
        // 8. RCAS (reads this frame's internal upscaled colour)
        s(P_RCAS, 0, mInExposure); s(P_RCAS, 1, upU);
        img(P_RCAS, 2, outView, G, U);
        cb(P_RCAS, 3, kCbFsr, fsrCb); cb(P_RCAS, 4, kCbRcas, (uint32_t)sizeof(RcasConstants));
        // 9. debug view
        s(P_DEBUG, 0, mDilReactive); s(P_DEBUG, 1, mDilMotion); s(P_DEBUG, 2, mDilDepth);
        s(P_DEBUG, 3, upU); s(P_DEBUG, 4, mInExposure);
        img(P_DEBUG, 5, outView, G, U);
        cb(P_DEBUG, 6, kCbFsr, fsrCb);
    }
    vkUpdateDescriptorSets(dev, (uint32_t)w.size(), w.data(), 0, nullptr);
}

void LLCleanFSR3VK::clearImage(VkCommandBuffer cmd, VkImage img, const float rgba[4], uint32_t mips)
{
    VkClearColorValue c{};
    std::memcpy(c.float32, rgba, sizeof(c.float32));   // (0 = all-zero bits for the UINT images too)
    const VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
    vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &r);
}

// Between passes: every compute write so far -> the next pass's reads and writes (and
// fromClear: the clears too). Execution order also covers the WAR / WAW on the aliased
// and ping-pong images.
void LLCleanFSR3VK::barrier(VkCommandBuffer cmd, bool fromClear)
{
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | (fromClear ? VK_PIPELINE_STAGE_2_CLEAR_BIT : 0);
    m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | (fromClear ? VK_ACCESS_2_TRANSFER_WRITE_BIT : 0);
    m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    m.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT
                    | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_UNIFORM_READ_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1; dep.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void LLCleanFSR3VK::dispatch(VkCommandBuffer cmd, Pass p, VkPipeline pipe, uint32_t parity, uint32_t gx, uint32_t gy)
{
    // Dynamic offsets: one per UNIFORM_BUFFER_DYNAMIC binding, in binding order; all
    // select this frame slot's 1 KiB region.
    uint32_t dyn[2]; uint32_t nd = 0;
    for (uint32_t i = 0; i < kPasses[p].count; ++i)
        if (kPasses[p].binds[i].type == C) dyn[nd++] = mCurSlotOffset;
    vkBreadcrumb(cmd, kPasses[p].name);   // DEVICE_LOST: which FSR pass (static names)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout[p], 0, 1, &mSet[p][parity], nd, dyn);
    vkCmdDispatch(cmd, std::max(gx, 1u), std::max(gy, 1u), 1);
}

bool LLCleanFSR3VK::prepare(VkCommandBuffer cmd, const Frame& f)
{
    if (!f.ctx || !f.rw || !f.rh || !f.dw || !f.dh || !f.colorView || !f.depthView || !f.motionView
        || !f.reactView || !f.refView || !f.noTemporalView || !f.exposureView || !f.nearestSampler || !f.reproj
        || !f.proj || !f.invProj)
        return false;
    if (!ensurePipelines(f.ctx)) return false;
    if (!ensureCb(f.slotCount)) return false;
    if (!ensureResources(cmd, f)) return false;
    const VkImageView outView = f.outView ? f.outView : mStage.view;
    {
        const VkImageView ext[8] = {f.colorView, f.depthView, f.motionView, f.reactView, f.refView,
                                    f.exposureView, outView, f.noTemporalView};
        // Generation first: a handle compare alone is unsafe (recycled view handles).
        if (mBoundGen != f.targetGen || mBoundGen == 0 || std::memcmp(ext, mBound, sizeof(ext)) != 0
            || mBoundSampler != f.nearestSampler)
        {
            vkDeviceWaitIdle(mCtx->device());   // the sets are in use by frames in flight
            writeSets(f, outView);
            std::memcpy(mBound, ext, sizeof(ext));
            mBoundSampler = f.nearestSampler;
            mBoundGen = f.targetGen;
        }
    }
    mFsr4Now = ensureFsr4(f);
    return true;
}

// #1600: the FSR 4 upscaler for this frame's sizes, made (or made again) here, never in record(). Our FSR 3.1
// is made either way (pass 0 feeds both; a refusal falls back to it within the frame).
bool LLCleanFSR3VK::ensureFsr4(const Frame& f)
{
    if (!f.fsr4 || !f.colorImage || f.colorFormat == VK_FORMAT_UNDEFINED)
        return false;
    const llffx::Camera cam = llffx::cameraFromProjection(f.proj, f.invProj, f.jitterPx);
    const uint64_t key = ((uint64_t)f.rw << 48) ^ ((uint64_t)f.rh << 32) ^ ((uint64_t)f.dw << 16) ^ (uint64_t)f.dh
                       ^ (cam.infiniteFar ? (1ull << 63) : 0ull);
    // (the refusal first: a dispatch refused for these sizes leaves the context made, and fitting)
    if (mFsr4Refused && mFsr4RefusedKey == key) return false;
    if (mFsr4.fits(f.rw, f.rh, f.dw, f.dh, cam.infiniteFar)) return true;
    if (mFsr4.valid())
    {
        vkDeviceWaitIdle(mCtx->device());   // (the old context's resources are in use by frames in flight)
        mFsr4.destroy();
    }
    std::string why;
    const llffx::Device dev{mCtx->physicalDevice(), mCtx->device(), mCtx->fsr4Features()};
    if (!mFsr4.create(dev, f.rw, f.rh, f.dw, f.dh, cam.infiniteFar, why))
    {
        mFsr4Refused = true;
        mFsr4RefusedKey = key;
        LL_INFOS("Vulkan") << "FSR 4 upscaling is not used, FSR 3.1 instead: " << why << LL_ENDL;
        return false;
    }
    mFsr4Refused = false;
    return true;
}

// #1544 frame generation: the main view's camera in the renderer's own terms (llfg::GenCamera), set once
// per frame before this upscale is recorded (the generated frames of this frame are made with the same
// camera). Nothing is converted here: the projection, the view and the reprojection as the renderer holds
// them (column-major), the jitter as rendered (FSR takes it as it is: sign 1), the render size, and what the
// inputs pass writes -- motion in UV units (prev_uv - uv), unjittered; depth reversed-Z. The reset is this
// upscale's own (the renderer's, or fresh resources). Each generator derives its conventions from it.
void LLCleanFSR3VK::frameGenConstants(const Frame& f, bool reset) const
{
    llfg::GenCamera c{};
    std::memcpy(c.proj, f.proj, sizeof(c.proj));
    std::memcpy(c.view, f.view, sizeof(c.view));
    if (f.reproj)
        std::memcpy(c.reproj, f.reproj, sizeof(c.reproj));
    else
        c.reproj[0] = c.reproj[5] = c.reproj[10] = c.reproj[15] = 1.f;   // (no camera history: identity)
    c.jitterPx[0] = f.jitterPx[0];
    c.jitterPx[1] = f.jitterPx[1];
    c.jitterSign = 1.f;
    c.renderW = f.rw;
    c.renderH = f.rh;
    c.motion = llfg::MotionUnits::UvPrevMinusCur;
    c.depthInverted = true;
    c.reset = reset;
    LLVKFrameGen::instance().mainViewConstants(c);
}

void LLCleanFSR3VK::record(VkCommandBuffer cmd, const Frame& f, bool* sharpened)
{
    if (sharpened) *sharpened = false;
    // (#1600) Our FSR 3.1 history is stale after frames upscaled by FSR 4, and FSR 4's after frames of 3.1.
    const bool reset = f.reset || mFirst || mFsr4RanLast;
    const bool fsr4Reset = f.reset || !mFsr4RanLast;
    const uint32_t parity = mFrameIndex & 1u;
    mCurSlotOffset = (f.slot % mCbSlots) * kCbSlotBytes;

    // ---- constants (fsr3upscalerDispatch) ----
    FsrConstants k{};
    k.renderSize[0] = (int32_t)f.rw; k.renderSize[1] = (int32_t)f.rh;
    k.previousFrameRenderSize[0] = reset ? (int32_t)f.rw : mPrevRender[0];
    k.previousFrameRenderSize[1] = reset ? (int32_t)f.rh : mPrevRender[1];
    k.upscaleSize[0] = (int32_t)f.dw; k.upscaleSize[1] = (int32_t)f.dh;
    k.previousFrameUpscaleSize[0] = reset ? (int32_t)f.dw : mPrevDisplay[0];
    k.previousFrameUpscaleSize[1] = reset ? (int32_t)f.dh : mPrevDisplay[1];
    // Resources are allocated at exactly this frame's sizes (re-made on a change).
    k.maxRenderSize[0] = (int32_t)f.rw; k.maxRenderSize[1] = (int32_t)f.rh;
    k.maxUpscaleSize[0] = (int32_t)f.dw; k.maxUpscaleSize[1] = (int32_t)f.dh;
    // Device depth -> positive view depth and the field of view's tangents, from the projection the frame
    // was rendered with (llfsr::deviceToViewDepth: shared with FSR frame generation, #1544).
    llfsr::deviceToViewDepth(f.proj, f.invProj, k.deviceToViewDepth);
    k.tanHalfFOV = k.deviceToViewDepth[2];
    k.jitterOffset[0] = f.jitterPx[0]; k.jitterOffset[1] = f.jitterPx[1];
    k.previousFrameJitterOffset[0] = mPrevJitter[0]; k.previousFrameJitterOffset[1] = mPrevJitter[1];
    k.motionVectorScale[0] = k.motionVectorScale[1] = 1.f;   // the INPUTS pass writes UV, FSR convention
    k.downscaleFactor[0] = (float)f.rw / (float)f.dw;
    k.downscaleFactor[1] = (float)f.rh / (float)f.dh;
    // (motionVectorJitterCancellation: 0 -- our motion is unjittered.)
    // Jitter phase count: this renderer's own Halton cycle (16 x ratio^2, see recordWorld),
    // stepped toward a change one per frame as the SDK does.
    if (reset || mJitterPhaseCount <= 0.f) mJitterPhaseCount = (float)f.jitterPhases;
    else if ((float)f.jitterPhases > mJitterPhaseCount) mJitterPhaseCount += 1.f;
    else if ((float)f.jitterPhases < mJitterPhaseCount) mJitterPhaseCount -= 1.f;
    k.jitterPhaseCount = mJitterPhaseCount;
    if (f.deltaTime >= 0.f)
        k.deltaTime = std::clamp(f.deltaTime, 0.f, 1.f);   // (#1075: the views' shared clock)
    else
    {
        const auto now = std::chrono::steady_clock::now();
        float dt = 1.f / 60.f;
        if (mHaveTime) dt = std::chrono::duration<float>(now - mLastTime).count();
        mLastTime = now; mHaveTime = true;
        k.deltaTime = std::clamp(dt, 0.f, 1.f);
    }
    k.deltaPreExposure = 1.f;            // the scene is not pre-exposed
    k.viewSpaceToMetersFactor = 1.f;     // SL: metres
    mFrameCount = reset ? 0.f : mFrameCount + 1.f;
    k.frameIndex = mFrameCount;
    k.velocityFactor = 1.f;
    k.reactivenessScale = 1.f;           // the INPUTS pass applies our gains
    k.shadingChangeScale = 1.f;
    k.accumulationAddedPerFrame = 1.f / 3.f;
    k.minDisocclusionAccumulation = -1.f / 3.f;

    SpdConstants spd{};
    uint32_t spdGroups[2];
    spdSetup(f.rw, f.rh, spdGroups, spd.workGroupOffset, spd.numWorkGroups, spd.mips);
    spd.renderSize[0] = f.rw; spd.renderSize[1] = f.rh;

    const float sharp = std::clamp(f.sharpness, 0.f, 1.f);
    const bool sharpen = sharp > 0.f;
    RcasConstants rc{};
    rcasCon(rc.rcasConfig, -2.f * sharp + 2.f);   // the SDK's remap: 1 = sharpest

    InputsParams ip{};
    std::memcpy(ip.reproj, f.reproj, sizeof(ip.reproj));
    ip.rscreen[0] = (float)f.rw; ip.rscreen[1] = (float)f.rh;
    ip.rscreen[2] = 1.f / (float)f.rw; ip.rscreen[3] = 1.f / (float)f.rh;
    ip.jitter[0] = f.jitterPx[0]; ip.jitter[1] = f.jitterPx[1];
    ip.jitter[2] = f.jitterPx[0] / (float)f.rw; ip.jitter[3] = f.jitterPx[1] / (float)f.rh;
    if (f.waterPlane) std::memcpy(ip.wplane, f.waterPlane, sizeof(ip.wplane));
    ip.p0[0] = f.motionValid ? 1.f : 0.f;
    ip.p0[1] = f.staticExposure;
    ip.p0[2] = f.dynamicExposure ? 1.f : 0.f;
    ip.p0[3] = f.reactiveMaskGain;
    ip.p1[0] = std::clamp(f.waterTc, 0.f, 1.f);
    ip.p1[1] = std::clamp(f.reactiveCap, 0.f, 1.f);
    ip.p1[2] = f.reactiveDiffGain;

    uint8_t* base = (uint8_t*)mCb.mapped + mCurSlotOffset;
    std::memcpy(base + kCbFsr, &k, sizeof(k));
    std::memcpy(base + kCbSpd, &spd, sizeof(spd));
    std::memcpy(base + kCbRcas, &rc, sizeof(rc));
    std::memcpy(base + kCbInputs, &ip, sizeof(ip));

    mPrevJitter[0] = f.jitterPx[0]; mPrevJitter[1] = f.jitterPx[1];
    mPrevRender[0] = (int32_t)f.rw; mPrevRender[1] = (int32_t)f.rh;
    mPrevDisplay[0] = (int32_t)f.dw; mPrevDisplay[1] = (int32_t)f.dh;

    // ---- clears ----
    // Last frame's FSR reads / writes of these images -> this frame's clears and passes
    // (COPY: the caller's copy out of the staging output, which accumulate overwrites).
    {
        VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT
                       | VK_PIPELINE_STAGE_2_COPY_BIT;
        m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        m.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        m.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1; dep.pMemoryBarriers = &m;
        vkCmdPipelineBarrier2(cmd, &dep);
    }
    const float zero[4] = {0.f, 0.f, 0.f, 0.f};
    const bool first = mFirst;
    if (!mFsr4Now)
    {
    if (mFirst)
    {
        // fsr3upscalerDispatch's firstExecution clears, plus the histories the SDK leaves
        // uninitialised (no NaN garbage reaches a reprojection).
        for (GpuImage* im : {&mAccum[0], &mAccum[1], &mLuma[0], &mLuma[1], &mLumaHist[0], &mLumaHist[1],
                             &mUpscaled[0], &mUpscaled[1]})
            clearImage(cmd, im->image, zero);
        mFirst = false;
    }
    if (reset)
    {
        // (SYNC-HAZARD-WRITE-AFTER-WRITE, B6 validation: the first frame's loop above just
        // cleared both accumulations -- a second clear of the same image with no barrier between)
        if (!first) clearImage(cmd, mAccum[parity ? 1 : 0].image, zero);   // the SRV accumulation
        const float fi[4] = {-1.f, 1.f, 0.f, 0.f};
        clearImage(cmd, mFrameInfo.image, fi);
    }
    clearImage(cmd, mReconDepth.image, zero);   // inverted depth: far = 0
    clearImage(cmd, mSpdAtomic.image, zero);
    clearImage(cmd, mSpdMips.image, zero, mSpdMipCount);
    barrier(cmd, true);
    }

    // ---- passes ----
    const uint32_t srcX = (f.rw + 7) / 8, srcY = (f.rh + 7) / 8;
    const uint32_t dstX = (f.dw + 7) / 8, dstY = (f.dh + 7) / 8;
    const uint32_t scX = ((uint32_t)(f.rw * 0.5f) + 7) / 8, scY = ((uint32_t)(f.rh * 0.5f) + 7) / 8;
    dispatch(cmd, P_INPUTS, mPipe[P_INPUTS], parity, srcX, srcY);
    barrier(cmd);
    bool fsr4Ran = false;
    bool fgReset = reset;
    if (mFsr4Now)
    {
        // #1600: FSR 4 from pass 0's inputs, in the layouts they are in (the colour SHADER_READ_ONLY, ours
        // GENERAL); it writes the output as our passes would (a compute storage write, GENERAL).
        auto in = [](const GpuImage& im) {
            return llffx::Image{im.image, im.format, im.extent.width, im.extent.height, VK_IMAGE_LAYOUT_GENERAL, false};
        };
        llffx::Upscaler::Inputs u;
        u.color = llffx::Image{f.colorImage, f.colorFormat, f.rw, f.rh, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false};
        u.depth = in(mInDepth);
        u.motion = in(mInMotion);
        u.exposure = in(mInExposure);
        u.reactive = in(mInReactive);
        u.composition = in(mInTc);
        u.output = llffx::Image{f.outImage ? f.outImage : mStage.image, VK_FORMAT_R16G16B16A16_SFLOAT, f.dw, f.dh,
                                VK_IMAGE_LAYOUT_GENERAL, true};
        u.renderW = f.rw;
        u.renderH = f.rh;
        u.camera = llffx::cameraFromProjection(f.proj, f.invProj, f.jitterPx);
        u.frameMs = k.deltaTime * 1000.f;
        u.sharpness = sharp;   // (FSR 4 sharpens itself: RenderCASSharpness, as RCAS below)
        u.reset = fsr4Reset;
        std::string err;
        fsr4Ran = mFsr4.dispatch(cmd, u, err);
        if (fsr4Ran)
            fgReset = fsr4Reset;
        else
        {
            // Refused: this frame on FSR 3.1 as a full restart -- its histories are cleared below and its constants
            // (written above as a continuation) rewritten as a reset, as frame generation's are -- and FSR 4 not
            // again for these sizes (ensureFsr4). (The constant ring's slot is not submitted yet: rewriting it is safe.)
            LL_WARNS("Vulkan") << "FSR 4 upscaling refused, FSR 3.1 from now on: " << err << LL_ENDL;
            mFrameCount = 0.f;
            k.frameIndex = 0.f;
            k.previousFrameRenderSize[0] = (int32_t)f.rw; k.previousFrameRenderSize[1] = (int32_t)f.rh;
            k.previousFrameUpscaleSize[0] = (int32_t)f.dw; k.previousFrameUpscaleSize[1] = (int32_t)f.dh;
            mJitterPhaseCount = (float)f.jitterPhases;
            k.jitterPhaseCount = mJitterPhaseCount;
            std::memcpy(base + kCbFsr, &k, sizeof(k));
            fgReset = true;
            mFsr4Refused = true;
            mFsr4RefusedKey = ((uint64_t)f.rw << 48) ^ ((uint64_t)f.rh << 32) ^ ((uint64_t)f.dw << 16) ^ (uint64_t)f.dh
                            ^ (u.camera.infiniteFar ? (1ull << 63) : 0ull);
            mFsr4Now = false;
            mFirst = true;
            for (GpuImage* im : {&mAccum[0], &mAccum[1], &mLuma[0], &mLuma[1], &mLumaHist[0], &mLumaHist[1],
                                 &mUpscaled[0], &mUpscaled[1]})
                clearImage(cmd, im->image, zero);
            const float fi[4] = {-1.f, 1.f, 0.f, 0.f};
            clearImage(cmd, mFrameInfo.image, fi);
            clearImage(cmd, mReconDepth.image, zero);
            clearImage(cmd, mSpdAtomic.image, zero);
            clearImage(cmd, mSpdMips.image, zero, mSpdMipCount);
            barrier(cmd, true);
            mFirst = false;
        }
    }
    // #1544 frame generation: the main view's camera, with the reset of the upscaler that ran this frame (FSR 4,
    // or 3.1 -- a fallback above restarted it).
    if (f.frameGenView && f.view && LLVKFrameGen::instance().directReady()) frameGenConstants(f, fgReset);
    if (fsr4Ran)
    {
        if (sharpened) *sharpened = sharpen;
        mFsr4RanLast = true;
        ++mFrameIndex;
        return;   // (the probe and debug views are FSR 3.1's internal images: not made by FSR 4)
    }
    mFsr4RanLast = false;
    dispatch(cmd, P_PREPARE_INPUTS, mPipe[P_PREPARE_INPUTS], parity, srcX, srcY);
    barrier(cmd);
    dispatch(cmd, P_LUMA_PYRAMID, mPipe[P_LUMA_PYRAMID], parity, spdGroups[0], spdGroups[1]);
    barrier(cmd);
    // (The SPD atomic counter is reset by the last workgroup of each SPD dispatch.)
    dispatch(cmd, P_SHADING_PYRAMID, mPipe[P_SHADING_PYRAMID], parity, spdGroups[0], spdGroups[1]);
    barrier(cmd);
    dispatch(cmd, P_SHADING_CHANGE, mPipe[P_SHADING_CHANGE], parity, scX, scY);
    barrier(cmd);
    dispatch(cmd, P_PREPARE_REACTIVITY, mPipe[P_PREPARE_REACTIVITY], parity, srcX, srcY);
    barrier(cmd);
    dispatch(cmd, P_LUMA_INSTABILITY, mPipe[P_LUMA_INSTABILITY], parity, srcX, srcY);
    barrier(cmd);
    dispatch(cmd, P_ACCUMULATE, sharpen ? mAccumulateSharpen : mPipe[P_ACCUMULATE], parity, dstX, dstY);
    if (sharpen)
    {
        barrier(cmd);
        dispatch(cmd, P_RCAS, mPipe[P_RCAS], parity, (f.dw + 15) / 16, (f.dh + 15) / 16);
        if (sharpened) *sharpened = true;
    }
    if (f.probe)
    {
        // RenderVKFSRDebugProbe: colour input, pre-RCAS internal upscaled colour, output,
        // with the exposure FSR itself used (the INPUTS pass's 1x1: static 1, "dynamic" on).
        LLCleanFSRProbeVK::Images im;
        const VkImageView outView = f.outView ? f.outView : mStage.view;
        im.view[0] = f.colorView;                         im.layout[0] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        im.view[1] = mUpscaled[parity ? 0 : 1].view;      im.layout[1] = VK_IMAGE_LAYOUT_GENERAL;
        im.view[2] = outView;                             im.layout[2] = VK_IMAGE_LAYOUT_GENERAL;
        im.w[0] = f.rw; im.h[0] = f.rh; im.w[1] = im.w[2] = f.dw; im.h[1] = im.h[2] = f.dh;
        im.name[0] = "fsrInput"; im.name[1] = "fsrPreRcas"; im.name[2] = "fsrOutput";
        im.count = 3;
        char label[40];
        std::snprintf(label, sizeof(label), "FSR %ux%u->%ux%u%s", f.rw, f.rh, f.dw, f.dh, sharpen ? " RCAS" : "");
        f.probe->record(mCtx, cmd, LLCleanFSRProbeVK::PT_FSR, f.slot, f.slotCount, im, mInExposure.view,
                        1.f, true, f.probeUv, label);
    }
    if (f.debugView)
    {
        barrier(cmd);
        dispatch(cmd, P_DEBUG, mPipe[P_DEBUG], parity, dstX, dstY);
    }
    ++mFrameIndex;
}

void LLCleanFSR3VK::destroy(VkDevice dev)
{
    if (!dev) return;
    vkDeviceWaitIdle(dev);
    mFsr4.destroy();   // #1600 (the device is idle)
    mFsr4Now = mFsr4RanLast = mFsr4Refused = false;
    destroyResources();
    for (uint32_t p = 0; p < P_COUNT; ++p)
    {
        if (mPipe[p]) vkDestroyPipeline(dev, mPipe[p], nullptr);
        if (mLayout[p]) vkDestroyPipelineLayout(dev, mLayout[p], nullptr);
        mPipe[p] = VK_NULL_HANDLE; mLayout[p] = VK_NULL_HANDLE;
    }
    if (mAccumulateSharpen) vkDestroyPipeline(dev, mAccumulateSharpen, nullptr);
    mAccumulateSharpen = VK_NULL_HANDLE;
    mPool.destroy();
    for (uint32_t p = 0; p < P_COUNT; ++p)
    {
        if (mDsl[p]) vkdDestroySetLayout(dev, mDsl[p]);
        mDsl[p] = VK_NULL_HANDLE;
        mSet[p][0] = mSet[p][1] = VK_NULL_HANDLE;
    }
    if (mPointClamp) vkDestroySampler(dev, mPointClamp, nullptr);
    if (mLinearClamp) vkDestroySampler(dev, mLinearClamp, nullptr);
    mPointClamp = mLinearClamp = VK_NULL_HANDLE;
    if (mCtx && mCb.buffer) mCtx->destroyBuffer(mCb);
    mCbSlots = 0;
    mFirst = true;
    mFrameIndex = 0; mFrameCount = 0.f; mJitterPhaseCount = 0.f;
    mHaveTime = false;
}

// ===========================================================================
// RenderVKFSRDebugProbe (clean_fsr_probe.comp)
// ===========================================================================
namespace
{
struct ProbePush
{
    int32_t size01[4];   // A w, h; B w, h
    int32_t size2[4];    // C w, h; region base (uint index); image count
    float   params[4];   // probe u, v; static exposure; dynamic exposure on
};
static_assert(sizeof(ProbePush) == 48, "ProbePush must match clean_fsr_probe.comp Push");

float probeF(uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return f; }

float probeHalf(uint32_t h)
{
    const uint32_t e = (h >> 10) & 31u, m = h & 1023u;
    float v = e == 0u ? std::ldexp((float)m, -24)
            : e == 31u ? FLT_MAX   // Inf / NaN (the build's float options rule both out of the CPU side)
            : std::ldexp((float)(m | 1024u), (int)e - 25);
    return (h & 0x8000u) ? -v : v;
}

// A texel copied raw into the probe region (recordPixel's copy path); false: format not read.
bool probeTexelRgb(VkFormat format, const uint32_t* t, float rgb[3])
{
    switch (format)
    {
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        rgb[0] = probeHalf(t[0] & 0xffffu); rgb[1] = probeHalf(t[0] >> 16); rgb[2] = probeHalf(t[1] & 0xffffu);
        return true;
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
        rgb[0] = ((t[0] >> 16) & 255u) / 255.f; rgb[1] = ((t[0] >> 8) & 255u) / 255.f; rgb[2] = (t[0] & 255u) / 255.f;
        return true;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
        rgb[0] = (t[0] & 255u) / 255.f; rgb[1] = ((t[0] >> 8) & 255u) / 255.f; rgb[2] = ((t[0] >> 16) & 255u) / 255.f;
        return true;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        rgb[0] = (t[0] & 1023u) / 1023.f; rgb[1] = ((t[0] >> 10) & 1023u) / 1023.f; rgb[2] = ((t[0] >> 20) & 1023u) / 1023.f;
        return true;
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        rgb[0] = ((t[0] >> 20) & 1023u) / 1023.f; rgb[1] = ((t[0] >> 10) & 1023u) / 1023.f; rgb[2] = (t[0] & 1023u) / 1023.f;
        return true;
    default:
        return false;
    }
}

float probeSrgbDecode(float v)
{
    return v <= 0.04045f ? v / 12.92f : std::pow((std::max(v, 0.f) + 0.055f) / 1.055f, 2.4f);
}

void probeLayout(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                 VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                 VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage; b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1; dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}
} // namespace

bool LLCleanFSRProbeVK::ensure(VkDeviceContext* ctx, uint32_t slotCount)
{
    VkMemScope memcat(VKMEM_UPSCALER);   // #407
    if (mFailed) return false;
    if (mPipe && mSlots >= std::min(slotCount, kMaxSlots)) return true;
    if (mPipe) return false;   // (slot count grew: not expected; stay off)
    mCtx = ctx;
    VkDevice dev = ctx->device();
    mSlots = std::max(1u, std::min(slotCount, kMaxSlots));

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(dev, &sci, nullptr, &mSampler));
    const VkSampler imm[3] = {mSampler, mSampler, mSampler};

    VkDescriptorSetLayoutBinding b[5]{};
    for (uint32_t i = 0; i < 3; ++i)
    {
        b[i].binding = i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; b[i].pImmutableSamplers = &imm[i];
    }
    b[3].binding = 3; b[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[3].descriptorCount = 1; b[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[4].binding = 4; b[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[4].descriptorCount = 1; b[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 5; lci.pBindings = b;
    VK_CHECK(vkdCreateSetLayout(dev, &lci, &mDsl, "fsrProbe"));
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, (uint32_t)sizeof(ProbePush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1; pli.pSetLayouts = &mDsl;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
    VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mLayout, "fsrProbe"));
    VK_CHECK(mPool.create(dev, "fsrProbe", {{mDsl, PT_COUNT * mSlots}}));
    for (uint32_t p = 0; p < PT_COUNT; ++p)
        for (uint32_t s = 0; s < mSlots; ++s)
            VKD_ALLOC_SOFT(mPool.allocate(mDsl, &mSet[p][s]), { destroy(dev); return false; });
    for (uint32_t s = 0; s < mSlots; ++s)
    {
        mBuf[s] = ctx->createBuffer((VkDeviceSize)PT_COUNT * kRegionWords * 4,
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,   // recordPixel's copy
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!mBuf[s].buffer || !mBuf[s].mapped) { mFailed = true; break; }
        std::memset(mBuf[s].mapped, 0, (size_t)PT_COUNT * kRegionWords * 4);
    }
    if (!mFailed)
    {
        VkShaderModule m = fsrModule(dev, kFsrProbeSpv, kFsrProbeSpv_bytes);
        mPipe = createComputePipeline(dev, m, mLayout);
        vkDestroyShaderModule(dev, m, nullptr);
        if (!mPipe) mFailed = true;
    }
    if (mFailed)
    {
        LL_WARNS("VKFSR") << "HDR probe: set-up failed; RenderVKFSRDebugProbe stays off" << LL_ENDL;
        return false;
    }
    LL_INFOS("VKFSR") << "HDR probe on (RenderVKFSRDebugProbe): " << mSlots << " slots" << LL_ENDL;
    return true;
}

void LLCleanFSRProbeVK::collect(uint32_t slot)
{
    if (!mPipe || mFailed) return;
    slot %= mSlots;
    uint32_t* w = static_cast<uint32_t*>(mBuf[slot].mapped);
    if (!w) return;
    const auto now = std::chrono::steady_clock::now();
    for (uint32_t pt = 0; pt < PT_COUNT; ++pt)
    {
        Meta& m = mMeta[slot][pt];
        const uint32_t* r = w + pt * kRegionWords;
        if (m.recorded && r[0] == 1u && (m.force || now - mLastLog[pt] >= std::chrono::milliseconds(1000)))
        {
            mLastLog[pt] = now;
            const float expo = probeF(r[1]);
            std::ostringstream os;
            os.setf(std::ios::fixed); os.precision(3);
            if (m.display)
            {
                // One display-stage pixel (#1451). HDR: relative linear, 1 = paper white (the
                // final pass's nits divided by it). SDR: the display-encoded value decoded with
                // the sRGB curve, i.e. what the panel shows relative to SDR white.
                float raw[3] = {0.f, 0.f, 0.f};
                const bool known = m.copied ? probeTexelRgb(m.format, r + 2, raw) : true;
                if (!m.copied)
                    for (uint32_t c = 0; c < 3; ++c) raw[c] = probeF(r[2 + c]);
                os << "HDR probe [" << m.label << "] @" << m.px[0][0] << "," << m.px[0][1];
                if (!known)
                    os << " texel format " << (int)m.format << " not decoded";
                else
                {
                    const float white = std::max(m.paperWhiteNits, 1.f);
                    float lin[3];
                    for (uint32_t c = 0; c < 3; ++c)
                        lin[c] = m.units == Units::Nits ? raw[c] / white
                               : (m.units == Units::SrgbEncoded ? probeSrgbDecode(raw[c]) : raw[c]);
                    const float lum = 0.2126f * lin[0] + 0.7152f * lin[1] + 0.0722f * lin[2];
                    const bool sdr = m.units == Units::SrgbEncoded;
                    os << " raw(" << raw[0] << ", " << raw[1] << ", " << raw[2] << ") "
                       << (sdr ? "decoded(" : "relative(") << lin[0] << ", " << lin[1] << ", " << lin[2]
                       << ") luma " << lum << (sdr ? " sdrDisplayNits " : " lumaNits ") << lum * white
                       << " paperWhite " << white;
                }
                LL_INFOS("VKFSR") << os.str() << LL_ENDL;
                m.recorded = false;
                m.force = false;
                continue;
            }
            os << "HDR probe [" << m.label << "] sceneExposureFactor " << expo;
            for (uint32_t k = 0; k < m.count; ++k)
            {
                const uint32_t* st = r + 11 + 6 * k;
                const float cr = probeF(r[2 + 3 * k]), cg = probeF(r[3 + 3 * k]), cb = probeF(r[4 + 3 * k]);
                const float lum = 0.2126f * cr + 0.7152f * cg + 0.0722f * cb;
                const float meanScale = (pt == PT_POST || pt == PT_HDR_ENCODE) ? 16.f : 256.f;
                const float gmean = st[5] ? (float)st[4] / meanScale / (float)st[5] : 0.f;
                const float measuredLuma = lum * (pt == PT_HDR_ENCODE ? 1.f : expo);
                os << " | " << m.name[k] << " @" << m.px[k][0] << "," << m.px[k][1]
                   << " rgb(" << cr << ", " << cg << ", " << cb << ") "
                   << (pt == PT_HDR_ENCODE ? "encodeInputLumaNits " : "exposedLuma ") << measuredLuma
                   << " max " << probeF(st[0]) << " nanInf " << st[1] << " >16 " << st[2] << " >256 " << st[3]
                   << (pt == PT_HDR_ENCODE ? " gridMeanNits " : " gridMeanExposedLuma ") << gmean;
                if ((pt == PT_POST || pt == PT_HDR_ENCODE) && k == 0)
                {
                    uint32_t samples = 0;
                    for (uint32_t bin = 0; bin < 128; ++bin) samples += r[32 + bin];
                    const uint32_t target = (samples * 95u + 99u) / 100u;
                    uint32_t cumulative = 0, p95bin = 0;
                    for (; p95bin < 128; ++p95bin)
                    {
                        cumulative += r[32 + p95bin];
                        if (cumulative >= target) break;
                    }
                    const float p95 = std::exp2(-16.f + (std::min(p95bin, 127u) + 0.5f) * 0.25f);
                    os << (pt == PT_HDR_ENCODE ? " encodeInputMeanNits " : " exposedSceneMean ") << gmean
                       << (pt == PT_HDR_ENCODE ? " encodeInputP95Nits " : " exposedSceneP95 ") << p95
                       << " histogramSamples " << samples << " histogramBins 128 rangeStops [-16,+16]";
                }
            }
            LL_INFOS("VKFSR") << os.str() << LL_ENDL;
        }
        m.recorded = false;
        m.force = false;
    }
    std::memset(w, 0, (size_t)PT_COUNT * kRegionWords * 4);   // host-coherent; the slot is idle
}

void LLCleanFSRProbeVK::record(VkDeviceContext* ctx, VkCommandBuffer cmd, Point pt, uint32_t slot, uint32_t slotCount,
                               const Images& im, VkImageView exposureView, float staticExposure, bool dynamicExposure,
                               const float uv[2], const char* label, bool histogram, bool histogramInputIsExposed)
{
    if (!ctx || !exposureView || !im.view[0] || !im.count || !ensure(ctx, slotCount)) return;
    slot %= mSlots;
    const uint32_t n = std::min(im.count, 3u);

    // Sets: this slot's (idle: its fence was waited), rewritten every record (the views
    // change with the ping-pong parity, resizes and mode switches).
    VkDescriptorImageInfo ii[4]{};
    for (uint32_t k = 0; k < 3; ++k)
    {
        const uint32_t s = k < n && im.view[k] ? k : 0;
        ii[k] = {VK_NULL_HANDLE, im.view[s], im.layout[s]};
    }
    ii[3] = {VK_NULL_HANDLE, exposureView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo bi{mBuf[slot].buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet wr[5]{};
    for (uint32_t i = 0; i < 5; ++i)
    {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = mSet[pt][slot]; wr[i].dstBinding = i; wr[i].descriptorCount = 1;
        wr[i].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                             : (i == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        if (i < 4) wr[i].pImageInfo = &ii[i]; else wr[i].pBufferInfo = &bi;
    }
    vkUpdateDescriptorSets(ctx->device(), 5, wr, 0, nullptr);

    ProbePush pp{};
    uint32_t w[3], h[3];
    for (uint32_t k = 0; k < 3; ++k)
    {
        const uint32_t s = k < n && im.view[k] ? k : 0;
        w[k] = std::max(im.w[s], 1u); h[k] = std::max(im.h[s], 1u);
    }
    pp.size01[0] = (int32_t)w[0]; pp.size01[1] = (int32_t)h[0];
    pp.size01[2] = (int32_t)w[1]; pp.size01[3] = (int32_t)h[1];
    pp.size2[0] = (int32_t)w[2]; pp.size2[1] = (int32_t)h[2];
    pp.size2[2] = (int32_t)(pt * kRegionWords);
    pp.size2[3] = histogram ? -int32_t(n + (histogramInputIsExposed ? 0u : 10u)) : int32_t(n);
    pp.params[0] = std::clamp(uv[0], 0.f, 1.f); pp.params[1] = std::clamp(uv[1], 0.f, 1.f);
    pp.params[2] = staticExposure; pp.params[3] = dynamicExposure ? 1.f : 0.f;

    Meta& m = mMeta[slot][pt];
    m.recorded = true; m.count = n; m.force = mForceLog;
    std::snprintf(m.label, sizeof(m.label), "%s", label ? label : "");
    for (uint32_t k = 0; k < n; ++k)
    {
        std::snprintf(m.name[k], sizeof(m.name[k]), "%s", im.name[k] ? im.name[k] : "");
        m.px[k][0] = std::min((uint32_t)(pp.params[0] * (float)w[k]), w[k] - 1);
        m.px[k][1] = std::min((uint32_t)(pp.params[1] * (float)h[k]), h[k] - 1);
    }

    // Every earlier write (attachments, copies, compute) -> the probe's reads.
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                    | VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
    mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                     | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT
                     | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1; dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout, 0, 1, &mSet[pt][slot], 0, nullptr);
    vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pp), &pp);
    const uint32_t gw = std::max(std::max(w[0], w[1]), w[2]), gh = std::max(std::max(h[0], h[1]), h[2]);
    vkCmdDispatch(cmd, (gw + 7) / 8, (gh + 7) / 8, 1);

    // Results -> host (read a slot later, after the fence); and the probe's reads finish
    // before anything later in the frame overwrites what it read (WAR, any stage).
    VkMemoryBarrier2 ab[2]{};
    ab[0].sType = ab[1].sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    ab[0].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; ab[0].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    ab[0].dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;           ab[0].dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    ab[1].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; ab[1].srcAccessMask = VK_ACCESS_2_NONE;
    ab[1].dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;   ab[1].dstAccessMask = VK_ACCESS_2_NONE;
    VkDependencyInfo d2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d2.memoryBarrierCount = 2; d2.pMemoryBarriers = ab;
    vkCmdPipelineBarrier2(cmd, &d2);
}

void LLCleanFSRProbeVK::recordPixel(VkDeviceContext* ctx, VkCommandBuffer cmd, Point pt, uint32_t slot,
                                    uint32_t slotCount, const Pixel& px, VkImageView exposureView,
                                    const float uv[2], const char* label)
{
    if (!ctx || !px.image || !px.w || !px.h || !exposureView || !ensure(ctx, slotCount)) return;
    const uint32_t zero[2] = {0u, 0u};
    float unused[3];
    if (!px.view && !probeTexelRgb(px.format, zero, unused)) return;   // a copy in a format collect() cannot read
    slot %= mSlots;
    // Every earlier write of the image (attachments, copies, compute) -> the tap's read.
    const VkPipelineStageFlags2 writers = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
                                        | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    const VkAccessFlags2 writes = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                                | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    const VkPipelineStageFlags2 later = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    const VkAccessFlags2 laterAccess = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    if (px.view)
    {
        probeLayout(cmd, px.image, px.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, writers, writes,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        Images im;
        im.view[0] = px.view; im.layout[0] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        im.w[0] = px.w; im.h[0] = px.h; im.name[0] = "pixel"; im.count = 1;
        record(ctx, cmd, pt, slot, slotCount, im, exposureView, 1.f, false, uv, label);
        probeLayout(cmd, px.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, px.layout,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_NONE, later, laterAccess);
    }
    else
    {
        // No SAMPLED usage (the swapchain): copy the one texel into the region's rgb words.
        const uint32_t x = std::min((uint32_t)(std::clamp(uv[0], 0.f, 1.f) * (float)px.w), px.w - 1);
        const uint32_t y = std::min((uint32_t)(std::clamp(uv[1], 0.f, 1.f) * (float)px.h), px.h - 1);
        probeLayout(cmd, px.image, px.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, writers, writes,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.bufferOffset = (VkDeviceSize)(pt * kRegionWords + 2u) * 4u;   // a multiple of 8: any texel size here
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {(int32_t)x, (int32_t)y, 0};
        region.imageExtent = {1, 1, 1};
        vkCmdCopyImageToBuffer(cmd, px.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mBuf[slot].buffer, 1, &region);
        vkCmdFillBuffer(cmd, mBuf[slot].buffer, (VkDeviceSize)pt * kRegionWords * 4u, 4u, 1u);   // "written"
        probeLayout(cmd, px.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, px.layout,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_NONE, later, laterAccess);
        VkMemoryBarrier2 hb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        hb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; hb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        hb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;         hb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1; dep.pMemoryBarriers = &hb;
        vkCmdPipelineBarrier2(cmd, &dep);
        Meta& c = mMeta[slot][pt];
        c.recorded = true; c.count = 1; c.force = mForceLog;
        std::snprintf(c.label, sizeof(c.label), "%s", label ? label : "");
        std::snprintf(c.name[0], sizeof(c.name[0]), "pixel");
        c.px[0][0] = x; c.px[0][1] = y;
    }
    Meta& m = mMeta[slot][pt];
    m.display = true;
    m.copied = !px.view;
    m.format = px.format;
    m.units = px.units;
    m.paperWhiteNits = px.paperWhiteNits;
}

void LLCleanFSRProbeVK::destroy(VkDevice dev)
{
    if (!dev) return;
    vkDeviceWaitIdle(dev);
    if (mPipe) vkDestroyPipeline(dev, mPipe, nullptr);
    mPipe = VK_NULL_HANDLE;
    mPool.destroy();
    for (auto& row : mSet) for (VkDescriptorSet& s : row) s = VK_NULL_HANDLE;
    if (mLayout) vkDestroyPipelineLayout(dev, mLayout, nullptr);
    mLayout = VK_NULL_HANDLE;
    if (mDsl) vkdDestroySetLayout(dev, mDsl);
    mDsl = VK_NULL_HANDLE;
    if (mSampler) vkDestroySampler(dev, mSampler, nullptr);
    mSampler = VK_NULL_HANDLE;
    if (mCtx)
        for (GpuBuffer& b : mBuf)
            if (b.buffer) mCtx->destroyBuffer(b);
    mSlots = 0;
    mFailed = false;
}
