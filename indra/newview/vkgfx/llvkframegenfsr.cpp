/**
 * @file llvkframegenfsr.cpp
 * @brief Frame generation's FSR generator: see llvkframegenfsr.h.
 *
 * The resources, clears, constants, pass order and dispatch sizes follow FidelityFX SDK v1.1.4
 * sdk/src/components/frameinterpolation/ffx_frameinterpolation.cpp (ffxFrameInterpolationPrepare /
 * ffxFrameInterpolationDispatch) and sdk/src/components/opticalflow/ffx_opticalflow.cpp (dispatch) -- MIT,
 * Copyright (C) 2024 Advanced Micro Devices, Inc. (indra/vkclean/third_party/fidelityfx/LICENSE.txt). The
 * numbers are llvkframegenfsrmath.h's (tested on the host); this file makes the objects and records.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */
#include "linden_common.h"

#include "llvkframegenfsr.h"
#include "vk_pipeline.h"
#include "vk_fsr3_shaders_spv.h"

#include "llerror.h"
#include "../llviewercontrol.h"   // #1600 RenderVKFrameGenFsr4 (read on the main thread only)

#include <algorithm>
#include <cstring>
#include <vector>

namespace
{
using llfsrfg::Step;

constexpr VkDescriptorType S = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
constexpr VkDescriptorType U = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
constexpr VkDescriptorType C = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
constexpr VkDescriptorType B = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

// Which block of the constant ring a uniform buffer binding reads (its dynamic offset).
enum Cb : uint8_t { CB_NONE, CB_FI, CB_PYRAMID_GAME, CB_PYRAMID_OUTPUT, CB_OF, CB_OF_SPD };

// Each pass's set 0: every binding its .glsl entry point declares, in binding order (ffx_frameinterpolation_*
// _pass.glsl, ffx_opticalflow_*_pass.glsl) -- also those glslc -O drops (a set layout may hold more than the
// module reads, never less, and what survives depends on the compiler) -- plus, on the frame interpolation
// passes, the linear clamp sampler at 1000 (immutable). The sets exist once, or per present slot (a pass that
// binds the slot's world, depth or motion image), per frame parity and per optical flow level, as marked.
struct Bind { uint32_t binding; VkDescriptorType type; Cb cb; };
struct PassDesc
{
    const char* name;
    bool sampler, perSlot, perParity, perLevel;
    uint32_t count;
    Bind binds[18];
};
const PassDesc kPasses[] = {
    {"fsrFgReconstructDilate",     true,  true,  false, false, 6,
        {{0,S,CB_NONE},{1,S,CB_NONE},{2,U,CB_NONE},{3,U,CB_NONE},{4,U,CB_NONE},{5,C,CB_FI}}},
    {"fsrFgOfPrepareLuma",         false, true,  true,  false, 3,
        {{0,S,CB_NONE},{1,U,CB_NONE},{2,C,CB_OF}}},
    {"fsrFgOfLuminancePyramid",    false, false, true,  false, 9,
        {{0,U,CB_NONE},{1,U,CB_NONE},{2,U,CB_NONE},{3,U,CB_NONE},{4,U,CB_NONE},{5,U,CB_NONE},{6,U,CB_NONE},{7,C,CB_OF},{8,C,CB_OF_SPD}}},
    {"fsrFgOfScdHistogram",        false, false, true,  false, 3,
        {{0,S,CB_NONE},{1,U,CB_NONE},{2,C,CB_OF}}},
    {"fsrFgOfScdDivergence",       false, false, false, false, 5,
        {{0,U,CB_NONE},{1,U,CB_NONE},{2,U,CB_NONE},{3,U,CB_NONE},{4,C,CB_OF}}},
    {"fsrFgOfSearch",              false, false, true,  true,  5,
        {{0,S,CB_NONE},{1,S,CB_NONE},{2,U,CB_NONE},{3,U,CB_NONE},{4,C,CB_OF}}},
    {"fsrFgOfFilter",              false, false, true,  true,  3,
        {{0,S,CB_NONE},{1,U,CB_NONE},{2,C,CB_OF}}},
    {"fsrFgOfScale",               false, false, true,  true,  6,
        {{0,S,CB_NONE},{1,S,CB_NONE},{2,S,CB_NONE},{3,U,CB_NONE},{4,U,CB_NONE},{5,C,CB_OF}}},
    {"fsrFgSetup",                 true,  false, false, false, 8,
        {{0,S,CB_NONE},{1,U,CB_NONE},{2,U,CB_NONE},{3,U,CB_NONE},{4,U,CB_NONE},{5,U,CB_NONE},{6,B,CB_NONE},{7,C,CB_FI}}},
    {"fsrFgReconstructPrevDepth",  true,  true,  false, false, 6,
        {{0,S,CB_NONE},{1,S,CB_NONE},{2,S,CB_NONE},{3,S,CB_NONE},{4,U,CB_NONE},{5,C,CB_FI}}},
    {"fsrFgGameVectorField",       true,  true,  false, false, 8,
        {{0,S,CB_NONE},{1,S,CB_NONE},{2,S,CB_NONE},{3,S,CB_NONE},{4,S,CB_NONE},{5,U,CB_NONE},{6,U,CB_NONE},{7,C,CB_FI}}},
    {"fsrFgGameVectorPyramid",     true,  false, false, false, 18,
        {{0,S,CB_NONE},{1,S,CB_NONE},{2,B,CB_NONE},{3,U,CB_NONE},{4,U,CB_NONE},{5,U,CB_NONE},{6,U,CB_NONE},{7,U,CB_NONE},{8,U,CB_NONE},
         {9,U,CB_NONE},{10,U,CB_NONE},{11,U,CB_NONE},{12,U,CB_NONE},{13,U,CB_NONE},{14,U,CB_NONE},{15,U,CB_NONE},{16,C,CB_FI},
         {17,C,CB_PYRAMID_GAME}}},
    {"fsrFgOpticalFlowVectorField", true, true,  false, false, 8,
        {{0,S,CB_NONE},{1,S,CB_NONE},{2,S,CB_NONE},{3,S,CB_NONE},{4,S,CB_NONE},{6,U,CB_NONE},{7,U,CB_NONE},{8,C,CB_FI}}},
    {"fsrFgDisocclusionMask",      true,  false, false, false, 9,
        {{0,S,CB_NONE},{1,S,CB_NONE},{2,S,CB_NONE},{3,S,CB_NONE},{4,S,CB_NONE},{5,S,CB_NONE},{6,S,CB_NONE},{7,U,CB_NONE},{8,C,CB_FI}}},
    {"fsrFgInterpolate",           true,  true,  false, false, 11,
        {{0,S,CB_NONE},{1,S,CB_NONE},{2,S,CB_NONE},{3,S,CB_NONE},{4,S,CB_NONE},{5,S,CB_NONE},{6,S,CB_NONE},{7,S,CB_NONE},{8,B,CB_NONE},
         {9,U,CB_NONE},{10,C,CB_FI}}},
    {"fsrFgInpaintingPyramid",     true,  false, false, false, 17,
        {{0,S,CB_NONE},{1,B,CB_NONE},{2,U,CB_NONE},{3,U,CB_NONE},{4,U,CB_NONE},{5,U,CB_NONE},{6,U,CB_NONE},{7,U,CB_NONE},{8,U,CB_NONE},
         {9,U,CB_NONE},{10,U,CB_NONE},{11,U,CB_NONE},{12,U,CB_NONE},{13,U,CB_NONE},{14,U,CB_NONE},{15,C,CB_FI},{16,C,CB_PYRAMID_OUTPUT}}},
    {"fsrFgInpainting",            true,  true,  false, false, 6,
        {{0,S,CB_NONE},{1,S,CB_NONE},{2,S,CB_NONE},{3,S,CB_NONE},{7,U,CB_NONE},{8,C,CB_FI}}},
};
static_assert(sizeof(kPasses) / sizeof(kPasses[0]) == LLVKFrameGenFsr::P_COUNT, "one PassDesc per LLVKFrameGenFsr::Pass");

// The pass that records a compute step of the plan (P_COUNT: not a compute step).
LLVKFrameGenFsr::Pass passOf(Step s)
{
    switch (s)
    {
    case Step::ReconstructAndDilate:   return LLVKFrameGenFsr::P_RECONSTRUCT_DILATE;
    case Step::OfPrepareLuma:          return LLVKFrameGenFsr::P_OF_PREPARE_LUMA;
    case Step::OfLuminancePyramid:     return LLVKFrameGenFsr::P_OF_LUMA_PYRAMID;
    case Step::OfScdHistogram:         return LLVKFrameGenFsr::P_OF_SCD_HISTOGRAM;
    case Step::OfScdDivergence:        return LLVKFrameGenFsr::P_OF_SCD_DIVERGENCE;
    case Step::OfSearch:               return LLVKFrameGenFsr::P_OF_SEARCH;
    case Step::OfFilter:               return LLVKFrameGenFsr::P_OF_FILTER;
    case Step::OfScale:                return LLVKFrameGenFsr::P_OF_SCALE;
    case Step::Setup:                  return LLVKFrameGenFsr::P_SETUP;
    case Step::ReconstructPrevDepth:   return LLVKFrameGenFsr::P_RECONSTRUCT_PREV_DEPTH;
    case Step::GameVectorField:        return LLVKFrameGenFsr::P_GAME_VECTOR_FIELD;
    case Step::GameVectorPyramid:      return LLVKFrameGenFsr::P_GAME_VECTOR_PYRAMID;
    case Step::OpticalFlowVectorField: return LLVKFrameGenFsr::P_OF_VECTOR_FIELD;
    case Step::DisocclusionMask:       return LLVKFrameGenFsr::P_DISOCCLUSION_MASK;
    case Step::Interpolate:            return LLVKFrameGenFsr::P_INTERPOLATE;
    case Step::InpaintingPyramid:      return LLVKFrameGenFsr::P_INPAINTING_PYRAMID;
    case Step::Inpainting:             return LLVKFrameGenFsr::P_INPAINTING;
    case Step::ClearAll:
    case Step::ClearReconstructedPrevDepth:
    case Step::ClearReconstructedDepthInterpolated:
    case Step::CopyPreviousSource:
        break;
    }
    return LLVKFrameGenFsr::P_COUNT;
}

// The variant of the three passes that write the generated frame, by the output (scratch) image's format:
// their storage image is declared with an explicit format (the vendored callbacks' SLIPSTREAM PATCH), so
// the image must have exactly it. 0: rgb10_a2 (HDR10), 1: rgba8 (SDR; GLSL has no BGRA storage format),
// -1: no variant.
int32_t outputVariant(VkFormat format)
{
    return format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ? 0 : format == VK_FORMAT_R8G8B8A8_UNORM ? 1 : -1;
}

const char* formatName(VkFormat f)
{
    switch (f)
    {
    case VK_FORMAT_R8_UINT:                  return "R8_UINT";
    case VK_FORMAT_R16G16_SINT:              return "R16G16_SINT";
    case VK_FORMAT_R32_UINT:                 return "R32_UINT";
    case VK_FORMAT_R32_SFLOAT:               return "R32_SFLOAT";
    case VK_FORMAT_R16G16_SFLOAT:            return "R16G16_SFLOAT";
    case VK_FORMAT_R8G8_UNORM:               return "R8G8_UNORM";
    case VK_FORMAT_R16G16B16A16_SFLOAT:      return "R16G16B16A16_SFLOAT";
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_UNORM";
    case VK_FORMAT_R8G8B8A8_UNORM:           return "R8G8B8A8_UNORM";
    case VK_FORMAT_B8G8R8A8_UNORM:           return "B8G8R8A8_UNORM";
    default:                                 return "?";
    }
}

VkPhysicalDeviceSubgroupProperties subgroups(VkDeviceContext& dev)
{
    VkPhysicalDeviceSubgroupProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &sg;
    vkGetPhysicalDeviceProperties2(dev.physicalDevice(), &p2);
    return sg;
}

// What this device lacks for the passes (empty: nothing). Checked against the compiled modules: their storage
// image formats (each also read through a sampled binding), the formats read through the linear sampler (the
// world image in each format it can have, the disocclusion mask and the distortion field), and the optical
// flow search's subgroup arithmetic -- it sums over a 64-thread group as one subgroup of 64 or two of 32.
std::string deviceLacks(VkDeviceContext& dev)
{
    if (!dev.storageImageExtendedFormats()) return "the GPU has no extended storage image formats";
    const VkFormat storage[] = {VK_FORMAT_R8_UINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SFLOAT,
                                VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT,
                                VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_R8G8B8A8_UNORM};
    for (VkFormat f : storage)
    {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(dev.physicalDevice(), f, &fp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((fp.optimalTilingFeatures & need) != need)
            return std::string("the GPU cannot write the image format ") + formatName(f);
    }
    const VkFormat linear[] = {VK_FORMAT_R8G8_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_R8G8B8A8_UNORM,
                               VK_FORMAT_B8G8R8A8_UNORM};
    for (VkFormat f : linear)
    {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(dev.physicalDevice(), f, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
            return std::string("the GPU cannot filter the image format ") + formatName(f);
    }
    const VkPhysicalDeviceSubgroupProperties sg = subgroups(dev);
    const VkSubgroupFeatureFlags ops = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
    if (!(sg.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) || (sg.supportedOperations & ops) != ops)
        return "the GPU has no subgroup arithmetic in compute shaders";
    if (sg.subgroupSize != 32 && sg.subgroupSize != 64)
        return "the optical flow needs a subgroup size of 32 or 64 (this GPU: " + std::to_string(sg.subgroupSize) + ")";
    // (the present queue is a queue of the graphics family: the passes are compute)
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev.physicalDevice(), &n, nullptr);
    std::vector<VkQueueFamilyProperties> families(n);
    vkGetPhysicalDeviceQueueFamilyProperties(dev.physicalDevice(), &n, families.data());
    if (dev.queueFamily() >= n || !(families[dev.queueFamily()].queueFlags & VK_QUEUE_COMPUTE_BIT))
        return "the queue that presents cannot run compute shaders";
    return std::string();
}

bool sameRef(const llfg::GpuImageRef& a, const llfg::GpuImageRef& b)
{
    return a.image == b.image && a.view == b.view && a.format == b.format && a.extent.width == b.extent.width
        && a.extent.height == b.extent.height;
}

bool hasExtent(const llfg::GpuImageRef& r, uint32_t w, uint32_t h)
{
    return r.image && r.view && r.extent.width == w && r.extent.height == h;
}
} // namespace

llfg::Block LLVKFrameGenFsr::init(VkDeviceContext& dev, llfg::GeneratorCaps& caps, std::string& detail, std::string& log)
{
    caps = llfg::GeneratorCaps{};
    const std::string lacks = deviceLacks(dev);
    if (!lacks.empty())
    {
        detail = lacks;
        log = "FSR frame generation (FidelityFX SDK 1.1.4 frame interpolation and optical flow) cannot run: " + lacks;
        return llfg::Block::Unsupported;
    }
    caps = llfg::fsrGeneratorCaps(dev.lowLatency2());
    return llfg::Block::None;
}

bool LLVKFrameGenFsr::fits(const llfg::GenSize& size) const
{
    if (!mReady || size.displayW != mSize.displayW || size.displayH != mSize.displayH || size.renderW != mSize.renderW
        || size.renderH != mSize.renderH || size.colorFormat != mSize.colorFormat || size.scratchFormat != mSize.scratchFormat
        || size.slots != mSize.slots || size.imagesSerial != mSize.imagesSerial || !sameRef(size.output, mSize.output))
        return false;
    for (uint32_t k = 0; k < mSize.slots; ++k)
        if (!sameRef(size.color[k], mSize.color[k]) || !sameRef(size.depth[k], mSize.depth[k])
            || !sameRef(size.motion[k], mSize.motion[k]))
            return false;
    // #1600: FSR 4 made exactly when it is wanted (the setting turned on or off: made again), and for these
    // frames' encoding (HDR10 or SDR: made again when it changes)
    if (mFsr4.valid() && size.colorHdr != mSize.colorHdr) return false;
    return wantFsr4(size) == mFsr4.valid();
}

bool LLVKFrameGenFsr::wantFsr4(const llfg::GenSize& size) const
{
    // RenderVKFrameGenFsr4 (hidden, not saved, off): FSR 4 frame generation has not been run on an RX 9000 yet.
    static LLCachedControl<bool> s_fsr4(gSavedSettings, "RenderVKFrameGenFsr4", false);
    if (!s_fsr4) return false;
    return !(mFsr4Refused && mFsr4RefusedDisplay[0] == size.displayW && mFsr4RefusedDisplay[1] == size.displayH
             && mFsr4RefusedRender[0] == size.renderW && mFsr4RefusedRender[1] == size.renderH
             && mFsr4RefusedFormat == size.colorFormat && mFsr4RefusedHdr == size.colorHdr);
}

uint64_t LLVKFrameGenFsr::imageBytes(const llfg::GenSize& size) const
{
    // (every format the world image can have is 4 bytes a pixel)
    return llfsrfg::imageBytes(size.renderW ? size.renderW : size.displayW, size.renderH ? size.renderH : size.displayH,
                               size.displayW, size.displayH, 4);
}

// ===========================================================================
// main thread: prepare / release / shutdown
// ===========================================================================

bool LLVKFrameGenFsr::ensurePipelines(VkDeviceContext& dev, std::string& err)
{
    if (mPipelines) return true;
    VkDevice d = dev.device();

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    VK_CHECK(vkCreateSampler(d, &sci, nullptr, &mLinearClamp));

    // SPD: subgroup quad operations in compute, else the LDS-only permutation (as the upscaler's). The optical
    // flow search: full subgroups required where the device can promise them (64 threads a group).
    const VkPhysicalDeviceSubgroupProperties sg = subgroups(dev);
    mWaveOps = (sg.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) && (sg.supportedOperations & VK_SUBGROUP_FEATURE_QUAD_BIT)
            && (sg.supportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT);
    mFullSubgroups = dev.computeFullSubgroups() && sg.subgroupSize && (64u % sg.subgroupSize) == 0u;

    for (uint32_t p = 0; p < P_COUNT; ++p)
    {
        const PassDesc& pd = kPasses[p];
        VkDescriptorSetLayoutBinding b[20]{};
        uint32_t n = 0;
        for (uint32_t i = 0; i < pd.count; ++i, ++n)
        {
            b[n].binding = pd.binds[i].binding;
            b[n].descriptorType = pd.binds[i].type;
            b[n].descriptorCount = 1;
            b[n].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        if (pd.sampler)
        {
            b[n].binding = 1000;
            b[n].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            b[n].descriptorCount = 1;
            b[n].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[n].pImmutableSamplers = &mLinearClamp;
            ++n;
        }
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = n;
        lci.pBindings = b;
        VK_CHECK(vkdCreateSetLayout(d, &lci, &mDsl[p], pd.name));
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &mDsl[p];
        VK_CHECK(vkdCreatePipelineLayout(d, &pli, &mLayout[p], pd.name));
    }

    const auto make = [&](Pass p, uint32_t variant, const uint32_t* code, size_t bytes, VkPipelineShaderStageCreateFlags flags = 0)
    {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = bytes;
        ci.pCode = code;
        VkShaderModule m = VK_NULL_HANDLE;
        VK_CHECK(vkCreateShaderModule(d, &ci, nullptr, &m));
        mPipe[p][variant] = createComputePipeline(d, m, mLayout[p], flags, nullptr, kPasses[p].name);
        vkDestroyShaderModule(d, m, nullptr);
    };
    // (a pass that does not write the output image has one pipeline, under both variants)
    const auto one = [&](Pass p, const uint32_t* code, size_t bytes, VkPipelineShaderStageCreateFlags flags = 0)
    {
        make(p, 0, code, bytes, flags);
        mPipe[p][1] = mPipe[p][0];
    };
    one(P_RECONSTRUCT_DILATE, kFsrFgReconstructDilateSpv, kFsrFgReconstructDilateSpv_bytes);
    one(P_OF_PREPARE_LUMA, kFsrFgOfPrepareLumaSpv, kFsrFgOfPrepareLumaSpv_bytes);
    if (mWaveOps) one(P_OF_LUMA_PYRAMID, kFsrFgOfLuminancePyramidSpv, kFsrFgOfLuminancePyramidSpv_bytes);
    else          one(P_OF_LUMA_PYRAMID, kFsrFgOfLuminancePyramidNoWaveSpv, kFsrFgOfLuminancePyramidNoWaveSpv_bytes);
    one(P_OF_SCD_HISTOGRAM, kFsrFgOfScdHistogramSpv, kFsrFgOfScdHistogramSpv_bytes);
    one(P_OF_SCD_DIVERGENCE, kFsrFgOfScdDivergenceSpv, kFsrFgOfScdDivergenceSpv_bytes);
    one(P_OF_SEARCH, kFsrFgOfSearchSpv, kFsrFgOfSearchSpv_bytes,
        mFullSubgroups ? (VkPipelineShaderStageCreateFlags)VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT : 0u);
    one(P_OF_FILTER, kFsrFgOfFilterSpv, kFsrFgOfFilterSpv_bytes);
    one(P_OF_SCALE, kFsrFgOfScaleSpv, kFsrFgOfScaleSpv_bytes);
    one(P_SETUP, kFsrFgSetupSpv, kFsrFgSetupSpv_bytes);
    one(P_RECONSTRUCT_PREV_DEPTH, kFsrFgReconstructPrevDepthSpv, kFsrFgReconstructPrevDepthSpv_bytes);
    one(P_GAME_VECTOR_FIELD, kFsrFgGameVectorFieldSpv, kFsrFgGameVectorFieldSpv_bytes);
    if (mWaveOps) one(P_GAME_VECTOR_PYRAMID, kFsrFgGameVectorPyramidSpv, kFsrFgGameVectorPyramidSpv_bytes);
    else          one(P_GAME_VECTOR_PYRAMID, kFsrFgGameVectorPyramidNoWaveSpv, kFsrFgGameVectorPyramidNoWaveSpv_bytes);
    one(P_OF_VECTOR_FIELD, kFsrFgOpticalFlowVectorFieldSpv, kFsrFgOpticalFlowVectorFieldSpv_bytes);
    one(P_DISOCCLUSION_MASK, kFsrFgDisocclusionMaskSpv, kFsrFgDisocclusionMaskSpv_bytes);
    make(P_INTERPOLATE, 0, kFsrFgInterpolate10Spv, kFsrFgInterpolate10Spv_bytes);
    make(P_INTERPOLATE, 1, kFsrFgInterpolate8Spv, kFsrFgInterpolate8Spv_bytes);
    if (mWaveOps) one(P_INPAINTING_PYRAMID, kFsrFgInpaintingPyramidSpv, kFsrFgInpaintingPyramidSpv_bytes);
    else          one(P_INPAINTING_PYRAMID, kFsrFgInpaintingPyramidNoWaveSpv, kFsrFgInpaintingPyramidNoWaveSpv_bytes);
    make(P_INPAINTING, 0, kFsrFgInpainting10Spv, kFsrFgInpainting10Spv_bytes);
    make(P_INPAINTING, 1, kFsrFgInpainting8Spv, kFsrFgInpainting8Spv_bytes);
    mPipelines = true;   // (destroyPipelines takes whatever was made)
    for (uint32_t p = 0; p < P_COUNT; ++p)
        for (uint32_t v = 0; v < kVariants; ++v)
            if (!mPipe[p][v])
            {
                err = std::string("the pipeline ") + kPasses[p].name + " could not be made";
                destroyPipelines(dev);
                return false;
            }
    LL_INFOS("Vulkan") << "Frame generation (FSR): pipelines ready (SPD " << (mWaveOps ? "subgroup quad" : "LDS-only")
                       << "; optical flow search subgroups of " << sg.subgroupSize
                       << (mFullSubgroups ? ", full subgroups required" : "") << ")" << LL_ENDL;
    return true;
}

void LLVKFrameGenFsr::destroyPipelines(VkDeviceContext& dev)
{
    VkDevice d = dev.device();
    if (!d) return;
    for (uint32_t p = 0; p < P_COUNT; ++p)
    {
        if (mPipe[p][1] && mPipe[p][1] != mPipe[p][0]) vkDestroyPipeline(d, mPipe[p][1], nullptr);
        if (mPipe[p][0]) vkDestroyPipeline(d, mPipe[p][0], nullptr);
        mPipe[p][0] = mPipe[p][1] = VK_NULL_HANDLE;
        if (mLayout[p]) vkDestroyPipelineLayout(d, mLayout[p], nullptr);
        mLayout[p] = VK_NULL_HANDLE;
        if (mDsl[p]) vkdDestroySetLayout(d, mDsl[p]);
        mDsl[p] = VK_NULL_HANDLE;
    }
    if (mLinearClamp) vkDestroySampler(d, mLinearClamp, nullptr);
    mLinearClamp = VK_NULL_HANDLE;
    mPipelines = false;
}

// The images at exactly the sizes of `size` (FI.cpp's and OF.cpp's resource tables; not made: the inpainting
// mask no pass binds, a distortion field input, the HUD-less pair; the three "shared" images exist once --
// prepare, optical flow and interpolation of a frame are recorded back to back). The second name is the
// GLSL declaration the image is bound to: a storage image's format is its declaration's.
bool LLVKFrameGenFsr::makeResources(VkDeviceContext& dev, const llfg::GenSize& size, std::string& err)
{
    VkMemScope category(VKMEM_TARGET);
    VkDeviceContext::SoftAllocScope soft(&dev);
    const VkImageUsageFlags rwUse = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageUsageFlags roUse = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    bool ok = true;
    const auto mk = [&](GpuImage& im, llfsrfg::Size s, VkFormat format, const char* glslName, uint32_t mips = 1,
                        VkImageUsageFlags usage = 0)
    {
        if (!ok) return;
        im = dev.createImage2D(std::max(s.w, 1u), std::max(s.h, 1u), format, usage ? usage : rwUse, VK_IMAGE_ASPECT_COLOR_BIT, mips);
        if (!im.image || !im.view) { ok = false; return; }
        dev.setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)im.image, (std::string("fsrFg.") + glslName).c_str());
    };
    const llfsrfg::Size render{size.renderW, size.renderH}, display{size.displayW, size.displayH}, one{1, 1};
    const VkFormat worldFormat = size.colorFormat;
    mk(mDilatedDepth,           render, VK_FORMAT_R32_SFLOAT,    "rw_dilated_depth");
    mk(mDilatedMotion,          render, VK_FORMAT_R16G16_SFLOAT, "rw_dilated_motion_vectors");
    mk(mReconPrevDepth,         render, VK_FORMAT_R32_UINT,      "rw_reconstructed_depth_previous_frame");
    mk(mReconDepthInterpolated, render, VK_FORMAT_R32_UINT,      "rw_reconstructed_depth_interpolated_frame");
    mk(mGameFieldX,             render, VK_FORMAT_R32_UINT,      "rw_game_motion_vector_field_x");
    mk(mGameFieldY,             render, VK_FORMAT_R32_UINT,      "rw_game_motion_vector_field_y");
    mk(mOfFieldX,               render, VK_FORMAT_R32_UINT,      "rw_optical_flow_motion_vector_field_x");
    mk(mOfFieldY,               render, VK_FORMAT_R32_UINT,      "rw_optical_flow_motion_vector_field_y");
    mk(mDisocclusion,           render, VK_FORMAT_R8G8_UNORM,    "rw_disocclusion_mask");
    mPyramidMips = llfsrfg::pyramidMips(size.displayW, size.displayH);
    mk(mPyramid, llfsrfg::pyramidSize(size.displayW, size.displayH), VK_FORMAT_R16G16B16A16_SFLOAT, "rw_inpainting_pyramid", mPyramidMips);
    mk(mPrevSource,             display, worldFormat,            "r_previous_interpolation_source", 1, roUse);
    mk(mDistortion,             one,    VK_FORMAT_R8G8_UNORM,    "r_input_distortion_field", 1, roUse);
    mk(mZeroUint,               one,    VK_FORMAT_R32_UINT,      "r_optical_flow_confidence", 1, roUse);
    for (uint32_t i = 0; i < 2; ++i)
        for (uint32_t level = 0; level < kLevels; ++level)
        {
            mk(mOfInput[i][level], llfsrfg::ofLumaSize(size.displayW, size.displayH, level), VK_FORMAT_R8_UINT, "rw_optical_flow_input");
            mk(mOfFlow[i][level], llfsrfg::ofFlowSize(size.displayW, size.displayH, level), VK_FORMAT_R16G16_SINT, "rw_optical_flow");
        }
    mk(mOfResult, llfsrfg::ofFlowSize(size.displayW, size.displayH, 0), VK_FORMAT_R16G16_SINT, "rw_optical_flow");
    mk(mScdOutput,        llfsrfg::Size{llfsrfg::kScdOutputWidth, 1},    VK_FORMAT_R32_UINT,   "rw_optical_flow_scd_output");
    mk(mScdHistogram,     llfsrfg::Size{llfsrfg::kScdHistogramWidth, 1}, VK_FORMAT_R32_UINT,   "rw_optical_flow_scd_histogram");
    mk(mScdPrevHistogram, llfsrfg::Size{llfsrfg::kScdHistogramWidth, 1}, VK_FORMAT_R32_SFLOAT, "rw_optical_flow_scd_previous_histogram");
    mk(mScdTemp,          llfsrfg::Size{llfsrfg::kScdOutputWidth, 1},    VK_FORMAT_R32_UINT,   "rw_optical_flow_scd_temp");
    for (uint32_t m = 0; ok && m < llfsrfg::kPyramidMipBindings; ++m)
    {
        // (13 mip bindings in both SPD passes; the image has fewer: the last repeats, never written twice --
        // SPD writes cbInpaintingPyramid.mips of them)
        mPyramidMip[m] = dev.createViewSoft(mPyramid.image, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_VIEW_TYPE_2D,
                                            VK_IMAGE_ASPECT_COLOR_BIT, std::min(m, mPyramidMips - 1), 1, 0, 1);
        if (!mPyramidMip[m]) ok = false;
    }
    if (ok)
    {
        // One region per present slot; written by the present thread only, read through dynamic offsets.
        mCb = dev.createBuffer((VkDeviceSize)llfsrfg::kCbSlotBytes * kSlots, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        mCounters = dev.createBuffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        ok = mCb.buffer && mCb.mapped && mCounters.buffer;
    }
    if (!ok) err = "not enough video memory for FSR frame generation's images";
    return ok;
}

// The pool and every set: per pass once, or per present slot, frame parity and optical flow level, as the
// pass needs (kPasses).
bool LLVKFrameGenFsr::allocateSets(VkDeviceContext& dev, const llfg::GenSize& size, std::string& err)
{
    std::vector<VkdPoolUse> uses;
    for (uint32_t p = 0; p < P_COUNT; ++p)
    {
        const PassDesc& pd = kPasses[p];
        uses.push_back({mDsl[p], (pd.perSlot ? size.slots : 1u) * (pd.perParity ? 2u : 1u) * (pd.perLevel ? kLevels : 1u)});
    }
    VK_CHECK(mPool.create(dev.device(), "fsrFg", uses));
    bool ok = true;
    for (uint32_t p = 0; ok && p < P_COUNT; ++p)
    {
        const PassDesc& pd = kPasses[p];
        for (uint32_t slot = 0; ok && slot < (pd.perSlot ? size.slots : 1u); ++slot)
            for (uint32_t parity = 0; ok && parity < (pd.perParity ? 2u : 1u); ++parity)
                for (uint32_t level = 0; ok && level < (pd.perLevel ? kLevels : 1u); ++level)
                    VKD_ALLOC_SOFT(mPool.allocate(mDsl[p], &mSet[p][slot][parity][level]), { ok = false; break; });
    }
    if (!ok) err = "no memory for FSR frame generation's descriptor sets";
    return ok;
}

// Every set of every pass, from the images of this size and the present slots' images. The present thread
// is drained and the GPU idle (prepare's contract): nothing uses a set.
void LLVKFrameGenFsr::writeSets(VkDeviceContext& dev, const llfg::GenSize& size)
{
    std::vector<VkWriteDescriptorSet> w;
    std::vector<VkDescriptorImageInfo> ii;
    std::vector<VkDescriptorBufferInfo> bi;
    ii.reserve(2048); bi.reserve(512); w.reserve(2560);   // stable addresses (no reallocation below)
    VkDescriptorSet set = VK_NULL_HANDLE;
    const auto image = [&](uint32_t binding, VkDescriptorType type, VkImageView view)
    {
        ii.push_back({VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL});
        VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        x.dstSet = set; x.dstBinding = binding; x.descriptorCount = 1;
        x.descriptorType = type; x.pImageInfo = &ii.back();
        w.push_back(x);
    };
    const auto s = [&](uint32_t binding, VkImageView view) { image(binding, S, view); };
    const auto u = [&](uint32_t binding, VkImageView view) { image(binding, U, view); };
    const auto buffer = [&](uint32_t binding, VkDescriptorType type, VkBuffer b, VkDeviceSize range)
    {
        bi.push_back({b, 0, range});
        VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        x.dstSet = set; x.dstBinding = binding; x.descriptorCount = 1;
        x.descriptorType = type; x.pBufferInfo = &bi.back();
        w.push_back(x);
    };
    // (a constant block: one 256-byte sub-slot, bound whole; the dynamic offset picks the slot and the block)
    const auto cb = [&](uint32_t binding) { buffer(binding, C, mCb.buffer, llfsrfg::kCbSubSlotBytes); };
    const auto counters = [&](uint32_t binding) { buffer(binding, B, mCounters.buffer, VK_WHOLE_SIZE); };

    for (uint32_t p = 0; p < P_COUNT; ++p)
    {
        const PassDesc& pd = kPasses[p];
        for (uint32_t slot = 0; slot < (pd.perSlot ? size.slots : 1u); ++slot)
            for (uint32_t parity = 0; parity < (pd.perParity ? 2u : 1u); ++parity)
                for (uint32_t level = 0; level < (pd.perLevel ? kLevels : 1u); ++level)
                {
                    set = mSet[p][slot][parity][level];
                    const VkImageView world = size.color[slot].view;
                    const llfsrfg::OfInputs in = llfsrfg::ofInputs(parity);
                    const llfsrfg::OfFlow flow = llfsrfg::ofFlowTargets(parity, level);
                    switch ((Pass)p)
                    {
                    case P_RECONSTRUCT_DILATE:
                        s(0, size.motion[slot].view); s(1, size.depth[slot].view);
                        u(2, mReconPrevDepth.view); u(3, mDilatedMotion.view); u(4, mDilatedDepth.view);
                        cb(5);
                        break;
                    case P_OF_PREPARE_LUMA:
                        s(0, world); u(1, mOfInput[in.current][0].view); cb(2);
                        break;
                    case P_OF_LUMA_PYRAMID:
                        for (uint32_t l = 0; l < kLevels; ++l) u(l, mOfInput[in.current][l].view);
                        cb(7); cb(8);
                        break;
                    case P_OF_SCD_HISTOGRAM:
                        s(0, mOfInput[in.current][0].view); u(1, mScdHistogram.view); cb(2);
                        break;
                    case P_OF_SCD_DIVERGENCE:
                        u(0, mScdHistogram.view); u(1, mScdPrevHistogram.view); u(2, mScdTemp.view); u(3, mScdOutput.view);
                        cb(4);
                        break;
                    case P_OF_SEARCH:   // reads and refines A[level] in place
                        s(0, mOfInput[in.current][level].view); s(1, mOfInput[in.previous][level].view);
                        u(2, mOfFlow[flow.a][level].view); u(3, mScdOutput.view);
                        cb(4);
                        break;
                    case P_OF_FILTER:   // A[level] -> B[level], or the result at level 0
                        s(0, mOfFlow[flow.a][level].view);
                        u(1, flow.filterToShared ? mOfResult.view : mOfFlow[flow.b][level].view);
                        cb(2);
                        break;
                    case P_OF_SCALE:    // B[level] -> B[level - 1] (level 0 has no scale: its set is never bound)
                        if (!flow.scales) continue;
                        s(0, mOfInput[in.current][level].view); s(1, mOfInput[in.previous][level].view);
                        s(2, mOfFlow[flow.b][level].view);
                        u(3, mOfFlow[flow.b][level - 1].view); u(4, mScdOutput.view);
                        cb(5);
                        break;
                    case P_SETUP:
                        s(0, mScdOutput.view);
                        u(1, mGameFieldX.view); u(2, mGameFieldY.view); u(3, mOfFieldX.view); u(4, mOfFieldY.view);
                        u(5, mDisocclusion.view);
                        counters(6); cb(7);
                        break;
                    case P_RECONSTRUCT_PREV_DEPTH:
                        s(0, mDilatedMotion.view); s(1, mDilatedDepth.view); s(2, world); s(3, mDistortion.view);
                        u(4, mReconDepthInterpolated.view);
                        cb(5);
                        break;
                    case P_GAME_VECTOR_FIELD:
                        s(0, mDilatedMotion.view); s(1, mDilatedDepth.view); s(2, mPrevSource.view); s(3, world);
                        s(4, mDistortion.view);
                        u(5, mGameFieldX.view); u(6, mGameFieldY.view);
                        cb(7);
                        break;
                    case P_GAME_VECTOR_PYRAMID:
                        s(0, mGameFieldX.view); s(1, mGameFieldY.view);
                        counters(2);
                        for (uint32_t m = 0; m < llfsrfg::kPyramidMipBindings; ++m) u(3 + m, mPyramidMip[m]);
                        cb(16); cb(17);
                        break;
                    case P_OF_VECTOR_FIELD:   // (binding 1, the optical flow confidence: no pass makes one -- zero)
                        s(0, mOfResult.view); s(1, mZeroUint.view); s(2, mDilatedDepth.view); s(3, mPrevSource.view);
                        s(4, world);
                        u(6, mOfFieldX.view); u(7, mOfFieldY.view);
                        cb(8);
                        break;
                    case P_DISOCCLUSION_MASK:
                        s(0, mGameFieldX.view); s(1, mGameFieldY.view); s(2, mReconPrevDepth.view); s(3, mDilatedDepth.view);
                        s(4, mReconDepthInterpolated.view); s(5, mPyramid.view); s(6, mDistortion.view);
                        u(7, mDisocclusion.view);
                        cb(8);
                        break;
                    case P_INTERPOLATE:
                        s(0, mGameFieldX.view); s(1, mGameFieldY.view); s(2, mOfFieldX.view); s(3, mOfFieldY.view);
                        s(4, mPrevSource.view); s(5, world); s(6, mDisocclusion.view); s(7, mPyramid.view);
                        counters(8);
                        u(9, size.output.view);
                        cb(10);
                        break;
                    case P_INPAINTING_PYRAMID:
                        s(0, size.output.view);
                        counters(1);
                        for (uint32_t m = 0; m < llfsrfg::kPyramidMipBindings; ++m) u(2 + m, mPyramidMip[m]);
                        cb(15); cb(16);
                        break;
                    case P_INPAINTING:   // (the world is both the present backbuffer and the current source: one picture)
                        s(0, mScdOutput.view); s(1, mPyramid.view); s(2, world); s(3, world);
                        u(7, size.output.view);
                        cb(8);
                        break;
                    case P_COUNT:
                        break;
                    }
                }
    }
    vkUpdateDescriptorSets(dev.device(), (uint32_t)w.size(), w.data(), 0, nullptr);
}

bool LLVKFrameGenFsr::prepare(VkDeviceContext& dev, const llfg::GenSize& size, std::string& err)
{
    mReady = false;
    if (!size.displayW || !size.displayH || !size.renderW || !size.renderH)
    {
        err = "the render size is not known";
        return false;
    }
    if (!size.slots || size.slots > kSlots)
    {
        err = "no present slots";
        return false;
    }
    // The images the frames will bring: the sets are written for them now, never on the present thread.
    bool known = hasExtent(size.output, size.displayW, size.displayH) && size.output.format == size.scratchFormat;
    for (uint32_t k = 0; known && k < size.slots; ++k)
        known = hasExtent(size.color[k], size.displayW, size.displayH) && size.color[k].format == size.colorFormat
             && hasExtent(size.depth[k], size.renderW, size.renderH) && hasExtent(size.motion[k], size.renderW, size.renderH);
    if (!known)
    {
        err = "the present slots' images are not known, or not of the stated sizes";
        return false;
    }
    const int32_t variant = outputVariant(size.scratchFormat);
    if (variant < 0)
    {
        err = "FSR cannot write an output image of format " + std::to_string((int)size.scratchFormat);
        return false;
    }
    if (!ensurePipelines(dev, err)) return false;
    if (!makeResources(dev, size, err) || !allocateSets(dev, size, err))
    {
        destroyResources(dev);
        return false;
    }
    mSize = size;
    mVariant = variant;
    writeSets(dev, size);
    mFirst = true;
    mParity = 0;
    mOfFrameIndex = 0;
    mLastSerial = 0;
    mReady = true;
    // #1600: FSR 4 frame generation beside the 3.1 images just made (the GPU is idle: the old context goes).
    mFsr4.destroy();
    mFsr4Now = mFsr4LastRan = false;
    if (wantFsr4(size))
    {
        std::string why;
        // (made for a finite far plane: evaluate clamps an infinite one)
        if (mFsr4.create(llffx::Device{dev.physicalDevice(), dev.device(), dev.fsr4Features()}, size.displayW,
                         size.displayH, size.renderW, size.renderH, size.colorFormat, size.colorHdr, false, why))
        {
            mFsr4Now = true;
            mFsr4Refused = false;
        }
        else
        {
            mFsr4Refused = true;
            mFsr4RefusedDisplay[0] = size.displayW; mFsr4RefusedDisplay[1] = size.displayH;
            mFsr4RefusedRender[0] = size.renderW; mFsr4RefusedRender[1] = size.renderH;
            mFsr4RefusedFormat = size.colorFormat;
            mFsr4RefusedHdr = size.colorHdr;
            LL_INFOS("Vulkan") << "FSR 4 frame generation is not used, FSR 3.1 instead: " << why << LL_ENDL;
        }
    }
    LL_INFOS("Vulkan") << "Frame generation (FSR): images " << size.renderW << "x" << size.renderH << " -> " << size.displayW
                       << "x" << size.displayH << ", " << (imageBytes(size) / 1000000ull) << " MB, output "
                       << (mVariant == 0 ? "rgb10_a2" : "rgba8") << ", " << size.slots << " slots" << LL_ENDL;
    return true;
}

void LLVKFrameGenFsr::destroyResources(VkDeviceContext& dev)
{
    for (VkImageView& v : mPyramidMip)
        if (v) dev.destroyView(v);
    GpuImage* const images[] = {&mDilatedDepth, &mDilatedMotion, &mReconPrevDepth, &mReconDepthInterpolated, &mGameFieldX,
                                &mGameFieldY, &mOfFieldX, &mOfFieldY, &mDisocclusion, &mPyramid, &mPrevSource, &mDistortion,
                                &mZeroUint, &mOfResult, &mScdOutput, &mScdHistogram, &mScdPrevHistogram, &mScdTemp};
    for (GpuImage* im : images)
        if (im->image) { dev.destroyImage(*im); *im = GpuImage{}; }
    for (uint32_t i = 0; i < 2; ++i)
        for (uint32_t level = 0; level < kLevels; ++level)
            for (GpuImage* im : {&mOfInput[i][level], &mOfFlow[i][level]})
                if (im->image) { dev.destroyImage(*im); *im = GpuImage{}; }
    if (mCb.buffer) dev.destroyBuffer(mCb);
    if (mCounters.buffer) dev.destroyBuffer(mCounters);
    mCb = GpuBuffer{};
    mCounters = GpuBuffer{};
    mPool.destroy();
    std::memset(mSet, 0, sizeof(mSet));
    mPyramidMips = 0;
    mReady = false;
    mVariant = -1;
    mSize = llfg::GenSize{};
}

void LLVKFrameGenFsr::release(VkDeviceContext& dev)
{
    mFsr4.destroy();   // #1600 (the GPU is idle)
    mFsr4Now = mFsr4LastRan = false;
    destroyResources(dev);
}

void LLVKFrameGenFsr::shutdown(VkDeviceContext& dev)
{
    mFsr4.destroy();
    mFsr4Now = mFsr4LastRan = false;
    destroyResources(dev);
    destroyPipelines(dev);
}

// ===========================================================================
// present thread: evaluate
// ===========================================================================

VkDescriptorSet LLVKFrameGenFsr::setOf(Pass pass, uint32_t slot, uint32_t parity, uint32_t level) const
{
    const PassDesc& pd = kPasses[pass];
    return mSet[pass][pd.perSlot ? slot : 0][pd.perParity ? (parity & 1u) : 0][pd.perLevel ? level : 0];
}

// Between two steps: every write so far (compute, clear, copy) -> the next step's reads and writes. One
// memory barrier, as the upscaler's passes: all the images stay in GENERAL. It replaces the SDK backend's
// per-resource state transitions; the optical flow levels read and write neighbouring levels of the same
// pyramids, so no step goes without it.
void LLVKFrameGenFsr::barrier(VkCommandBuffer cmd) const
{
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COPY_BIT;
    m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    m.dstStageMask = m.srcStageMask;
    m.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                    | VK_ACCESS_2_UNIFORM_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void LLVKFrameGenFsr::dispatch(VkCommandBuffer cmd, const Frame& f, Pass pass, uint32_t level, const llfsrfg::Groups& groups) const
{
    // Dynamic offsets: one per uniform buffer binding, in binding order -- this slot's region, the block.
    const PassDesc& pd = kPasses[pass];
    uint32_t dyn[2];
    uint32_t nd = 0;
    for (uint32_t i = 0; i < pd.count; ++i)
    {
        uint32_t block = 0;
        switch (pd.binds[i].cb)
        {
        case CB_NONE:           continue;
        case CB_FI:             block = llfsrfg::kCbFi; break;
        case CB_PYRAMID_GAME:   block = llfsrfg::kCbPyramidGame; break;
        case CB_PYRAMID_OUTPUT: block = llfsrfg::kCbPyramidOutput; break;
        case CB_OF:             block = llfsrfg::kCbOf + (pd.perLevel ? level : 0u) * llfsrfg::kCbSubSlotBytes; break;
        case CB_OF_SPD:         block = llfsrfg::kCbOfSpd; break;
        }
        dyn[nd++] = f.cbOffset + block;
    }
    const VkDescriptorSet set = setOf(pass, f.slot, f.parity, level);
    // (no vkBreadcrumb here: its marker ring and sequence are the main thread's, and this is the present thread)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe[pass][mVariant]);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout[pass], 0, 1, &set, nd, dyn);
    vkCmdDispatch(cmd, groups.x, groups.y, groups.z);
}

// This frame's constant blocks into this present slot's region of the ring (its last use has run: the slot
// was free). Every block is one a tested filler returned.
void LLVKFrameGenFsr::stageConstants(const Frame& f, const llfg::GenInputs& in)
{
    uint8_t* const base = (uint8_t*)mCb.mapped + f.cbOffset;
    const uint32_t rw = mSize.renderW, rh = mSize.renderH, dw = mSize.displayW, dh = mSize.displayH;
    const llfsrfg::FiConstants fi = llfsrfg::fillFiConstants(in, f.reset);
    std::memcpy(base + llfsrfg::kCbFi, &fi, sizeof(fi));
    const llfsrfg::PyramidConstants game = llfsrfg::fillPyramidConstants(Step::GameVectorPyramid, rw, rh, dw, dh);
    std::memcpy(base + llfsrfg::kCbPyramidGame, &game, sizeof(game));
    const llfsrfg::PyramidConstants output = llfsrfg::fillPyramidConstants(Step::InpaintingPyramid, rw, rh, dw, dh);
    std::memcpy(base + llfsrfg::kCbPyramidOutput, &output, sizeof(output));
    const llfsrfg::OfSpdConstants spd = llfsrfg::fillOfSpdConstants(dw, dh);
    std::memcpy(base + llfsrfg::kCbOfSpd, &spd, sizeof(spd));
    for (uint32_t level = 0; level < kLevels; ++level)
    {
        const llfsrfg::OfConstants of = llfsrfg::fillOfConstants(in, level, mOfFrameIndex);
        std::memcpy(base + llfsrfg::kCbOf + level * llfsrfg::kCbSubSlotBytes, &of, sizeof(of));
    }
}

// A reset's first step: every image of the generator defined (GENERAL, whatever it was -- fresh, or left by
// commands that never ran) and zero, the counters zero. Zero is far for the reversed-Z depths and "nothing"
// for the vector fields, the histograms and the pyramids.
void LLVKFrameGenFsr::recordClearAll(VkCommandBuffer cmd)
{
    VkImage images[20 + 4 * kLevels];
    uint32_t n = 0;
    for (const GpuImage* im : {&mDilatedDepth, &mDilatedMotion, &mReconPrevDepth, &mReconDepthInterpolated, &mGameFieldX,
                               &mGameFieldY, &mOfFieldX, &mOfFieldY, &mDisocclusion, &mPyramid, &mPrevSource, &mDistortion,
                               &mZeroUint, &mOfResult, &mScdOutput, &mScdHistogram, &mScdPrevHistogram, &mScdTemp})
        images[n++] = im->image;
    for (uint32_t i = 0; i < 2; ++i)
        for (uint32_t level = 0; level < kLevels; ++level)
        {
            images[n++] = mOfInput[i][level].image;
            images[n++] = mOfFlow[i][level].image;
        }
    VkImageMemoryBarrier2 b[20 + 4 * kLevels];
    const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1};
    for (uint32_t i = 0; i < n; ++i)
    {
        b[i] = VkImageMemoryBarrier2{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b[i].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COPY_BIT;
        b[i].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b[i].dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        b[i].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b[i].srcQueueFamilyIndex = b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[i].image = images[i];
        b[i].subresourceRange = all;
    }
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = n;
    dep.pImageMemoryBarriers = b;
    vkCmdPipelineBarrier2(cmd, &dep);
    const VkClearColorValue zero{};   // (all-zero bits: the UINT / SINT images too)
    for (uint32_t i = 0; i < n; ++i) vkCmdClearColorImage(cmd, images[i], VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
    vkCmdFillBuffer(cmd, mCounters.buffer, 0, VK_WHOLE_SIZE, 0);
}

void LLVKFrameGenFsr::recordStep(VkCommandBuffer cmd, const Frame& f, const llfsrfg::PlanStep& step)
{
    barrier(cmd);
    const VkClearColorValue zero{};   // (inverted depth: far = 0)
    const VkImageSubresourceRange one{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    switch (step.step)
    {
    case Step::ClearAll:
        recordClearAll(cmd);
        return;
    case Step::ClearReconstructedPrevDepth:
        vkCmdClearColorImage(cmd, mReconPrevDepth.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &one);
        return;
    case Step::ClearReconstructedDepthInterpolated:
        vkCmdClearColorImage(cmd, mReconDepthInterpolated.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &one);
        return;
    case Step::CopyPreviousSource:
    {
        // The world image of this frame is the next frame's previous picture (same format, same size).
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = region.srcSubresource;
        region.extent = {mSize.displayW, mSize.displayH, 1};
        vkCmdCopyImage(cmd, f.color, VK_IMAGE_LAYOUT_GENERAL, mPrevSource.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        return;
    }
    default:
        break;
    }
    const Pass pass = passOf(step.step);
    if (pass == P_COUNT) return;
    dispatch(cmd, f, pass, step.level,
             llfsrfg::dispatchGroups(step.step, step.level, mSize.renderW, mSize.renderH, mSize.displayW, mSize.displayH));
}

// A. ffxFrameInterpolationPrepare: the depth and motion vectors of the frame, reconstructed and dilated.
void LLVKFrameGenFsr::recordPrepare(VkCommandBuffer cmd, const Frame& f, const llfsrfg::Plan& plan)
{
    for (uint32_t i = 0; i < plan.count; ++i)
        if (llfsrfg::stageOf(plan.steps[i].step) == llfsrfg::Stage::Prepare) recordStep(cmd, f, plan.steps[i]);
}

// B. the optical flow's dispatch: the world image's luminance pyramid, the scene change detection, the search.
void LLVKFrameGenFsr::recordOpticalFlow(VkCommandBuffer cmd, const Frame& f, const llfsrfg::Plan& plan)
{
    for (uint32_t i = 0; i < plan.count; ++i)
        if (llfsrfg::stageOf(plan.steps[i].step) == llfsrfg::Stage::OpticalFlow) recordStep(cmd, f, plan.steps[i]);
}

// C. ffxFrameInterpolationDispatch: the vector fields, the half-way frame, its inpainting, the kept picture.
void LLVKFrameGenFsr::recordInterpolation(VkCommandBuffer cmd, const Frame& f, const llfsrfg::Plan& plan)
{
    for (uint32_t i = 0; i < plan.count; ++i)
        if (llfsrfg::stageOf(plan.steps[i].step) == llfsrfg::Stage::Interpolation) recordStep(cmd, f, plan.steps[i]);
}

bool LLVKFrameGenFsr::evaluate(VkCommandBuffer cmd, uint32_t slot, const llfg::GenInputs& in, uint32_t index, uint32_t count,
                               std::string& err)
{
    if (!mReady || mVariant < 0)
    {
        err = "the FSR feature is not made";
        return false;
    }
    if (index != 1 || count != 1)
    {
        err = "FSR generates one frame per rendered frame";
        return false;
    }
    // The sets were written in prepare for these very images, at these sizes: another image here would be
    // read as a ghost of a frame, another size past an image's edge.
    if (slot >= mSize.slots || !sameRef(in.color, mSize.color[slot]) || !sameRef(in.depth, mSize.depth[slot])
        || !sameRef(in.motion, mSize.motion[slot]) || !sameRef(in.output, mSize.output))
    {
        err = "the frame's images are not the ones the FSR feature was made for";
        return false;
    }
    if (in.camera.renderW != mSize.renderW || in.camera.renderH != mSize.renderH)
    {
        err = "the frame's render size is not the one the FSR feature was made for";
        return false;
    }
    // #1600 FSR 4: on a refusal, this frame (and every one until the next prepare) with 3.1, which restarts:
    // its images were not kept up while FSR 4 ran.
    if (mFsr4Now)
    {
        if (evaluateFsr4(cmd, in, err))
        {
            mFsr4LastRan = true;
            mLastSerial = in.serial;
            return true;
        }
        LL_WARNS("Vulkan") << "FSR 4 frame generation refused, FSR 3.1 until the sizes change: " << err << LL_ENDL;
        err.clear();
        mFsr4Now = false;
    }
    if (mFsr4LastRan) mFirst = true;
    mFsr4LastRan = false;
    Frame f;
    f.slot = slot;
    f.parity = mParity;
    f.reset = llfsrfg::resetFor(in.reset, in.serial, mLastSerial, mFirst);
    f.cbOffset = slot * llfsrfg::kCbSlotBytes;
    f.color = in.color.image;
    mOfFrameIndex = llfsrfg::ofFrameIndex(f.reset, mOfFrameIndex);
    stageConstants(f, in);
    const llfsrfg::Plan plan = llfsrfg::passPlan(f.reset);
    recordPrepare(cmd, f, plan);
    recordOpticalFlow(cmd, f, plan);
    recordInterpolation(cmd, f, plan);
    barrier(cmd);   // (the last writes, for whoever records next on this queue)
    // What the next frame is recorded with: it assumes these commands run (else the caller resets it).
    mFirst = false;
    mParity ^= 1u;
    mLastSerial = in.serial;
    return true;
}

// #1600: one generated frame from FSR Frame Generation 4.x (llcleanffxvk.h), from the very images the 3.1
// passes read, in GENERAL. The camera in the renderer's terms (llfg::GenCamera) turned into the SDK's: near /
// far / field of view from the projection, the world-space camera from the inverse view (view space looks down
// -Z), the jitter as rendered (FSR's own convention, as 3.1's), motion in UV units x the render size.
bool LLVKFrameGenFsr::evaluateFsr4(VkCommandBuffer cmd, const llfg::GenInputs& in, std::string& err)
{
    auto image = [](const llfg::GpuImageRef& r, bool write) {
        return llffx::Image{r.image, r.format, r.extent.width, r.extent.height, VK_IMAGE_LAYOUT_GENERAL, write};
    };
    llffx::FrameGen::Inputs g;
    g.color = image(in.color, false);
    g.depth = image(in.depth, false);
    g.motion = image(in.motion, false);
    g.output = image(in.output, true);
    g.renderW = in.camera.renderW;
    g.renderH = in.camera.renderH;
    float invProj[16] = {};
    (void)llfsr::invert4(in.camera.proj, invProj);   // (singular: zeros -> the guarded defaults)
    g.camera = llffx::cameraFromProjection(in.camera.proj, invProj, in.camera.jitterPx);
    if (g.camera.infiniteFar)
    {
        // (the context is made for a finite far plane; an infinite one is far enough at a million metres)
        g.camera.infiniteFar = false;
        g.camera.farDist = 1.0e6f;
    }
    float toWorld[16] = {};
    if (llfsr::invert4(in.camera.view, toWorld))
    {
        for (int i = 0; i < 3; ++i)
        {
            g.camRight[i] = toWorld[0 * 4 + i];
            g.camUp[i] = toWorld[1 * 4 + i];
            g.camFwd[i] = -toWorld[2 * 4 + i];
            g.camPos[i] = toWorld[3 * 4 + i];
        }
    }
    g.frameMs = in.frameMs > 0.f ? in.frameMs : 16.7f;
    g.peakNits = in.peakNits;
    g.hdr = in.colorHdr;
    g.frameId = in.serial;
    g.reset = in.reset || in.camera.reset || !mFsr4LastRan;
    return mFsr4.dispatch(cmd, g, err);
}
